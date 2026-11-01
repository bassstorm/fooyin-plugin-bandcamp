#include "bandcampmodel.h"
#include "bandcampservice.h"
#include "bandcampurl.h"

#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QtTest>

#include <optional>
#include <variant>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
// Backstop for one operation. The service's own item timeouts are the primary limit.
constexpr int OperationWatchdogMs = 120'000;

const QString ArtistUrl{u"https://dither.bandcamp.com"_s};
const QString LabelUrl{u"https://breakbeatrebelz.bandcamp.com/"_s};
const QString AlbumUrl{u"https://offland.bandcamp.com/album/parallels"_s};
const QString MemberTrackUrl{u"https://offland.bandcamp.com/track/caaa-baaa-aaa"_s};
const QString StandaloneTrackUrl{u"https://ravver.bandcamp.com/track/burial-shutta-ravver-remix"_s};

struct LiveRun
{
    BandcampSubmission submission;
    QList<BandcampScopeResult> results;
    std::optional<BandcampOperationSummary> summary;
    bool timedOut{false};
};

QList<BandcampPageUrl> pagesFor(const QString& url)
{
    return {parseBandcampPageUrl(url)};
}

// Resolves releases one at a time, so the first reported scope is the one checked.
// With stopAfterFirstScope, the operation is cancelled after that scope, so a
// discography does not fetch its whole catalogue.
LiveRun runOperation(const QList<BandcampPageUrl>& pages, bool stopAfterFirstScope = false)
{
    QNetworkAccessManager network;
    BandcampService service(&network);
    QEventLoop loop;
    LiveRun run;
    bool cancelRequested{false};

    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(OperationWatchdogMs);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&run, &loop] {
        run.timedOut = true;
        loop.quit();
    });

    QObject::connect(&service, &BandcampService::scopeResolved, &loop,
                     [&run, &service, &cancelRequested, stopAfterFirstScope](const BandcampScopeResult& result) {
                         run.results.append(result);
                         if(!stopAfterFirstScope || cancelRequested) {
                             return;
                         }
                         cancelRequested = true;
                         const auto id = result.operationId;
                         // Queued so the service is not cancelled from inside its own emission.
                         QMetaObject::invokeMethod(
                             &service, [&service, id] { service.cancel(id); }, Qt::QueuedConnection);
                     });

    QObject::connect(&service, &BandcampService::operationFinished, &loop,
                     [&run, &loop](const BandcampOperationSummary& summary) {
                         run.summary = summary;
                         loop.quit();
                     });

    OperationOptions options;
    options.parallelism = 1;
    run.submission = service.submit(pages, options);
    if(std::holds_alternative<BandcampOperationAccepted>(run.submission)) {
        watchdog.start();
        loop.exec();
        watchdog.stop();
    }

    return run;
}

QString completionProblem(const LiveRun& run)
{
    if(!std::holds_alternative<BandcampOperationAccepted>(run.submission)) {
        return u"submission was rejected"_s;
    }
    if(run.timedOut) {
        return u"operation exceeded its watchdog"_s;
    }
    if(!run.summary) {
        return u"operation did not report a summary"_s;
    }
    return {};
}

QString describeOutcome(const BandcampResolutionOutcome& outcome)
{
    if(const auto* failure = std::get_if<ResolutionFailure>(&outcome)) {
        return u"%1 during %2: %3"_s.arg(resolutionErrorCodeName(failure->error.code),
                                          resolutionStageName(failure->error.stage), failure->error.reason);
    }
    return u"not resolved (cancelled)"_s;
}

const BandcampResolvedData* successData(const BandcampScopeResult& result)
{
    if(const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&result.outcome)) {
        return &success->value;
    }
    return nullptr;
}

void verifySourceUrl(const QString& url, BandcampPageKind kind)
{
    const auto page = parseBandcampPageUrl(url);
    QVERIFY2(page.isValid(), qPrintable(page.error));
    QVERIFY(page.kind == kind);
}

void verifyTrack(const BandcampTrack& track)
{
    QVERIFY(track.id > 0);
    QVERIFY(!track.title.trimmed().isEmpty());
    QVERIFY(track.streamUrl.startsWith(u"https://"_s));
}

void verifyAlbumMetadata(const BandcampAlbumMetadata& metadata)
{
    QVERIFY(metadata.id > 0);
    QVERIFY(!metadata.title.trimmed().isEmpty());
    QVERIFY(metadata.trackTotal > 0);
    verifySourceUrl(metadata.sourceUrl, BandcampPageKind::Album);
}

void verifyAlbum(const BandcampAlbum& album)
{
    verifyAlbumMetadata(album.metadata);
    QVERIFY(!album.tracks.isEmpty());
    QVERIFY(album.metadata.trackTotal >= album.tracks.size());
    for(const auto& track : album.tracks) {
        verifyTrack(track);
    }
}

void verifyResolvedTrack(const BandcampResolvedTrack& resolved)
{
    verifyTrack(resolved.track);
    if(resolved.album) {
        verifyAlbumMetadata(*resolved.album);
        verifySourceUrl(resolved.track.sourceUrl, BandcampPageKind::Album);
    }
    else {
        verifySourceUrl(resolved.track.sourceUrl, BandcampPageKind::Track);
    }
}

void verifyResolvedData(const BandcampResolvedData& data)
{
    if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
        verifyAlbum(*album);
    }
    else {
        verifyResolvedTrack(std::get<BandcampResolvedTrack>(data));
    }
}
} // namespace

class BandcampLiveTests : public QObject
{
    Q_OBJECT

private slots:
    void artistDiscographyResolves();
    void labelDiscographyResolves();
    void albumResolves();
    void memberTrackResolves();
    void standaloneTrackResolves();
};

void BandcampLiveTests::artistDiscographyResolves()
{
    const auto run = runOperation(pagesFor(ArtistUrl), true);
    const auto problem = completionProblem(run);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QVERIFY2(!run.results.isEmpty(), "no releases were reported");

    const auto& first = run.results.constFirst();
    const auto* data = successData(first);
    QVERIFY2(data, qPrintable(describeOutcome(first.outcome)));
    verifyResolvedData(*data);
    QVERIFY(run.summary->succeeded >= 1);
}

void BandcampLiveTests::labelDiscographyResolves()
{
    const auto run = runOperation(pagesFor(LabelUrl), true);
    const auto problem = completionProblem(run);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QVERIFY2(!run.results.isEmpty(), "no releases were reported");

    const auto& first = run.results.constFirst();
    const auto* data = successData(first);
    QVERIFY2(data, qPrintable(describeOutcome(first.outcome)));
    verifyResolvedData(*data);
    QVERIFY(run.summary->succeeded >= 1);
}

void BandcampLiveTests::albumResolves()
{
    const auto run = runOperation(pagesFor(AlbumUrl));
    const auto problem = completionProblem(run);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(run.results.size(), qsizetype{1});

    const auto* data = successData(run.results.constFirst());
    QVERIFY2(data, qPrintable(describeOutcome(run.results.constFirst().outcome)));
    const auto* album = std::get_if<BandcampAlbum>(data);
    QVERIFY(album);
    verifyAlbum(*album);
}

void BandcampLiveTests::memberTrackResolves()
{
    const auto run = runOperation(pagesFor(MemberTrackUrl));
    const auto problem = completionProblem(run);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(run.results.size(), qsizetype{1});

    const auto* data = successData(run.results.constFirst());
    QVERIFY2(data, qPrintable(describeOutcome(run.results.constFirst().outcome)));
    const auto* resolved = std::get_if<BandcampResolvedTrack>(data);
    QVERIFY(resolved);
    QVERIFY(resolved->album.has_value());
    verifyResolvedTrack(*resolved);
}

void BandcampLiveTests::standaloneTrackResolves()
{
    const auto run = runOperation(pagesFor(StandaloneTrackUrl));
    const auto problem = completionProblem(run);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(run.results.size(), qsizetype{1});

    const auto* data = successData(run.results.constFirst());
    QVERIFY2(data, qPrintable(describeOutcome(run.results.constFirst().outcome)));
    const auto* resolved = std::get_if<BandcampResolvedTrack>(data);
    QVERIFY(resolved);
    QVERIFY(!resolved->album.has_value());
    verifyResolvedTrack(*resolved);
}

QTEST_MAIN(BandcampLiveTests)

#include "bandcamplivetests.moc"
