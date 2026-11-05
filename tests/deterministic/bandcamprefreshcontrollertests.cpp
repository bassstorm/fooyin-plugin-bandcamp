#include "bandcamprefreshcontroller.h"
#include "bandcampsettings.h"
#include "bandcampfakenetwork.h"
#include "bandcamptestpages.h"
#include "bandcamplogcapture.h"

#include <QDateTime>
#include <QPromise>
#include <QtTest>

#include <limits>
#include <stdexcept>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QString AlbumUrl{u"https://artist.bandcamp.com/album/parent"_s};
const QString SoloUrl{u"https://artist.bandcamp.com/track/solo"_s};

QString stream(qint64 expiryMs)
{
    return u"https://t4.bcbits.com/stream/abc?token=private&ts="_s + QString::number(expiryMs / 1000);
}

Fooyin::Track resident(qint64 id, const QString& source, qint64 expiryMs)
{
    BandcampTrack model;
    model.id = id;
    model.title = u"Original"_s;
    model.sourceUrl = source;
    model.streamUrl = stream(expiryMs);
    auto track = makeFooyinTrack({model, std::nullopt});
    track.setId(static_cast<int>(id));
    track.setLibraryId(12);
    track.setTitle(u"User edit"_s);
    track.setArtists({u"User artist"_s});
    track.setAlbum(u"User album"_s);
    track.replaceExtraTag(u"MOOD"_s, u"User mood"_s);
    track.replaceExtraTag(u"COVERART"_s, u"User artwork"_s);
    track.setExtraProperty(u"_USER_PROPERTY"_s, u"User property"_s);
    track.setPlayCount(17);
    track.setRating(0.8F);
    track.setLoved(true);
    track.setHash(u"user-hash"_s);
    return track;
}

QByteArray row(qint64 id, qint64 expiryMs)
{
    return "{\"id\":" + QByteArray::number(id) + ",\"title\":\"Child\",\"file\":{\"mp3-128\":\""
        + stream(expiryMs).toUtf8() + "\"}}";
}

QByteArray album(qint64 expiryMs, bool badSibling = false)
{
    return albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                     {row(7, expiryMs), badSibling ? QByteArray{R"({"id":8,"title":"Unavailable"})"}
                                                   : row(8, expiryMs)});
}

QByteArray solo(qint64 expiryMs)
{
    return tralbumPage(R"({"current":{"type":"track","id":9,"title":"Solo","album_id":null},"trackinfo":[)"
                       + row(9, expiryMs) + "]}");
}

// Only public snapshot/write boundaries; no host database or live-copy simulation.
struct LibraryBoundary
{
    LibraryBoundary()
    {
        api.owner = &owner;
        api.playlistOwner = &owner;
        api.tracks = [this]() {
            if(snapshotHook) {
                snapshotHook();
            }
            return library;
        };
        api.playlistTracks = [this]() { return playlist; };
        api.metadataStore = []() { return std::shared_ptr<Fooyin::TrackMetadataStore>{}; };
        api.writeTrackMetadata = [this](const Fooyin::TrackList& targets) {
            writes.push_back(targets);
            if(writeHook) {
                writeHook();
            }
            QPromise<Fooyin::WriteResult> promise;
            promise.start();
            promise.addResult(Fooyin::WriteResult{writeState,
                                                  failWrite || writeState == Fooyin::WriteState::Cancelled
                                                      ? 0 : static_cast<int>(targets.size()),
                                                  failWrite ? static_cast<int>(targets.size()) : 0});
            promise.finish();
            return Fooyin::WriteRequest{[this]() { ++cancellations; }, promise.future()};
        };
    }
    QObject owner;
    FooyinLibraryApi api;
    Fooyin::TrackList library;
    Fooyin::TrackList playlist;
    std::vector<Fooyin::TrackList> writes;
    std::function<void()> snapshotHook;
    std::function<void()> writeHook;
    bool failWrite{false};
    Fooyin::WriteState writeState{Fooyin::WriteState::Completed};
    int cancellations{0};
};

struct Capture
{
    explicit Capture(BandcampRefreshController& operation)
    {
        QObject::connect(&operation, &BandcampRefreshController::finished, &receiver, [this](const auto& value) {
            summary = value;
            ++finishes;
        });
        QObject::connect(&operation, &BandcampRefreshController::consumerFinished, &receiver,
                         [this](qint64, const auto& value) { outcomes.append(value); });
    }
    QObject receiver;
    BandcampRefreshSummary summary;
    QList<BandcampRefreshOutcome> outcomes;
    int finishes{0};
};

class RefreshTests : public QObject
{
    Q_OBJECT
private slots:
    void freshnessMeasuredBoundary();
    void selectionAndOptionsSnapshot();
    void transportWriteMatchesSelectedTrackWhenPersistenceFails();
    void groupedAlbumAndStandalonePreserveResidents();
    void atomicReleaseFailureAndMissingId();
    void persistedBoundaryFailuresAndUntrustedExpiry();
    void queuedWriteReadsCurrentResidentAndFailsMissing();
    void failedWriteIsNotUpdated();
    void cancellationDuringWriteCountsAcceptedAndDoesNotFinishEarly();
    void cancellationBeforeWriting();
    void withdrawalAndDeletion();
    void localFailuresAreObservableAndLaterRefreshSucceeds();
};

void RefreshTests::freshnessMeasuredBoundary()
{
    const qint64 now = 2'000'000;
    const qint64 window = refreshWindowMilliseconds(BandcampSettings{}.prePlaybackRefreshWindowMinutes);
    QCOMPARE(window, qint64{3'600'000});
    QVERIFY(!bandcampRefreshNeeded({BandcampExpiryState::Valid, now + window}, now, window));
    QVERIFY(bandcampRefreshNeeded({BandcampExpiryState::Valid, now + window - 1}, now, window));
    QVERIFY(!bandcampRefreshNeeded({BandcampExpiryState::Valid, now + window + 1}, now, window));
    QVERIFY(bandcampRefreshNeeded({BandcampExpiryState::Valid, now}, now, 0));
    QVERIFY(!bandcampRefreshNeeded({BandcampExpiryState::Valid, now + 1}, now, 0));
    QVERIFY(bandcampRefreshNeeded({}, now, 0));
    QVERIFY(bandcampRefreshNeeded({BandcampExpiryState::Invalid, 0}, now, window));
    QVERIFY(!bandcampRefreshNeeded({BandcampExpiryState::Valid, std::numeric_limits<qint64>::max()}, now, window));
    QVERIFY(!bandcampStreamExpiryMs(u"https://t4.bcbits.com/stream/abc?ts=9223372036854776"_s));
    const QString prefix = u"https://cdn.example.com/"_s;
    const QString boundary = prefix + QString(MaxBandcampStreamUrlChars - prefix.size(), u'a');
    QVERIFY(isSupportedBandcampStreamUrl(boundary));
    QVERIFY(!isSupportedBandcampStreamUrl(boundary + u'a'));
}

void RefreshTests::selectionAndOptionsSnapshot()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const qint64 now = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    Fooyin::TrackList selected{Fooyin::Track{u"file:///local.mp3"_s}};
    QVERIFY(!hasSelectedBandcampTracks(selected));
    BandcampRefreshController empty{&service, selected, {}, host.api};
    Capture emptyResult{empty};
    empty.start(now);
    QCOMPARE(emptyResult.finishes, 1);
    QVERIFY(network.requested.empty());

    selected.push_back(resident(7, AlbumUrl, now + 3'600'000));
    selected.push_back(selected.back());
    QVERIFY(hasSelectedBandcampTracks(selected));
    OperationOptions options;
    BandcampRefreshController reusable{&service, selected, options, host.api};
    selected.clear();
    options.prePlaybackRefreshWindowMs = 0;
    Capture captured{reusable};
    reusable.start(now);
    QCOMPARE(captured.finishes, 1);
    QCOMPARE(captured.summary.unchanged, 1);
    QVERIFY(network.requested.empty());
    QVERIFY(std::none_of(reusable.children().cbegin(), reusable.children().cend(), [](QObject* child) {
        return dynamic_cast<BandcampLibraryGateway*>(child) != nullptr;
    }));

    auto near = resident(7, AlbumUrl, now + 1000);
    host.library = {near};
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    OperationOptions oneMinute;
    oneMinute.prePlaybackRefreshWindowMs = refreshWindowMilliseconds(1);
    BandcampRefreshController refresh{&service, {near}, oneMinute, host.api};
    oneMinute.prePlaybackRefreshWindowMs = 0;
    Capture refreshed{refresh};
    refresh.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(refreshed.finishes, 1, 3000);
    QCOMPARE(refreshed.summary.updated, 1);
    QCOMPARE(network.requested.size(), 1);

    BandcampRefreshController zero{&service, {near}, oneMinute, host.api};
    Capture zeroResult{zero};
    zero.start(now);
    QCOMPARE(zeroResult.summary.unchanged, 1);
    QCOMPARE(network.requested.size(), 1);
}

void RefreshTests::transportWriteMatchesSelectedTrackWhenPersistenceFails()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const qint64 now = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    const qint64 firstExpiry = now + 7'200'000;
    const qint64 selectedExpiry = now + 10'800'000;
    auto selected = resident(8, AlbumUrl, now - 1000);
    host.library = {selected};
    host.failWrite = true;
    network.setResponder([firstExpiry, selectedExpiry](const auto&) {
        return FakeResponse{.body = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                              {row(7, firstExpiry), row(8, selectedExpiry)})};
    });

    BandcampRefreshController operation{&service, {selected}, {}, host.api};
    Capture result{operation};
    operation.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 3000);
    QCOMPARE(result.summary.failed, 1);
    QCOMPARE(result.summary.updated, 0);
    QCOMPARE(host.writes.size(), std::size_t{1});
    QCOMPARE(host.writes.front().front().id(), 8);
    QCOMPARE(host.writes.front().front().extraProperties().value(BandcampStreamUrlProperty), stream(selectedExpiry));
    QCOMPARE(network.requested.size(), 1);
}

void RefreshTests::groupedAlbumAndStandalonePreserveResidents()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    host.library = {resident(7, AlbumUrl, now - 1000), resident(8, AlbumUrl, now - 1000),
                    resident(9, SoloUrl, now - 1000)};
    network.setResponder([now](const auto& request) {
        return FakeResponse{.body = request.url().path().startsWith(u"/album")
                                       ? album(now + 7'200'000) : solo(now + 7'200'000)};
    });
    auto selected = host.library;
    selected.push_back(selected.front());
    selected.push_back(Fooyin::Track{u"file:///other.mp3"_s});
    BandcampRefreshController operation{&service, selected, {}, host.api};
    Capture result{operation};
    operation.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 3000);
    QCOMPARE(result.summary.updated, 3);
    QCOMPARE(result.summary.failed, 0);
    QCOMPARE(network.requested.size(), 2);
    QCOMPARE(host.writes.size(), std::size_t{3});
    for(const auto& targets : host.writes) {
        QCOMPARE(targets.size(), std::size_t{1});
        const auto& target = targets.front();
        const auto& previous = host.library.at(static_cast<std::size_t>(target.id() - 7));
        QCOMPARE(target.title(), previous.title());
        QCOMPARE(target.artists(), previous.artists());
        QCOMPARE(target.album(), previous.album());
        QCOMPARE(target.extraTag(u"MOOD"_s), previous.extraTag(u"MOOD"_s));
        QCOMPARE(target.extraTag(u"COVERART"_s), previous.extraTag(u"COVERART"_s));
        QCOMPARE(target.playCount(), previous.playCount());
        QCOMPARE(target.rating(), previous.rating());
        QCOMPARE(target.isLoved(), previous.isLoved());
        QCOMPARE(target.hash(), previous.hash());
        QCOMPARE(target.filepath(), previous.filepath());
        QCOMPARE(target.libraryId(), previous.libraryId());
        QCOMPARE(target.extraProperties().value(u"_USER_PROPERTY"_s), u"User property"_s);
        QCOMPARE(target.extraProperties().value(BandcampStreamUrlProperty), stream(now + 7'200'000));
    }
}

void RefreshTests::atomicReleaseFailureAndMissingId()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    host.library = {resident(7, AlbumUrl, now - 1000), resident(8, AlbumUrl, now - 1000),
                    resident(9, SoloUrl, now - 1000)};
    network.setResponder([now](const auto& request) {
        return FakeResponse{.body = request.url().path().startsWith(u"/album")
                                       ? album(now + 7'200'000, true) : solo(now + 7'200'000)};
    });
    BandcampRefreshController operation{&service, host.library, {}, host.api};
    Capture result{operation};
    operation.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 3000);
    QCOMPARE(result.summary.updated, 1);
    QCOMPARE(result.summary.failed, 2);
    QVERIFY(std::any_of(result.outcomes.cbegin(), result.outcomes.cend(), [](const auto& outcome) {
        const auto* error = std::get_if<BandcampRefreshFailure>(&outcome);
        return error && error->cause.has_value();
    }));
    QCOMPARE(host.writes.size(), std::size_t{1});
    QCOMPARE(host.writes.front().front().id(), 9);
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    BandcampRefreshController missing{&service, {resident(99, AlbumUrl, now - 1000)}, {}, host.api};
    Capture missingResult{missing};
    missing.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(missingResult.finishes, 1, 3000);
    QCOMPARE(missingResult.summary.failed, 1);
    QCOMPARE(std::get<BandcampRefreshFailure>(missingResult.outcomes.front()).code,
             BandcampRefreshError::MissingSelectedTrack);
}

void RefreshTests::persistedBoundaryFailuresAndUntrustedExpiry()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    auto track = resident(7, AlbumUrl, now + 7'200'000);
    for(const auto& [property, value] : QList<QPair<QString, QString>>{
             {BandcampTrackIdProperty, u"8"_s}, {BandcampSourceUrlProperty, u"https://artist.bandcamp.com/music"_s},
             {BandcampSourceUrlProperty, {}}, {StreamProviderProperty, u"other"_s},
             {BandcampStreamUrlProperty, u"http://t4.bcbits.com/stream/abc?ts=123"_s}}) {
        auto invalid = track;
        invalid.setExtraProperty(property, value);
        BandcampRefreshController operation{&service, {invalid}, {}, host.api};
        Capture result{operation};
        operation.start(now);
        QCOMPARE(result.summary.failed, 1);
    }
    auto invalidUri = track;
    invalidUri.setFilePath(u"bandcamp://track/07"_s);
    BandcampRefreshController invalid{&service, {invalidUri}, {}, host.api};
    Capture invalidResult{invalid};
    invalid.start(now);
    QCOMPARE(invalidResult.summary.failed, 1);
    QVERIFY(network.requested.empty());
    for(const QString& expiry : {QString{}, u"9223372036854775808"_s, u"1"_s}) {
        auto unknown = track;
        unknown.setExtraProperty(BandcampExpiresAtProperty, expiry);
        host.library = {unknown};
        OperationOptions options;
        options.prePlaybackRefreshWindowMs = 0;
        BandcampRefreshController operation{&service, {unknown}, options, host.api};
        Capture result{operation};
        operation.start(now);
        QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 3000);
        QCOMPARE(result.summary.updated, 1);
    }
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now - 1000)}; });
    BandcampRefreshController expired{&service, {resident(7, AlbumUrl, now - 1000)}, {}, host.api};
    Capture expiredResult{expired};
    expired.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(expiredResult.finishes, 1, 3000);
    QCOMPARE(std::get<BandcampRefreshFailure>(expiredResult.outcomes.front()).code,
             BandcampRefreshError::UnusableRefreshedExpiry);
    network.setResponder([](const auto&) {
        return FakeResponse{.body = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                             {streamEntry(R"("id":7,"title":"Unknown expiry")")})};
    });
    BandcampRefreshController unknownFresh{&service, {resident(7, AlbumUrl, now - 1000)}, {}, host.api};
    Capture unknownResult{unknownFresh};
    unknownFresh.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(unknownResult.finishes, 1, 3000);
    QCOMPARE(std::get<BandcampRefreshFailure>(unknownResult.outcomes.front()).code,
             BandcampRefreshError::UnusableRefreshedExpiry);
}

void RefreshTests::queuedWriteReadsCurrentResidentAndFailsMissing()
{
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    host.library = {resident(7, AlbumUrl, now - 1000)};
    host.playlist = {resident(7, AlbumUrl, now - 1000)};
    BandcampLibraryGateway gateway{host.api};
    QObject receiver;
    std::optional<FooyinTrackCommitOutcome> result;
    BandcampTrack model{.id = 7, .title = u"Fresh"_s, .sourceUrl = AlbumUrl, .streamUrl = stream(now + 7'200'000)};
    gateway.refreshTransport({model, std::nullopt}, &receiver, {}, [&result](auto value) { result = value; });
    host.library.front().setTitle(u"Latest library edit"_s);
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QCOMPARE(host.writes.front().front().title(), u"Latest library edit"_s);
    host.library.clear();
    host.playlist.front().setTitle(u"First playlist edit"_s);
    host.playlist.push_back(resident(7, AlbumUrl, now - 1000));
    result.reset();
    gateway.refreshTransport({model, std::nullopt}, &receiver, {}, [&result](auto value) { result = value; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QCOMPARE(host.writes.back().front().title(), u"First playlist edit"_s);
    host.playlist.clear();
    result.reset();
    gateway.refreshTransport({model, std::nullopt}, &receiver, {}, [&result](auto value) { result = value; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QCOMPARE(std::get<FooyinTrackCommitFailure>(*result).code, FooyinTrackCommitErrorCode::MissingResident);
    QCOMPARE(host.writes.size(), std::size_t{2});
}

void RefreshTests::failedWriteIsNotUpdated()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    host.library = {resident(7, AlbumUrl, now - 1000)};
    host.failWrite = true;
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    BandcampRefreshController operation{&service, host.library, {}, host.api};
    Capture result{operation};
    operation.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 3000);
    QCOMPARE(result.summary.failed, 1);
    QCOMPARE(result.summary.updated, 0);
}

void RefreshTests::cancellationDuringWriteCountsAcceptedAndDoesNotFinishEarly()
{
    for(int scenario = 0; scenario != 3; ++scenario) {
        FakeNetworkManager network;
        BandcampResolutionService service{&network};
        LibraryBoundary host;
        host.failWrite = scenario == 1;
        host.writeState = scenario == 2 ? Fooyin::WriteState::Cancelled : Fooyin::WriteState::Completed;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        host.library = {resident(7, AlbumUrl, now - 1000), resident(8, AlbumUrl, now - 1000)};
        network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
        BandcampRefreshController operation{&service, host.library, {}, host.api};
        Capture result{operation};
        host.writeHook = [&]() {
            operation.cancel();
            QCOMPARE(result.finishes, 0);
        };
        operation.start(now);
        QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 3000);
        QCOMPARE(result.summary.updated, scenario == 0 ? 1 : 0);
        QCOMPARE(result.summary.failed, scenario == 1 ? 1 : 0);
        QCOMPARE(result.summary.cancelled, scenario == 2 ? 2 : 1);
        QCOMPARE(host.writes.size(), std::size_t{1});
        QCOMPARE(host.cancellations, 1);

        network.setResponder([now](const auto&) {
            return FakeResponse{.body = album(now + 7'200'000), .holdDelivery = true};
        });
        QPointer<BandcampResolutionService> dyingSource{new BandcampResolutionService{&network}};
        auto* withdrawing = new BandcampRefreshController{dyingSource, host.library, {}, host.api};
        Capture teardown{*withdrawing};
        connect(dyingSource, &BandcampResolutionService::scopeResolved, &host.owner,
                [&dyingSource]() { delete dyingSource; });
        const auto requestsBefore = network.requested.size();
        withdrawing->start(now);
        QTRY_COMPARE_WITH_TIMEOUT(network.requested.size(), requestsBefore + 1, 1000);
        delete withdrawing;
        QVERIFY(!dyingSource);
        QCOMPARE(teardown.outcomes.size(), 0);
        QCOMPARE(teardown.finishes, 0);
    }
}

void RefreshTests::withdrawalAndDeletion()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    host.library = {resident(7, AlbumUrl, now - 1000)};
    network.setResponder([now](const auto&) {
        return FakeResponse{.body = album(now + 7'200'000), .holdDelivery = true};
    });
    auto* operation = new BandcampRefreshController{&service, host.library, {}, host.api};
    Capture result{*operation};
    BandcampRefreshController independent{&service, host.library, {}, host.api};
    Capture independentResult{independent};
    operation->start(now);
    independent.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(network.requested.size(), 2, 1000);
    operation->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(result.finishes, 1, 1000);
    QCOMPARE(result.summary.cancelled, 1);
    delete operation;
    network.releaseHeldResponses();
    QTRY_COMPARE_WITH_TIMEOUT(independentResult.finishes, 1, 3000);
    QCOMPARE(independentResult.summary.updated, 1);
    QCOMPARE(host.writes.size(), std::size_t{1});
    host.writes.clear();

    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    QPointer<BandcampRefreshController> doomed
        = new BandcampRefreshController{&service, host.library, {}, host.api};
    host.snapshotHook = [&]() { delete doomed; };
    doomed->start(now);
    QTRY_VERIFY_WITH_TIMEOUT(!doomed, 3000);
    QVERIFY(host.writes.empty());
    host.snapshotHook = {};
    doomed = new BandcampRefreshController{&service, host.library, {}, host.api};
    host.writeHook = [&]() { delete doomed; };
    doomed->start(now);
    QTRY_VERIFY_WITH_TIMEOUT(!doomed, 3000);
    QCOMPARE(host.writes.size(), std::size_t{1});
    QCOMPARE(host.cancellations, 1);
}

void RefreshTests::cancellationBeforeWriting()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    host.library = {resident(7, AlbumUrl, now - 1000)};
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    BandcampRefreshController queued{&service, host.library, {}, host.api};
    Capture queuedResult{queued};
    const auto connection = connect(&service, &BandcampResolutionService::operationFinished, &queued,
                                    [&queued]() { queued.cancel(); });
    queued.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(queuedResult.finishes, 1, 3000);
    QCOMPARE(queuedResult.summary.cancelled, 1);
    QVERIFY(host.writes.empty());
    disconnect(connection);

    BandcampRefreshController reading{&service, host.library, {}, host.api};
    Capture readingResult{reading};
    host.snapshotHook = [&]() { reading.cancel(); };
    reading.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(readingResult.finishes, 1, 3000);
    QCOMPARE(readingResult.summary.cancelled, 1);
    QVERIFY(host.writes.empty());
}

void RefreshTests::localFailuresAreObservableAndLaterRefreshSucceeds()
{
    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    LibraryBoundary host;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto invalidOne = resident(7, AlbumUrl, now - 1000);
    invalidOne.setFilePath(u"bandcamp://track/07"_s);
    auto invalidTwo = invalidOne;
    invalidTwo.setFilePath(u"bandcamp://track/0"_s);
    BandcampRefreshController absent{
        nullptr, {resident(7, AlbumUrl, now - 1000), resident(8, AlbumUrl, now - 1000), invalidOne, invalidTwo},
        {}, host.api};
    Capture absentResult{absent};
    QList<qint64> identities;
    connect(&absent, &BandcampRefreshController::consumerFinished, &absent,
            [&identities](qint64 id) { identities.append(id); });
    absent.start(now);
    QCOMPARE(absentResult.finishes, 1);
    QCOMPARE(absentResult.summary.failed, 4);
    QCOMPARE(identities.count(0), 2);
    QVERIFY(std::all_of(identities.cbegin(), identities.cend(), [](qint64 id) { return id >= 0; }));
    QCOMPARE(std::get<BandcampRefreshFailure>(absentResult.outcomes.back()).code,
             BandcampRefreshError::SourceUnavailable);
    QVERIFY(std::any_of(capturedLogs().cbegin(), capturedLogs().cend(), [](const auto& log) {
        return log.message.contains(u"refresh_operation_failed"_s)
            && log.message.contains(u"The release service is unavailable."_s);
    }));

    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    host.library = {resident(7, AlbumUrl, now - 1000)};
    network.setResponder([now](const auto&) { return FakeResponse{.body = album(now + 7'200'000)}; });
    BandcampRefreshController observed{&service, host.library, {}, host.api};
    Capture observedResult{observed};
    observed.start(now);
    QTRY_COMPARE_WITH_TIMEOUT(observedResult.finishes, 1, 3000);
    QCOMPARE(observedResult.summary.updated, 1);
}
} // namespace

int runRefreshTests(int argc, char** argv)
{
    RefreshTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcamprefreshcontrollertests.moc"
