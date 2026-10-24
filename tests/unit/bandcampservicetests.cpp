#include "bandcampfakenetwork.h"
#include "bandcamplogcapture.h"
#include "bandcampservice.h"
#include "bandcamptestpages.h"
#include "bandcampurl.h"

#include <QEventLoop>
#include <QTimer>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QString TrackUrl{u"https://artist.bandcamp.com/track/selected"_s};
const QString AlbumUrl{u"https://artist.bandcamp.com/album/parent"_s};
const QByteArray TrackPage = tralbumPage(
    R"({"current":{"type":"track","id":7,"title":"Page title","artist":"Page artist","release_date":"2024-05-01","art_id":91,"album_id":40},"artist":"Fallback","album_url":"https://artist.bandcamp.com/album/parent","trackinfo":[{"id":7,"title":"Page title","artist":"Page artist","track_num":3,"duration":150,"file":{"mp3-128":"https://cdn.example.com/old-page.mp3?sig=old"}}]})");
const QByteArray UnknownParentTrackPage = tralbumPage(
    R"({"current":{"type":"track","id":7,"title":"Page title","artist":"Page artist","release_date":"2024-05-01","art_id":91},"artist":"Fallback","album_url":"https://artist.bandcamp.com/album/parent","trackinfo":[{"id":7,"title":"Page title","artist":"Page artist","track_num":3,"duration":150,"file":{"mp3-128":"https://cdn.example.com/old-page.mp3?sig=old"}}]})");
const QByteArray AlbumPage = albumPage(
    R"({"type":"album","id":40,"title":"Parent album","artist":"Parent artist","release_date":"2020-01-01","art_id":20})",
    {streamEntry(R"("id":7,"title":"Parent child title","artist":"Child artist","track_num":1,"duration":90)")});

struct ServiceRun
{
    bool finished{false};
    QList<BandcampResolvedPage> results;
};

ServiceRun resolve(BandcampService& service, const QList<BandcampPageUrl>& pages)
{
    ServiceRun run;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(5000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&service, &BandcampService::batchResolved, &loop,
                     [&run, &loop](const QList<BandcampResolvedPage>& results) {
                         run.finished = true;
                         run.results  = results;
                         loop.quit();
                     });
    service.resolve(pages);
    loop.exec();
    return run;
}
} // namespace

class ServiceAdmissionTests : public QObject
{
    Q_OBJECT

private slots:
    void albumTrackRequiresAndUsesParent()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            return FakeResponse{.status = 200,
                                .body   = request.url().path().startsWith(u"/track/"_s) ? TrackPage : AlbumPage};
        });
        BandcampService service{&network};
        const auto run = resolve(service, {parseBandcampPageUrl(TrackUrl)});
        QVERIFY(run.finished);
        QCOMPARE(run.results.size(), 1);
        QCOMPARE(network.requested.size(), 2);
        QCOMPARE(network.requested.at(0), QUrl(TrackUrl));
        QCOMPARE(network.requested.at(1), QUrl(AlbumUrl));

        const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&*run.results.first().outcome);
        QVERIFY(success);
        const auto* resolved = std::get_if<BandcampResolvedTrack>(&success->value);
        QVERIFY(resolved);
        QVERIFY(resolved->album.has_value());
        QCOMPARE(resolved->album->id, qint64{40});
        QCOMPARE(resolved->album->title, u"Parent album"_s);
        QCOMPARE(resolved->album->trackTotal, 1);
        QCOMPARE(resolved->track.title, u"Parent child title"_s);
        QCOMPARE(resolved->track.artist, u"Child artist"_s);
        QCOMPARE(resolved->track.trackNumber, 1);
        QCOMPARE(resolved->track.durationMs, qint64{90000});
        QCOMPARE(resolved->track.streamUrl, u"https://cdn.example.com/track.mp3"_s);
        QCOMPARE(resolved->track.releaseDate, QDate(2024, 5, 1));
        QCOMPARE(resolved->track.artId, qint64{91});
        QCOMPARE(resolved->track.sourceUrl, AlbumUrl);
    }

    void usableLocationAcquiresParentWhenMembershipIdIsUnknown()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            return FakeResponse{.status = 200,
                                .body
                                = request.url().path().startsWith(u"/track/"_s) ? UnknownParentTrackPage : AlbumPage};
        });
        BandcampService service{&network};
        const auto run = resolve(service, {parseBandcampPageUrl(TrackUrl)});
        QVERIFY(run.finished);
        QCOMPARE(network.requested.size(), 2);
        QCOMPARE(network.requested.at(1), QUrl(AlbumUrl));
        const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&*run.results.first().outcome);
        QVERIFY(success);
        const auto* track = std::get_if<BandcampResolvedTrack>(&success->value);
        QVERIFY(track);
        QVERIFY(track->album.has_value());
        QCOMPARE(track->album->id, qint64{40});
    }

    void requiredParentFailureIsTerminalWithoutPartialTrack()
    {
        LogCapture capture(u"fy.bandcamp.resolve.warning=true"_s);
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().path().startsWith(u"/track/"_s)) {
                return FakeResponse{.status = 200, .body = TrackPage};
            }
            return FakeResponse{.status = 503};
        });
        BandcampService service{&network};
        const auto run = resolve(service, {parseBandcampPageUrl(TrackUrl)});
        QVERIFY(run.finished);
        QCOMPARE(run.results.size(), 1);
        QCOMPARE(network.requested.size(), 2);
        QVERIFY(!run.results.first().isValid());
        const auto* failed = std::get_if<ResolutionFailure>(&*run.results.first().outcome);
        QVERIFY(failed);
        QCOMPARE(failed->error.code, ResolutionErrorCode::ParentUnavailable);
        QCOMPARE(failed->error.stage, ResolutionStage::Acquire);
        QCOMPARE(failed->error.trackId, qint64{7});
        QVERIFY(!std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(*run.results.first().outcome));

        int terminalWarnings = 0;
        for(const auto& log : capturedLogs()) {
            if(log.category == u"fy.bandcamp.resolve"_s && log.message.contains(u"resolution_failed"_s)) {
                ++terminalWarnings;
                QVERIFY(log.message.contains(u"bandcamp://track/7"_s));
                QVERIFY(!log.message.contains(u"sig=old"_s));
            }
        }
        QCOMPARE(terminalWarnings, 1);
    }

    void malformedRequiredParentDoesNotFallBackToTrackPage()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            return FakeResponse{
                .status = 200,
                .body   = request.url().path().startsWith(u"/track/"_s) ? TrackPage : tralbumPage(R"({"current":{}})")};
        });
        BandcampService service{&network};
        const auto run = resolve(service, {parseBandcampPageUrl(TrackUrl)});
        QVERIFY(run.finished);
        QCOMPARE(network.requested.size(), 2);
        const auto* failed = std::get_if<ResolutionFailure>(&*run.results.first().outcome);
        QVERIFY(failed);
        QCOMPARE(failed->error.code, ResolutionErrorCode::ParentUnavailable);
        QVERIFY(failed->error.reason.contains(u"required album parent failed"_s));
    }

    void wholeAlbumUsesAtomicAdmissionBeforePublishing()
    {
        const QByteArray brokenAlbum
            = albumPage(R"({"type":"album","id":40,"title":"Album"})",
                        {streamEntry(R"("id":1,"title":"Playable")"), R"({"id":2,"title":"No stream"})"});
        FakeNetworkManager network;
        network.setResponder(
            [brokenAlbum](const QNetworkRequest&) { return FakeResponse{.status = 200, .body = brokenAlbum}; });
        BandcampService service{&network};
        const auto run = resolve(service, {parseBandcampPageUrl(AlbumUrl)});
        QVERIFY(run.finished);
        const auto* failed = std::get_if<ResolutionFailure>(&*run.results.first().outcome);
        QVERIFY(failed);
        QCOMPARE(failed->error.trackId, qint64{2});
        QCOMPARE(failed->error.field, u"track.streamUrl"_s);
    }
};

int runServiceTests(int argc, char** argv)
{
    ServiceAdmissionTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampservicetests.moc"
