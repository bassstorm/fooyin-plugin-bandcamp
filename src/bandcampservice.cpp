#include "bandcampservice.h"

#include "bandcamplogging.h"
#include "bandcampparser.h"
#include "bandcamppostprocessor.h"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr int DefaultItemsInProgress = 2;
constexpr int MinItemsInProgress     = 1;
constexpr int MaxItemsInProgress     = 8;

constexpr int DefaultItemTimeoutMs = 10000;
// Internal floor and ceiling in milliseconds. The user-facing setting is limited to 1-120 s separately.
constexpr int MinItemTimeoutMs = 1;
constexpr int MaxItemTimeoutMs = 120000;

BandcampResolvedPage parsePage(const BandcampPageUrl& page, const QByteArray& body)
{
    BandcampResolvedPage result;
    result.page = page;
    const QUrl pageUrl(page.url);

    if(page.kind == BandcampPageKind::Album) {
        auto parsed     = parseAlbumPage(body, pageUrl);
        result.error    = parsed.error;
        result.warnings = parsed.warnings;
        result.album    = std::move(parsed.value);
        if(result.error.isEmpty()) {
            postProcessAlbum(result.album, result.warnings);
        }
    }
    else {
        auto parsed     = parseTrackPage(body, pageUrl);
        result.error    = parsed.error;
        result.warnings = parsed.warnings;
        result.track    = std::move(parsed.value);
        if(result.error.isEmpty()) {
            postProcessTrack(result.track, result.warnings);
        }
    }

    return result;
}
} // namespace

BandcampService::BandcampService(QNetworkAccessManager* network, QObject* parent)
    : QObject(parent)
    , m_client{new BandcampClient(network, this)}
    , m_maxItemsInProgress{DefaultItemsInProgress}
    , m_itemTimeoutMs{DefaultItemTimeoutMs}
{ }

BandcampService::~BandcampService()
{
    // A parse task cannot be cancelled, and it reads memory owned by its future, so wait for it to end.
    for(auto parse : std::as_const(m_parses)) {
        parse.waitForFinished();
    }
}

void BandcampService::setMaxItemsInProgress(int count)
{
    m_maxItemsInProgress = std::clamp(count, MinItemsInProgress, MaxItemsInProgress);
    startNextJobs();
}

void BandcampService::setItemTimeoutMs(int timeoutMs)
{
    m_itemTimeoutMs = std::clamp(timeoutMs, MinItemTimeoutMs, MaxItemTimeoutMs);
}

void BandcampService::resolve(const QList<BandcampPageUrl>& urls)
{
    if(urls.isEmpty()) {
        return;
    }

    const quint64 batchId = m_nextBatchId++;

    Batch batch;
    batch.remaining = static_cast<int>(urls.size());
    batch.results.resize(urls.size());
    for(qsizetype i = 0; i < urls.size(); ++i) {
        batch.results[i].page = urls.at(i);
    }
    m_batches.insert(batchId, std::move(batch));

    for(qsizetype i = 0; i < urls.size(); ++i) {
        m_queued.append(Job{m_nextJobId++, batchId, static_cast<int>(i), urls.at(i)});
    }

    startNextJobs();
}

void BandcampService::startNextJobs()
{
    while(!m_queued.isEmpty() && static_cast<int>(m_active.size()) < m_maxItemsInProgress) {
        const Job job = m_queued.takeFirst();

        // Discography expansion arrives in Step 3. Until then it is reported, not fetched.
        if(job.page.kind == BandcampPageKind::Discography) {
            qCInfo(BANDCAMP_RESOLVE) << "Discography resolution is not supported yet, skipping" << job.page.url;
            BandcampResolvedPage result;
            result.page  = job.page;
            result.error = tr("Discography resolution is not supported yet");
            recordResult(job, std::move(result));
            continue;
        }

        startJob(job);
    }
}

void BandcampService::startJob(const Job& job)
{
    qCInfo(BANDCAMP_RESOLVE) << "Resolving" << pageKindName(job.page.kind) << job.page.url;

    ActiveJob active;
    active.job     = job;
    active.timeout = new QTimer(this);
    active.timeout->setSingleShot(true);
    active.timeout->setInterval(m_itemTimeoutMs);
    connect(active.timeout, &QTimer::timeout, this, [this, jobId = job.jobId]() { onTimedOut(jobId); });
    active.timeout->start();
    m_active.insert(job.jobId, active);

    startFetch(job.jobId, job.page.url);
}

void BandcampService::startFetch(quint64 jobId, const QString& url)
{
    // The client never calls back synchronously, so the job is still active when its request id is stored.
    const BandcampClient::RequestId request = m_client->fetch(
        QUrl(url), [this, jobId](BandcampFetchResponse response) { onFetched(jobId, std::move(response)); },
        CorrelationContext{.jobId = static_cast<qint64>(jobId), .resourceUrl = QUrl(url)});

    if(auto it = m_active.find(jobId); it != m_active.end()) {
        it->request = request;
    }
}

void BandcampService::onFetched(quint64 jobId, BandcampFetchResponse response)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }

    it->request = 0;

    if(!response.isValid()) {
        if(it->awaitingAlbum) {
            // The track itself is still valid, so only its album details are lost.
            BandcampResolvedPage result = std::move(it->partial);
            result.warnings.append(tr("Album page could not be fetched, album artist and track total are unknown: %1")
                                       .arg(response.error));
            finishJob(jobId, std::move(result));
            return;
        }

        BandcampResolvedPage result;
        result.page  = it->job.page;
        result.error = response.error;
        finishJob(jobId, std::move(result));
        return;
    }

    const BandcampPageUrl page = it->awaitingAlbum ? it->albumPage : it->job.page;
    startParse(jobId, page, std::move(response.body));
}

void BandcampService::startParse(quint64 jobId, const BandcampPageUrl& page, QByteArray body)
{
    // Parsing runs on the thread pool, so the UI thread never waits on it.
    QFuture<BandcampResolvedPage> future
        = QtConcurrent::run([page, body = std::move(body)]() { return parsePage(page, body); });
    m_parses.insert(jobId, future);

    auto* watcher = new QFutureWatcher<BandcampResolvedPage>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, jobId, watcher]() {
        BandcampResolvedPage result = watcher->result();
        watcher->deleteLater();
        m_parses.remove(jobId);
        // Dropped by onParsed if the item has already timed out.
        onParsed(jobId, std::move(result));
    });
    watcher->setFuture(future);
}

void BandcampService::onParsed(quint64 jobId, BandcampResolvedPage result)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }

    if(it->awaitingAlbum) {
        BandcampResolvedPage track = std::move(it->partial);
        if(result.isValid()) {
            postProcessAlbumTrack(track.track, result.album, track.warnings);
        }
        else {
            track.warnings.append(tr("Album page could not be parsed, album artist, release date and track total are "
                                     "unknown: %1")
                                      .arg(result.error));
        }
        finishJob(jobId, std::move(track));
        return;
    }

    if(result.isValid() && result.page.kind == BandcampPageKind::Track && !result.track.sourceUrl.isEmpty()) {
        const BandcampPageUrl albumPage = parseBandcampPageUrl(result.track.sourceUrl);
        if(albumPage.isValid() && albumPage.kind == BandcampPageKind::Album) {
            qCDebug(BANDCAMP_RESOLVE) << "Fetching album for track" << result.page.url << albumPage.url;
            it->awaitingAlbum = true;
            it->albumPage     = albumPage;
            it->partial       = std::move(result);
            startFetch(jobId, albumPage.url);
            return;
        }
    }

    finishJob(jobId, std::move(result));
}

void BandcampService::onTimedOut(quint64 jobId)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }

    const BandcampPageUrl page = it->job.page;
    qCWarning(BANDCAMP_RESOLVE) << "Timed out after" << m_itemTimeoutMs << "ms:" << page.url;

    if(it->awaitingAlbum) {
        BandcampResolvedPage result = std::move(it->partial);
        result.warnings.append(tr("Album page timed out, album artist and track total are unknown"));
        finishJob(jobId, std::move(result));
        return;
    }

    BandcampResolvedPage result;
    result.page  = page;
    result.error = tr("Timed out after %1 s").arg(QString::number(m_itemTimeoutMs / 1000.0));
    finishJob(jobId, std::move(result));
}

void BandcampService::finishJob(quint64 jobId, BandcampResolvedPage result)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }

    ActiveJob active = std::move(it.value());
    m_active.erase(it);

    if(active.request != 0) {
        m_client->cancel(active.request);
    }
    active.timeout->stop();
    active.timeout->deleteLater();

    // A track page without a stream cannot be played, and the processors cannot fail an item.
    if(result.isValid() && result.page.kind == BandcampPageKind::Track && result.track.streamUrl.isEmpty()) {
        result.error = tr("track has no usable stream");
        qCWarning(BANDCAMP_RESOLVE) << "Track rejected:" << result.page.url << result.error;
    }

    recordResult(active.job, std::move(result));
    startNextJobs();
}

void BandcampService::recordResult(const Job& job, BandcampResolvedPage result)
{
    auto it = m_batches.find(job.batchId);
    if(it == m_batches.end()) {
        return;
    }

    it->results[job.index] = std::move(result);
    if(--it->remaining > 0) {
        return;
    }

    QList<BandcampResolvedPage> results = std::move(it->results);
    m_batches.erase(it);
    emit batchResolved(results);
}
} // namespace Fooyin::Bandcamp
