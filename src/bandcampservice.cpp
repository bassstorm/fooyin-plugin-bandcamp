#include "bandcampservice.h"

#include "bandcamplogging.h"
#include "bandcamppostprocessor.h"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr int DefaultItemsInProgress      = 2;
constexpr int MinItemsInProgress          = 1;
constexpr int MaxItemsInProgress          = 8;
constexpr int ServiceDefaultItemTimeoutMs = 10000;
constexpr int ServiceMinItemTimeoutMs     = 1;
constexpr int ServiceMaxItemTimeoutMs     = 120000;

ResolutionError operationFailure(ResolutionErrorCode code, ResolutionStage stage, const QUrl& resource, qint64 trackId,
                                 const QString& field, const QString& reason)
{
    return {code, stage, resource, trackId, field, reason};
}

ResolutionError parentFailure(const ResolutionError& cause, const QUrl& parentUrl, qint64 trackId,
                              ResolutionStage stage = ResolutionStage::Acquire)
{
    return operationFailure(ResolutionErrorCode::ParentUnavailable, stage, parentUrl, trackId,
                            cause.field.isEmpty() ? QStringLiteral("parent") : cause.field,
                            QStringLiteral("required album parent failed: %1").arg(cause.reason));
}

void processAlbum(AlbumAdmission& admission)
{
    postProcessAlbum(admission);
}

void processTrack(TrackAdmission& admission)
{
    postProcessTrack(admission);
}
} // namespace

BandcampService::BandcampService(QNetworkAccessManager* network, QObject* parent)
    : QObject{parent}
    , m_client{new BandcampClient(network, this)}
    , m_maxItemsInProgress{DefaultItemsInProgress}
    , m_itemTimeoutMs{ServiceDefaultItemTimeoutMs}
{ }

BandcampService::~BandcampService()
{
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
    m_itemTimeoutMs = std::clamp(timeoutMs, ServiceMinItemTimeoutMs, ServiceMaxItemTimeoutMs);
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
        if(job.page.kind == BandcampPageKind::Discography) {
            recordResult(job,
                         ResolutionFailure{operationFailure(
                             ResolutionErrorCode::UnsupportedRequest, ResolutionStage::Acquire, QUrl(job.page.url), 0,
                             QStringLiteral("resource"), QStringLiteral("discography expansion is not implemented"))});
            continue;
        }
        startJob(job);
    }
}

void BandcampService::startJob(const Job& job)
{
    ActiveJob active;
    active.job     = job;
    active.timeout = new QTimer(this);
    active.timeout->setSingleShot(true);
    active.timeout->setInterval(m_itemTimeoutMs);
    connect(active.timeout, &QTimer::timeout, this, [this, jobId = job.jobId]() { onTimedOut(jobId); });
    active.timeout->start();
    active.correlation = {.jobId = static_cast<qint64>(job.jobId), .resourceUrl = QUrl(job.page.url)};
    m_active.insert(job.jobId, std::move(active));
    startFetch(job.jobId, job.page.url);
}

void BandcampService::startFetch(quint64 jobId, const QString& url)
{
    const auto active = m_active.constFind(jobId);
    const qint64 trackId
        = active != m_active.cend() && active->trackCandidate && active->trackCandidate->track.id.isUsable()
            ? active->trackCandidate->track.id.value
            : 0;
    const BandcampClient::RequestId request = m_client->fetch(
        QUrl(url), [this, jobId](BandcampFetchOutcome response) { onFetched(jobId, std::move(response)); },
        CorrelationContext{.jobId = static_cast<qint64>(jobId), .resourceUrl = QUrl(url), .trackId = trackId});

    if(auto it = m_active.find(jobId); it != m_active.end()) {
        it->request = request;
    }
}

void BandcampService::onFetched(quint64 jobId, BandcampFetchOutcome response)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }

    const auto requestId   = it->request;
    it->request            = 0;
    const QUrl resourceUrl = it->awaitingAlbum ? QUrl(it->albumPage.url) : QUrl(it->job.page.url);
    if(const auto* failed = std::get_if<ResolutionFailure>(&response)) {
        if(it->awaitingAlbum) {
            const qint64 trackId = it->trackCandidate && it->trackCandidate->track.id.isUsable()
                                     ? it->trackCandidate->track.id.value
                                     : 0;
            finishFailure(jobId, parentFailure(failed->error, resourceUrl, trackId));
        }
        else {
            finishFailure(jobId, failed->error);
        }
        return;
    }
    if(std::holds_alternative<ResolutionCancelled>(response)) {
        finishJob(jobId, ResolutionCancelled{});
        return;
    }

    const auto& fetched        = std::get<ResolutionSuccess<BandcampFetchResponse>>(response).value;
    it->correlation            = CorrelationContext{.jobId       = static_cast<qint64>(jobId),
                                                    .requestId   = static_cast<qint64>(requestId),
                                                    .resourceUrl = fetched.url,
                                                    .trackId     = it->trackCandidate && it->trackCandidate->track.id.isUsable()
                                                                     ? it->trackCandidate->track.id.value
                                                                     : 0};
    const BandcampPageUrl page = it->awaitingAlbum ? it->albumPage : it->job.page;
    startParse(jobId, page, fetched.body, it->correlation);
}

void BandcampService::startParse(quint64 jobId, const BandcampPageUrl& page, QByteArray body,
                                 CorrelationContext context)
{
    QFuture<ReleaseParseOutcome> future = QtConcurrent::run([page, body = std::move(body), context]() {
        const QUrl effectiveUrl = context.resourceUrl.isValid() ? context.resourceUrl : QUrl(page.url);
        return parseReleasePage(body, effectiveUrl, context);
    });
    m_parses.insert(jobId, future);

    auto* watcher = new QFutureWatcher<ReleaseParseOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, jobId, watcher]() {
        ReleaseParseOutcome result = watcher->result();
        watcher->deleteLater();
        m_parses.remove(jobId);
        onParsed(jobId, std::move(result));
    });
    watcher->setFuture(future);
}

void BandcampService::onParsed(quint64 jobId, ReleaseParseOutcome result)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }
    if(const auto* failed = std::get_if<ResolutionFailure>(&result)) {
        if(it->awaitingAlbum) {
            const qint64 trackId = it->trackCandidate && it->trackCandidate->track.id.isUsable()
                                     ? it->trackCandidate->track.id.value
                                     : 0;
            finishFailure(jobId,
                          parentFailure(failed->error, QUrl(it->albumPage.url), trackId, ResolutionStage::Parse));
        }
        else {
            finishFailure(jobId, failed->error);
        }
        return;
    }
    if(std::holds_alternative<ResolutionCancelled>(result)) {
        finishJob(jobId, ResolutionCancelled{});
        return;
    }

    ReleaseCandidate candidate = std::get<ResolutionSuccess<ReleaseCandidate>>(std::move(result)).value;
    if(it->awaitingAlbum) {
        auto* album = std::get_if<AlbumCandidate>(&candidate);
        const qint64 trackId
            = it->trackCandidate && it->trackCandidate->track.id.isUsable() ? it->trackCandidate->track.id.value : 0;
        if(!album || !it->trackCandidate) {
            finishFailure(jobId, operationFailure(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Parse,
                                                  QUrl(it->albumPage.url), trackId, QStringLiteral("parent.resource"),
                                                  QStringLiteral("parent response did not identify an album")));
            return;
        }
        auto admission = admitAlbumTrack(*it->trackCandidate, *album, it->correlation);
        if(const auto* admitted = std::get_if<ResolutionSuccess<TrackAdmission>>(&admission)) {
            TrackAdmission refined = admitted->value;
            processTrack(refined);
            finishJob(jobId, ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.result)}});
        }
        else if(const auto* failed = std::get_if<ResolutionFailure>(&admission)) {
            finishFailure(jobId, failed->error);
        }
        else {
            finishJob(jobId, ResolutionCancelled{});
        }
        return;
    }

    if(it->job.page.kind == BandcampPageKind::Album) {
        auto* album = std::get_if<AlbumCandidate>(&candidate);
        if(!album) {
            finishFailure(jobId, operationFailure(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                  it->correlation.resourceUrl, 0, QStringLiteral("identity"),
                                                  QStringLiteral("parsed resource is not an album")));
            return;
        }
        auto admission = admitAlbum(*album, it->correlation);
        if(const auto* admitted = std::get_if<ResolutionSuccess<AlbumAdmission>>(&admission)) {
            AlbumAdmission refined = admitted->value;
            processAlbum(refined);
            finishJob(jobId, ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.album)}});
        }
        else if(const auto* failed = std::get_if<ResolutionFailure>(&admission)) {
            finishFailure(jobId, failed->error);
        }
        else {
            finishJob(jobId, ResolutionCancelled{});
        }
        return;
    }

    auto* track = std::get_if<TrackPageCandidate>(&candidate);
    if(!track) {
        finishFailure(jobId, operationFailure(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                              it->correlation.resourceUrl, 0, QStringLiteral("identity"),
                                              QStringLiteral("parsed resource is not a track")));
        return;
    }
    const ParentRequirement requirement = classifyParentEvidence(*track, it->correlation);
    if(const auto* failed = std::get_if<ResolutionFailure>(&requirement)) {
        finishFailure(jobId, failed->error);
        return;
    }
    if(std::holds_alternative<StandaloneParentEvidence>(requirement)) {
        auto admission = admitStandaloneTrack(*track, it->correlation);
        if(const auto* admitted = std::get_if<ResolutionSuccess<TrackAdmission>>(&admission)) {
            TrackAdmission refined = admitted->value;
            processTrack(refined);
            finishJob(jobId, ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.result)}});
        }
        else if(const auto* failed = std::get_if<ResolutionFailure>(&admission)) {
            finishFailure(jobId, failed->error);
        }
        else {
            finishJob(jobId, ResolutionCancelled{});
        }
        return;
    }

    const RequiredAlbumParent parent = std::get<RequiredAlbumParent>(requirement);
    const BandcampPageUrl albumPage  = parseBandcampPageUrl(parent.url.toString(QUrl::FullyEncoded));
    if(!albumPage.isValid() || albumPage.kind != BandcampPageKind::Album) {
        finishFailure(jobId, operationFailure(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire,
                                              parent.url, track->track.id.value, QStringLiteral("parent.albumUrl"),
                                              QStringLiteral("required parent location is not an album URL")));
        return;
    }
    it->awaitingAlbum  = true;
    it->albumPage      = albumPage;
    it->trackCandidate = *track;
    startFetch(jobId, albumPage.url);
}

void BandcampService::onTimedOut(quint64 jobId)
{
    auto it = m_active.find(jobId);
    if(it == m_active.end()) {
        return;
    }
    if(it->awaitingAlbum) {
        const qint64 trackId
            = it->trackCandidate && it->trackCandidate->track.id.isUsable() ? it->trackCandidate->track.id.value : 0;
        finishFailure(jobId, operationFailure(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire,
                                              QUrl(it->albumPage.url), trackId, QStringLiteral("parent"),
                                              QStringLiteral("required album parent timed out")));
        return;
    }
    finishFailure(jobId, operationFailure(ResolutionErrorCode::Timeout, ResolutionStage::Fetch, QUrl(it->job.page.url),
                                          0, QStringLiteral("resource"), QStringLiteral("resolution timed out")));
}

void BandcampService::finishFailure(quint64 jobId, ResolutionError error)
{
    finishJob(jobId, ResolutionFailure{std::move(error)});
}

void BandcampService::finishJob(quint64 jobId, BandcampResolutionOutcome result)
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

    recordResult(active.job, std::move(result));
    startNextJobs();
}

void BandcampService::recordResult(const Job& job, BandcampResolutionOutcome result)
{
    auto it = m_batches.find(job.batchId);
    if(it == m_batches.end()) {
        return;
    }

    auto& pageResult    = it->results[job.index];
    pageResult.page     = job.page;
    pageResult.outcome  = std::move(result);
    const auto& outcome = *pageResult.outcome;
    const auto& error   = std::get_if<ResolutionFailure>(&outcome);
    if(error) {
        CorrelationContext context{.jobId       = static_cast<qint64>(job.jobId),
                                   .resourceUrl = error->error.resourceUrl,
                                   .trackId     = error->error.trackId};
        qCWarning(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
            context, QStringLiteral("resolution_failed"),
            {DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error->error.code)),
             DiagnosticField::text(QStringLiteral("stage"), resolutionStageName(error->error.stage)),
             DiagnosticField::text(QStringLiteral("field"), error->error.field),
             DiagnosticField::text(QStringLiteral("reason"), error->error.reason)});
        qCInfo(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
            context, QStringLiteral("resolution_finished"),
            {DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("failure")),
             DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error->error.code))});
    }
    else if(const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&outcome)) {
        qint64 trackId = 0;
        QString kind;
        if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
            kind = QStringLiteral("album");
            qCInfo(BANDCAMP_RESOLVE).noquote()
                << formatTraceEvent({.jobId = static_cast<qint64>(job.jobId), .resourceUrl = QUrl(job.page.url)},
                                    QStringLiteral("resolution_finished"),
                                    {DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("success")),
                                     DiagnosticField::text(QStringLiteral("kind"), kind),
                                     DiagnosticField::number(QStringLiteral("tracks"), album->tracks.size())});
        }
        else {
            const auto& track = std::get<BandcampResolvedTrack>(success->value);
            trackId           = track.track.id;
            kind              = track.album ? QStringLiteral("album_track") : QStringLiteral("standalone_track");
            qCInfo(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
                {.jobId = static_cast<qint64>(job.jobId), .resourceUrl = QUrl(job.page.url), .trackId = trackId},
                QStringLiteral("resolution_finished"),
                {DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("success")),
                 DiagnosticField::text(QStringLiteral("kind"), kind)});
        }
    }
    else {
        qCInfo(BANDCAMP_RESOLVE).noquote()
            << formatTraceEvent({.jobId = static_cast<qint64>(job.jobId), .resourceUrl = QUrl(job.page.url)},
                                QStringLiteral("resolution_finished"),
                                {DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("cancelled"))});
    }

    if(--it->remaining > 0) {
        return;
    }
    QList<BandcampResolvedPage> results = std::move(it->results);
    m_batches.erase(it);
    emit batchResolved(results);
}
} // namespace Fooyin::Bandcamp
