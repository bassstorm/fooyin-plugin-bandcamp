#include "bandcampfooyintrack.h"
#include "bandcampimportcontroller.h"
#include "bandcamplibrarygateway.h"
#include "bandcamplogcapture.h"
#include "bandcamplogging.h"
#include "bandcampstreamprobe.h"
#include "bandcamptrackuri.h"

#include <core/scripting/scriptparser.h>
#include <core/scripting/scriptregistry.h>

#include <QPromise>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
const QString AlbumSource{u"https://artist.bandcamp.com/album/parent"_s};
const QString TrackSource{u"https://artist.bandcamp.com/track/solo"_s};
const QString SignedStream{u"https://t4.bcbits.com/stream/0123456789abcdef0123456789abcdef?token=one&ts=1793621993"_s};
const QString OtherSignedStream{
    u"https://t4.bcbits.com/stream/fedcba9876543210fedcba9876543210?token=two&ts=1793622993"_s};

BandcampResolvedTrack memberTrack(qint64 id = 42, QString stream = SignedStream)
{
    BandcampTrack track;
    track.id          = id;
    track.title       = u"  Recording title  "_s;
    track.artist      = u"Recording artist"_s;
    track.sourceUrl   = AlbumSource;
    track.streamUrl   = std::move(stream);
    track.releaseDate = QDate{2018, 5, 6};
    track.trackNumber = 4;
    track.durationMs  = 12345;

    BandcampAlbumMetadata album;
    album.id          = 77;
    album.title       = u"Containing album"_s;
    album.artist      = u"Album artist"_s;
    album.sourceUrl   = AlbumSource;
    album.releaseDate = QDate{2010, 1, 2};
    album.trackTotal  = 8;

    return {std::move(track), std::move(album)};
}

BandcampResolvedTrack standaloneTrack(qint64 id = 43)
{
    BandcampTrack track;
    track.id        = id;
    track.title     = u"Standalone title"_s;
    track.artist    = u"Standalone artist"_s;
    track.sourceUrl = TrackSource;
    track.streamUrl = u"https://cdn.example.com/solo.mp3?sig=private"_s;
    return {std::move(track), std::nullopt};
}

Fooyin::Track project(const BandcampResolvedTrack& resolved)
{
    return makeFooyinTrack(resolved);
}

Fooyin::Track residentWithUserEdits(int databaseId)
{
    Fooyin::Track track = project(memberTrack(42, u"https://cdn.example.com/old.mp3?sig=old"_s));
    track.setId(databaseId);
    track.setLibraryId(12);
    track.setTitle(u"User-edited title"_s);
    track.setArtists({u"User-edited artist"_s});
    track.setAlbum(u"User-edited album"_s);
    track.setAlbumArtists({u"User-edited album artist"_s});
    track.setDate(u"2023-02-01"_s);
    track.setTrackNumber(u"19"_s);
    track.setTrackTotal(u"21"_s);
    track.setPlayCount(6);
    track.setRating(0.8F);
    track.setLoved(true);
    track.setFirstPlayed(1234);
    track.setLastPlayed(5678);
    track.replaceExtraTag(u"MOOD"_s, u"User mood"_s);
    track.replaceExtraTag(u"COVERART"_s, u"https://covers.example.org/user-art.jpg"_s);
    track.setHash(u"user-stable-hash"_s);
    track.setExtraProperty(u"_USER_PROPERTY"_s, u"retain me"_s);
    track.clearWasModified();
    return track;
}

class PendingStreamProbe final : public BandcampStreamProbe
{
public:
    explicit PendingStreamProbe(QObject* parent = nullptr)
        : BandcampStreamProbe(nullptr, nullptr, parent)
    { }

    BandcampStreamProbeSubmission probe(const QList<BandcampResolvedTrack>& tracks, QObject* callbackContext,
                                        Completion completion) override
    {
        const quint64 token = m_nextToken++;
        m_pending.append([callbackContext, completion = std::move(completion), tracks]() {
            if(!callbackContext) {
                return;
            }
            QList<BandcampStreamProbeResult> results;
            results.reserve(tracks.size());
            for(const auto& track : tracks) {
                results.append({track.track.id, {}, std::nullopt});
            }
            completion(std::move(results));
        });
        return {token, std::nullopt};
    }

    void cancel(quint64) override
    {
        m_pending.clear();
    }

    void completePending()
    {
        auto pending = std::move(m_pending);
        for(auto& complete : pending) {
            complete();
        }
    }

    [[nodiscard]] bool hasPending() const
    {
        return !m_pending.isEmpty();
    }

private:
    quint64 m_nextToken{1};
    QList<std::function<void()>> m_pending;
};

class LibraryBoundary
{
public:
    LibraryBoundary()
    {
        api.owner          = owner.get();
        api.playlistOwner  = owner.get();
        api.playlistTracks = []() {
            return Fooyin::TrackList{};
        };
        api.tracks = [this]() {
            ++snapshotCalls;
            if(snapshotDelayMs > 0) {
                QThread::msleep(static_cast<unsigned long>(snapshotDelayMs));
            }
            if(snapshotHook) {
                snapshotHook();
            }
            return residents;
        };
        api.metadataStore = [this]() -> std::shared_ptr<Fooyin::TrackMetadataStore> {
            return store;
        };
        api.writeTrackMetadata = [this](const Fooyin::TrackList& tracks) {
            ++writeCalls;
            writeInputs.push_back(tracks);
            if(invalidWriteRequest || promiseIndex >= promises.size()) {
                return Fooyin::WriteRequest{};
            }
            auto promise = promises.at(promiseIndex++);
            if(writeHook) {
                writeHook();
            }
            return Fooyin::WriteRequest{[this]() { ++cancelCalls; }, promise->future()};
        };
    }

    void makeWriteAvailable()
    {
        auto promise = std::make_shared<QPromise<Fooyin::WriteResult>>();
        promise->start();
        promises.push_back(std::move(promise));
    }

    void finishWrite(std::size_t index, Fooyin::WriteResult result)
    {
        promises.at(index)->addResult(result);
        promises.at(index)->finish();
    }

    QObject* ownerObject()
    {
        return owner.get();
    }

    FooyinLibraryApi api;
    std::unique_ptr<QObject> owner{std::make_unique<QObject>()};
    std::shared_ptr<Fooyin::TrackMetadataStore> store{std::make_shared<Fooyin::TrackMetadataStore>()};
    Fooyin::TrackList residents;
    std::vector<Fooyin::TrackList> writeInputs;
    std::vector<std::shared_ptr<QPromise<Fooyin::WriteResult>>> promises;
    std::size_t promiseIndex{0};
    int snapshotCalls{0};
    int writeCalls{0};
    int cancelCalls{0};
    bool invalidWriteRequest{false};
    int snapshotDelayMs{0};
    std::function<void()> snapshotHook;
    std::function<void()> writeHook;
};

struct PlaylistBoundary
{
    PlaylistBoundary()
    {
        api.owner           = owner.get();
        api.ownersAvailable = [this]() {
            return owner != nullptr;
        };
        api.inspect = [this](const Fooyin::UId& id) {
            inspected.append(id);
            if(!owner) {
                return BandcampPlaylistAvailability::OwnerUnavailable;
            }
            return availability.value(id.toString(QUuid::WithoutBraces), BandcampPlaylistAvailability::Missing);
        };
        api.createNew = [this](const QString& name) -> std::optional<Fooyin::UId> {
            ++createCalls;
            names.append(name);
            if(failCreate) {
                return std::nullopt;
            }
            const Fooyin::UId id = Fooyin::UId::create();
            availability.insert(id.toString(QUuid::WithoutBraces), BandcampPlaylistAvailability::Writable);
            if(createHook) {
                createHook();
            }
            return id;
        };
        api.insert = [this](const Fooyin::UId& id, const Fooyin::TrackList& tracks) {
            insertedPlaylists.append(id);
            blocks.append(tracks);
            if(insertHook) {
                insertHook();
            }
        };
    }

    Fooyin::UId addPlaylist(BandcampPlaylistAvailability state)
    {
        const Fooyin::UId id = Fooyin::UId::create();
        availability.insert(id.toString(QUuid::WithoutBraces), state);
        return id;
    }

    void supplyResidents(FooyinLibraryApi& libraryApi)
    {
        libraryApi.playlistOwner  = owner.get();
        libraryApi.playlistTracks = [this]() {
            ++snapshotCalls;
            Fooyin::TrackList snapshot;
            for(const Fooyin::TrackList& playlist : residents) {
                snapshot.insert(snapshot.end(), playlist.cbegin(), playlist.cend());
            }
            return snapshot;
        };
    }

    BandcampPlaylistApi api;
    std::unique_ptr<QObject> owner{std::make_unique<QObject>()};
    QHash<QString, BandcampPlaylistAvailability> availability;
    QList<Fooyin::UId> inspected;
    QList<Fooyin::UId> insertedPlaylists;
    QList<Fooyin::TrackList> blocks;
    QList<Fooyin::TrackList> residents;
    QStringList names;
    int snapshotCalls{0};
    int createCalls{0};
    bool failCreate{false};
    std::function<void()> createHook;
    std::function<void()> insertHook;
};

BandcampResolvedTrack importTrack(qint64 id, const QString& title, const QString& sourceUrl,
                                  std::optional<BandcampAlbumMetadata> album = std::nullopt)
{
    BandcampTrack track;
    track.id          = id;
    track.title       = title;
    track.artist      = u"Recording artist"_s;
    track.sourceUrl   = sourceUrl;
    track.streamUrl   = u"https://t4.bcbits.com/stream/0123456789abcdef0123456789abcdef?token=secret&ts=1793621993"_s;
    track.trackNumber = 1;
    return {std::move(track), std::move(album)};
}

BandcampAlbumMetadata importAlbumMetadata()
{
    BandcampAlbumMetadata metadata;
    metadata.id         = 70;
    metadata.title      = u"Admitted album"_s;
    metadata.artist     = u"Album artist"_s;
    metadata.sourceUrl  = AlbumSource;
    metadata.trackTotal = 2;
    return metadata;
}

BandcampScopeResult successfulAlbumScope(BandcampOperationId operationId, qsizetype ordinal = 0)
{
    BandcampAlbum album;
    album.metadata = importAlbumMetadata();
    album.tracks.append(importTrack(71, u"First track"_s, AlbumSource).track);
    album.tracks.append(importTrack(72, u"Second track"_s, AlbumSource).track);
    return {operationId,
            ordinal,
            {BandcampPageKind::Album, AlbumSource, {}},
            ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(album)}}};
}

BandcampScopeResult successfulTrackScope(BandcampOperationId operationId, bool albumMember = false,
                                         qsizetype ordinal = 0)
{
    std::optional<BandcampAlbumMetadata> album;
    if(albumMember) {
        album = importAlbumMetadata();
    }
    const QString sourceUrl = albumMember ? AlbumSource : TrackSource;
    return {operationId,
            ordinal,
            {BandcampPageKind::Track, TrackSource, {}},
            ResolutionSuccess<BandcampResolvedData>{
                BandcampResolvedData{importTrack(73, u"Standalone title"_s, sourceUrl, std::move(album))}}};
}

BandcampScopeResult failedScope(BandcampOperationId operationId, qsizetype ordinal = 0)
{
    return {operationId,
            ordinal,
            {BandcampPageKind::Album, AlbumSource, {}},
            ResolutionFailure{ResolutionError{ResolutionErrorCode::FetchFailed, ResolutionStage::Acquire,
                                              QUrl{AlbumSource}, 0, u"page"_s, u"fetch failed"_s}}};
}

void connectImportCompletion(BandcampImportController& controller, bool& finished, BandcampImportSummary& summary)
{
    QObject::connect(&controller, &BandcampImportController::finished, &controller,
                     [&finished, &summary](const BandcampImportSummary& value) {
                         finished = true;
                         summary  = value;
                     });
}

void submit(BandcampLibraryGateway& gateway, QList<BandcampResolvedTrack> incoming, QObject* context,
            BandcampLibraryGateway::Completion completion, int timeoutMs = 1000, CorrelationContext correlation = {})
{
    gateway.commit(std::move(incoming), context, std::move(correlation), std::move(completion), timeoutMs);
}

} // namespace

class FooyinTrackTests : public QObject
{
    Q_OBJECT

private slots:
    void canonicalTrackUriRoundTrips();
    void rejectsMalformedTrackUris();
    void projectsTrustedAlbumMetadataWithoutInference();
    void projectsOnlyObservedStreamProperties();
    void partialStreamObservationsPreserveSourceBitDepth_data();
    void partialStreamObservationsPreserveSourceBitDepth();
    void reimportRepairsPersistedLossyBitDepth_data();
    void reimportRepairsPersistedLossyBitDepth();
    void standaloneProjectionLeavesParentFieldsAbsent();
    void parsesOnlyObservedBandcampExpiryForm();
    void preservesLargeRepeatedInputOccurrences();
    void mergesOnlyTransportAndPreservesResidentMetadata_data();
    void mergesOnlyTransportAndPreservesResidentMetadata();
    void mergesObservedStreamPropertiesAndPreservesResidentState();
    void updatesStreamPropertiesFromFirstPlaylistResident();
    void logsStreamPropertyDeltasAtDebugWithoutTrace();
    void serializesSharedGatewaySnapshotsAcrossWrites();
    void cancelsOnlyTheRequestedGatewayWrite();
    void selectsLibraryThenFirstPlaylistOccurrence();
    void rejectsConflictingPersistedTrackIdMirror_data();
    void rejectsConflictingPersistedTrackIdMirror();
    void reportsWriteFailuresWithoutMutatingResidents();
    void serializesOnlyWithinOneSubmission_data();
    void serializesOnlyWithinOneSubmission();
    void timeoutDoesNotPoisonQueuedCommit();
    void activeDeadlineIncludesSnapshotWork();
    void guardsCallbacksAndObjectLifetimes();
    void correlatesAndRedactsEffectiveTransportChanges();
    void importsIntoCapturedUiPlaylist();
    void reportsCreatedPlaylistForOpeningAfterImport();
    void importsPlaylistOnlyResidentMetadata();
    void reportsImportProgressAfterResolutionOrProbe_data();
    void reportsImportProgressAfterResolutionOrProbe();
    void countsFailedAndCancelledReleasesAsCompleted();
    void createsNewForForcedAndInitiallyUnsuitableDestinations();
    void preservesOrderedAlbumBlocksAndRepeatedScopes();
    void advancesAfterFailuresAndLeavesEmptyImportsUncreated();
    void stopsWhenDestinationChangesDuringPreparation();
    void waitsForGatewayBeforeFinishingResolutionImport();
    void cancellationWithdrawsPendingGatewayHandoff();
    void unavailableProbeFallsBackAndLogsOncePerTrack();
    void failedProbesStillAdmitResolvedScope();
    void capacityRejectedProbeRetriesWhenSlotFrees();
    void cancellationDuringProbeSkipsResolvedScope_data();
    void cancellationDuringProbeSkipsResolvedScope();
    void cancellationDuringProbePreservesAlreadyInsertedScopes();
    void deletingControllerFromPlaylistCallbackWithdrawsResolution();
    void cancellationDuringPlaylistCreationDoesNotInsert();
    void cancellationDuringInsertionDefersSummaryAndCountsAcceptedBlock();
    void deletingControllerFromIntegrationErrorWithdrawsResolution();
    void resolvesUniqueBandcampReleaseUrls();
};

void FooyinTrackTests::canonicalTrackUriRoundTrips()
{
    QCOMPARE(bandcampTrackUri(1), u"bandcamp://track/1"_s);
    QCOMPARE(parseBandcampTrackUri(u"bandcamp://track/1"_s), std::optional<qint64>{1});

    constexpr qint64 MaxId = std::numeric_limits<qint64>::max();
    const QString maxUri   = u"bandcamp://track/9223372036854775807"_s;
    QCOMPARE(bandcampTrackUri(MaxId), maxUri);
    QCOMPARE(parseBandcampTrackUri(maxUri), std::optional<qint64>{MaxId});
    QVERIFY(bandcampTrackUri(0).isEmpty());
    QVERIFY(bandcampTrackUri(-1).isEmpty());
}

void FooyinTrackTests::rejectsMalformedTrackUris()
{
    const QStringList invalid{
        {},
        u"bandcamp://track/0"_s,
        u"bandcamp://track/01"_s,
        u"bandcamp://track/+1"_s,
        u"bandcamp://track/-1"_s,
        u"Bandcamp://track/1"_s,
        u"bandcamp://Track/1"_s,
        u"bandcamp://track/9223372036854775808"_s,
        u"bandcamp://track/1/2"_s,
        u"bandcamp://track/1/"_s,
        u"bandcamp://track/1?"_s,
        u"bandcamp://track/1?x=1"_s,
        u"bandcamp://track/1#"_s,
        u"bandcamp://track/1#fragment"_s,
        u"bandcamp://track:443/1"_s,
        u"bandcamp://user@track/1"_s,
        u"bandcamp://track/%31"_s,
        u"bandcamp://track/1%2F2"_s,
        u"bandcamp://track/１２"_s,
        u"bandcamp://album/1"_s,
    };
    for(const QString& uri : invalid) {
        QVERIFY2(!parseBandcampTrackUri(uri).has_value(), qPrintable(uri));
    }
}

void FooyinTrackTests::projectsTrustedAlbumMetadataWithoutInference()
{
    const Fooyin::Track track = project(memberTrack());
    QCOMPARE(track.filepath(), u"bandcamp://track/42"_s);
    QCOMPARE(track.title(), u"  Recording title  "_s);
    QCOMPARE(track.artist(), u"Recording artist"_s);
    QCOMPARE(track.album(), u"Containing album"_s);
    QCOMPARE(track.albumArtist(), u"Album artist"_s);
    QCOMPARE(track.date(), u"2018-05-06"_s);
    QCOMPARE(track.year(), 2018);
    QCOMPARE(track.trackNumber(), u"4"_s);
    QCOMPARE(track.trackTotal(), u"8"_s);
    QCOMPARE(track.duration(), uint64_t{12345});
    QVERIFY(track.extraTag(u"COVERART"_s).isEmpty());
    QVERIFY(track.metadataWasRead());
    QVERIFY(!track.hash().isEmpty());
    QVERIFY(!track.hasExtraProperty(BandcampTrackIdProperty));

    const auto properties = track.extraProperties();
    QCOMPARE(properties.value(StreamProviderProperty), u"bandcamp"_s);
    QCOMPARE(properties.value(BandcampSourceUrlProperty), AlbumSource);
    QCOMPARE(properties.value(BandcampStreamUrlProperty), SignedStream);
    QCOMPARE(properties.value(BandcampExpiresAtProperty), u"1793621993000"_s);

    auto otherScope        = memberTrack();
    otherScope.track.title = u"Other selection title"_s;
    QCOMPARE(project(otherScope).filepath(), track.filepath());

    auto missingTrackDate              = memberTrack();
    missingTrackDate.track.releaseDate = {};
    QVERIFY(project(missingTrackDate).date().isEmpty());
}

void FooyinTrackTests::projectsOnlyObservedStreamProperties()
{
    BandcampImportTrack imported{memberTrack(), {}};
    imported.streamProperties.codec      = u"AAC"_s;
    imported.streamProperties.sampleRate = 48'000;
    imported.streamProperties.channels   = 2;
    Fooyin::Track track                  = makeFooyinImportTrack(imported);

    QCOMPARE(track.codec(), u"AAC"_s);
    QCOMPARE(track.sampleRate(), 48'000);
    QCOMPARE(track.channels(), 2);
    QVERIFY(track.bitDepth() <= 0);
    QCOMPARE(track.bitrate(), 0);

    BandcampStreamProperties partial;
    partial.bitDepth = 24;
    applyBandcampStreamProperties(track, partial);
    QCOMPARE(bandcampStreamProperties(track).bitDepth, 24);
}

void FooyinTrackTests::partialStreamObservationsPreserveSourceBitDepth_data()
{
    QTest::addColumn<QString>("residentEncoding");
    QTest::addColumn<QString>("observedEncoding");
    QTest::addColumn<int>("residentDepth");
    QTest::addColumn<int>("observedDepth");
    QTest::addColumn<int>("expectedDepth");
    QTest::newRow("lossy-depth-only") << u"Lossy"_s << QString{} << 32 << 32 << -1;
    QTest::newRow("lossy-encoding-only") << QString{} << u"Lossy"_s << 32 << 0 << -1;
    QTest::newRow("lossy-empty-observation") << u"Lossy"_s << QString{} << 32 << 0 << -1;
    QTest::newRow("lossless-depth-only") << u"Lossless"_s << QString{} << 16 << 24 << 24;
    QTest::newRow("lossless-encoding-only") << QString{} << u"Lossless"_s << 24 << 0 << 24;
    QTest::newRow("lossless-empty-observation") << u"Lossless"_s << QString{} << 24 << 0 << 24;
    QTest::newRow("lossy-to-lossless") << u"Lossy"_s << u"Lossless"_s << -1 << 24 << 24;
    QTest::newRow("unclassified") << QString{} << QString{} << 24 << 0 << 24;
}

void FooyinTrackTests::partialStreamObservationsPreserveSourceBitDepth()
{
    QFETCH(QString, residentEncoding);
    QFETCH(QString, observedEncoding);
    QFETCH(int, residentDepth);
    QFETCH(int, observedDepth);
    QFETCH(int, expectedDepth);
    Fooyin::Track track = residentWithUserEdits(701);
    track.setEncoding(residentEncoding);
    track.setBitDepth(residentDepth);
    BandcampStreamProperties properties;
    properties.encoding = observedEncoding;
    properties.bitDepth = observedDepth;
    applyBandcampStreamProperties(track, properties);
    QCOMPARE(track.bitDepth(), expectedDepth);
    QCOMPARE(track.title(), u"User-edited title"_s);
    QCOMPARE(track.hash(), u"user-stable-hash"_s);
    Fooyin::ScriptRegistry registry;
    registry.finalise();
    Fooyin::ScriptParser scripting{&registry};
    const QString displayedDepth = expectedDepth > 0 ? QString::number(expectedDepth) : QString{};
    QCOMPARE(track.techInfo(u"BITDEPTH"_s), displayedDepth);
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, track), displayedDepth);
}

void FooyinTrackTests::reimportRepairsPersistedLossyBitDepth_data()
{
    QTest::addColumn<bool>("playlistOnly");
    QTest::addColumn<bool>("observeEncoding");
    QTest::newRow("library-failed-probe") << false << false;
    QTest::newRow("playlist-failed-probe") << true << false;
    QTest::newRow("library-observed-lossy") << false << true;
    QTest::newRow("playlist-observed-lossy") << true << true;
}

void FooyinTrackTests::reimportRepairsPersistedLossyBitDepth()
{
    QFETCH(bool, playlistOnly);
    QFETCH(bool, observeEncoding);
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.supplyResidents(library.api);
    Fooyin::Track resident = residentWithUserEdits(702);
    resident.setCodec(u"MP3"_s);
    resident.setEncoding(observeEncoding ? QString{} : u"Lossy"_s);
    resident.setBitDepth(32);
    if(playlistOnly) {
        playlists.residents = {{resident}};
    }
    else {
        library.residents = {resident};
    }
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    BandcampImportTrack imported{memberTrack(), {}};
    if(observeEncoding) {
        imported.streamProperties.encoding = u"Lossy"_s;
        imported.streamProperties.bitDepth = 32;
    }
    gateway.commitImport({imported}, &receiver, {.trackId = 42}, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    });
    QTRY_COMPARE(library.writeCalls, 1);
    const Fooyin::Track written = library.writeInputs.front().front();
    QCOMPARE(written.id(), resident.id());
    QCOMPARE(written.bitDepth(), -1);
    QCOMPARE(written.encoding(), u"Lossy"_s);
    QCOMPARE(written.title(), resident.title());
    QCOMPARE(written.album(), resident.album());
    QCOMPARE(written.extraTag(u"MOOD"_s), resident.extraTag(u"MOOD"_s));
    QCOMPARE(written.extraProperties().value(u"_USER_PROPERTY"_s), u"retain me"_s);
    QCOMPARE(written.hash(), resident.hash());
    Fooyin::ScriptRegistry registry;
    registry.finalise();
    Fooyin::ScriptParser scripting{&registry};
    QCOMPARE(written.techInfo(u"BITDEPTH"_s), QString{});
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, written), QString{});
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));
    QCOMPARE(std::get<FooyinTrackCommitSuccess>(outcome).tracks.front().bitDepth(), -1);
    QCOMPARE(resident.bitDepth(), 32);
}

void FooyinTrackTests::standaloneProjectionLeavesParentFieldsAbsent()
{
    BandcampResolvedTrack input = standaloneTrack();
    input.track.trackNumber     = 0;
    input.track.durationMs      = 0;
    input.track.releaseDate     = {};
    const Fooyin::Track track   = project(input);

    QCOMPARE(track.filepath(), u"bandcamp://track/43"_s);
    QCOMPARE(track.title(), u"Standalone title"_s);
    QCOMPARE(track.artist(), u"Standalone artist"_s);
    QVERIFY(track.album().isEmpty());
    QVERIFY(track.albumArtist().isEmpty());
    QVERIFY(track.date().isEmpty());
    QVERIFY(track.trackNumber().isEmpty());
    QVERIFY(track.trackTotal().isEmpty());
    QCOMPARE(track.duration(), uint64_t{0});
    QCOMPARE(track.extraProperties().value(BandcampSourceUrlProperty), TrackSource);
}

void FooyinTrackTests::parsesOnlyObservedBandcampExpiryForm()
{
    QCOMPARE(bandcampStreamExpiryMs(SignedStream), std::optional<qint64>{1793621993000LL});
    QVERIFY(!bandcampStreamExpiryMs(u"https://cdn.example.com/audio.mp3?ts=1793621993"_s));
    QVERIFY(!bandcampStreamExpiryMs(u"https://t4.bcbits.com/audio/abc?ts=1793621993"_s));
    QVERIFY(!bandcampStreamExpiryMs(u"https://t4.bcbits.com/stream/abc?ts=not-a-number"_s));
    QVERIFY(!bandcampStreamExpiryMs(u"https://t4.bcbits.com/stream/abc?ts=1793621993&ts=1793622993"_s));
    QVERIFY(!bandcampStreamExpiryMs(u"https://t4.bcbits.com/stream/abc?ts=9223372036854776"_s));
}

void FooyinTrackTests::preservesLargeRepeatedInputOccurrences()
{
    LibraryBoundary library;
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    QList<BandcampResolvedTrack> incoming(600, standaloneTrack());
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    submit(gateway, std::move(incoming), &receiver, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    });

    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));
    const auto& success = std::get<FooyinTrackCommitSuccess>(outcome);
    QCOMPARE(success.tracks.size(), std::size_t{600});
    QCOMPARE(success.newIdentityCount, qsizetype{1});
    QCOMPARE(success.updatedResidentCount, qsizetype{0});
    QVERIFY(success.tracks.front().id() <= 0);
    QCOMPARE(success.tracks.front().filepath(), u"bandcamp://track/43"_s);
    QCOMPARE(library.snapshotCalls, 1);
    QCOMPARE(library.writeCalls, 0);
}

void FooyinTrackTests::mergesOnlyTransportAndPreservesResidentMetadata_data()
{
    QTest::addColumn<bool>("playlistOnly");
    QTest::newRow("library") << false;
    QTest::newRow("playlist-only") << true;
}

void FooyinTrackTests::mergesOnlyTransportAndPreservesResidentMetadata()
{
    QFETCH(bool, playlistOnly);
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.supplyResidents(library.api);
    const Fooyin::Track resident = residentWithUserEdits(700);
    if(playlistOnly) {
        playlists.residents = {{resident}};
    }

    else {
        library.residents.push_back(resident);
    }
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    QList<BandcampResolvedTrack> incoming{memberTrack(), memberTrack(42, OtherSignedStream)};
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    CorrelationContext correlation;
    correlation.albumId = 9;
    submit(
        gateway, std::move(incoming), &receiver,
        [&](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        },
        1000, correlation);

    QTRY_COMPARE(library.writeCalls, 1);
    QCOMPARE(library.snapshotCalls, 1);
    QCOMPARE(library.writeInputs.front().size(), std::size_t{1});
    const Fooyin::Track write = library.writeInputs.front().front();
    QCOMPARE(write.id(), 700);
    QCOMPARE(write.libraryId(), 12);
    QCOMPARE(write.filepath(), u"bandcamp://track/42"_s);
    QCOMPARE(write.title(), u"User-edited title"_s);
    QCOMPARE(write.artists(), QStringList{u"User-edited artist"_s});
    QCOMPARE(write.album(), u"User-edited album"_s);
    QCOMPARE(write.albumArtists(), QStringList{u"User-edited album artist"_s});
    QCOMPARE(write.date(), u"2023-02-01"_s);
    QCOMPARE(write.trackNumber(), u"19"_s);
    QCOMPARE(write.trackTotal(), u"21"_s);
    QCOMPARE(write.playCount(), 6);
    QCOMPARE(write.rating(), 0.8F);
    QVERIFY(write.isLoved());
    QCOMPARE(write.firstPlayed(), uint64_t{1234});
    QCOMPARE(write.lastPlayed(), uint64_t{5678});
    QCOMPARE(write.extraTag(u"MOOD"_s), QStringList{u"User mood"_s});
    QCOMPARE(write.extraTag(u"COVERART"_s), QStringList{u"https://covers.example.org/user-art.jpg"_s});
    QCOMPARE(write.hash(), u"user-stable-hash"_s);
    QCOMPARE(write.extraProperties().value(u"_USER_PROPERTY"_s), u"retain me"_s);
    QCOMPARE(write.extraProperties().value(BandcampStreamUrlProperty), SignedStream);

    // The boundary supplies only the async result; the plugin never mutates its resident snapshot.
    const Fooyin::Track& unchanged = playlistOnly ? playlists.residents.first().front() : library.residents.front();
    QCOMPARE(unchanged.extraProperties().value(BandcampStreamUrlProperty),
             u"https://cdn.example.com/old.mp3?sig=old"_s);
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));
    const auto& result = std::get<FooyinTrackCommitSuccess>(outcome);
    QCOMPARE(result.updatedResidentCount, qsizetype{1});
    QCOMPARE(result.tracks.size(), std::size_t{2});
    QCOMPARE(result.tracks.at(0).id(), 700);
    QCOMPARE(result.tracks.at(0).title(), u"User-edited title"_s);
    QCOMPARE(result.tracks.at(0).extraTag(u"COVERART"_s), QStringList{u"https://covers.example.org/user-art.jpg"_s});
    QCOMPARE(result.tracks.at(0).extraProperties().value(BandcampStreamUrlProperty), SignedStream);
    QCOMPARE(result.tracks.at(1).extraProperties().value(BandcampStreamUrlProperty), OtherSignedStream);
}

void FooyinTrackTests::mergesObservedStreamPropertiesAndPreservesResidentState()
{
    LibraryBoundary library;
    Fooyin::Track resident = residentWithUserEdits(707);
    resident.setBitDepth(24);
    resident.setBitrate(320);
    resident.setEncoding(u"User encoding"_s);
    resident.clearWasModified();
    library.residents.push_back(resident);
    library.makeWriteAvailable();

    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    BandcampImportTrack imported{memberTrack(), {}};
    imported.streamProperties.codec      = u"AAC"_s;
    imported.streamProperties.sampleRate = 48'000;
    imported.streamProperties.channels   = 2;
    gateway.commitImport({imported}, &receiver, {.trackId = 42}, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    });

    QTRY_COMPARE(library.writeCalls, 1);
    const Fooyin::Track written = library.writeInputs.front().front();
    QCOMPARE(written.title(), u"User-edited title"_s);
    QCOMPARE(written.extraTag(u"MOOD"_s), QStringList{u"User mood"_s});
    QCOMPARE(written.extraProperties().value(BandcampStreamUrlProperty), SignedStream);
    QCOMPARE(written.codec(), u"AAC"_s);
    QCOMPARE(written.sampleRate(), 48'000);
    QCOMPARE(written.channels(), 2);
    QCOMPARE(written.bitDepth(), 24);
    QCOMPARE(written.bitrate(), 320);
    QCOMPARE(written.encoding(), u"User encoding"_s);

    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));
}

void FooyinTrackTests::updatesStreamPropertiesFromFirstPlaylistResident()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.supplyResidents(library.api);
    Fooyin::Track first  = residentWithUserEdits(711);
    Fooyin::Track second = residentWithUserEdits(712);
    second.setTitle(u"Second playlist occurrence"_s);
    playlists.residents = {{first, second}};
    library.makeWriteAvailable();

    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    BandcampStreamProperties properties;
    properties.codec      = u"FLAC"_s;
    properties.sampleRate = 96'000;
    gateway.updateStreamProperties(42, properties, &receiver, {.trackId = 42}, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    });

    QTRY_COMPARE(library.writeCalls, 1);
    QCOMPARE(playlists.snapshotCalls, 1);
    QCOMPARE(library.writeInputs.front().size(), std::size_t{1});
    const Fooyin::Track written = library.writeInputs.front().front();
    QCOMPARE(written.id(), 711);
    QCOMPARE(written.filepath(), u"bandcamp://track/42"_s);
    QCOMPARE(written.title(), u"User-edited title"_s);
    QCOMPARE(written.extraProperties().value(BandcampStreamUrlProperty), u"https://cdn.example.com/old.mp3?sig=old"_s);
    QCOMPARE(written.codec(), u"FLAC"_s);
    QCOMPARE(written.sampleRate(), 96'000);

    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));
}

void FooyinTrackTests::logsStreamPropertyDeltasAtDebugWithoutTrace()
{
    LibraryBoundary library;
    Fooyin::Track resident = residentWithUserEdits(713);
    library.residents      = {resident};
    library.makeWriteAvailable();

    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    BandcampStreamProperties properties;
    properties.codec      = u"FLAC"_s;
    properties.sampleRate = 96'000;

    LogCapture logs{u"fy.bandcamp.tracks.debug=true\nfy.trace.bandcamp.tracks.debug=false"_s};
    gateway.updateStreamProperties(42, properties, &receiver, {.trackId = 42}, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    });

    QTRY_COMPARE(library.writeCalls, 1);
    const auto captured  = capturedLogsSnapshot();
    const auto deltaLogs = std::count_if(captured.cbegin(), captured.cend(), [](const CapturedLog& log) {
        return log.category == u"fy.bandcamp.tracks"_s
            && log.message.startsWith(u"track=42 stream_properties_updated"_s);
    });
    QCOMPARE(deltaLogs, 1);
    const auto delta = std::find_if(captured.cbegin(), captured.cend(), [](const CapturedLog& log) {
        return log.category == u"fy.bandcamp.tracks"_s
            && log.message.startsWith(u"track=42 stream_properties_updated"_s);
    });
    QVERIFY(delta != captured.cend());
    QVERIFY(delta->message.contains(u"properties=codec,sample_rate"_s));
    QVERIFY(std::none_of(captured.cbegin(), captured.cend(),
                         [](const CapturedLog& log) { return log.category == u"fy.trace.bandcamp.tracks"_s; }));

    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));
}

void FooyinTrackTests::serializesSharedGatewaySnapshotsAcrossWrites()
{
    LibraryBoundary library;
    Fooyin::Track resident = residentWithUserEdits(714);
    resident.setBitrate(320);
    library.residents = {resident};
    library.makeWriteAvailable();
    library.makeWriteAvailable();

    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    bool refreshCompleted    = false;
    bool propertiesCompleted = false;
    FooyinTrackCommitOutcome refreshOutcome;
    FooyinTrackCommitOutcome propertiesOutcome;
    BandcampResolvedTrack refreshed = memberTrack(42, OtherSignedStream);
    gateway.refreshTransport(refreshed, &receiver, {.trackId = 42}, [&](FooyinTrackCommitOutcome result) {
        refreshOutcome   = std::move(result);
        refreshCompleted = true;
    });
    BandcampStreamProperties properties;
    properties.codec   = u"AAC"_s;
    properties.bitrate = 256;
    gateway.updateStreamProperties(42, properties, &receiver, {.trackId = 42}, [&](FooyinTrackCommitOutcome result) {
        propertiesOutcome   = std::move(result);
        propertiesCompleted = true;
    });

    QTRY_COMPARE(library.writeCalls, 1);
    const Fooyin::Track refreshedSnapshot = library.writeInputs.front().front();
    QCOMPARE(refreshedSnapshot.extraProperties().value(BandcampStreamUrlProperty), OtherSignedStream);
    QCOMPARE(refreshedSnapshot.bitrate(), 320);
    library.residents = {refreshedSnapshot};
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});

    QTRY_COMPARE(library.writeCalls, 2);
    const Fooyin::Track propertiesSnapshot = library.writeInputs.back().front();
    QCOMPARE(propertiesSnapshot.extraProperties().value(BandcampStreamUrlProperty), OtherSignedStream);
    QCOMPARE(propertiesSnapshot.codec(), u"AAC"_s);
    QCOMPARE(propertiesSnapshot.bitrate(), 256);
    QCOMPARE(propertiesSnapshot.title(), resident.title());
    library.finishWrite(1, {Fooyin::WriteState::Completed, 1, 0});

    QTRY_VERIFY(refreshCompleted);
    QTRY_VERIFY(propertiesCompleted);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(refreshOutcome));
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(propertiesOutcome));
}

void FooyinTrackTests::cancelsOnlyTheRequestedGatewayWrite()
{
    LibraryBoundary library;
    library.residents = {residentWithUserEdits(715)};
    library.makeWriteAvailable();

    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    bool firstCompleted  = false;
    bool secondCompleted = false;
    FooyinTrackCommitOutcome firstOutcome;
    FooyinTrackCommitOutcome secondOutcome;
    BandcampStreamProperties firstProperties;
    firstProperties.codec    = u"FLAC"_s;
    const quint64 firstToken = gateway.updateStreamProperties(42, firstProperties, &receiver, {.trackId = 42},
                                                              [&](FooyinTrackCommitOutcome result) {
                                                                  firstOutcome   = std::move(result);
                                                                  firstCompleted = true;
                                                              });
    BandcampStreamProperties secondProperties;
    secondProperties.codec    = u"AAC"_s;
    const quint64 secondToken = gateway.updateStreamProperties(42, secondProperties, &receiver, {.trackId = 42},
                                                               [&](FooyinTrackCommitOutcome result) {
                                                                   secondOutcome   = std::move(result);
                                                                   secondCompleted = true;
                                                               });
    QVERIFY(firstToken > 0);
    QVERIFY(secondToken > firstToken);
    gateway.cancelRequest(firstToken);

    QTRY_COMPARE(library.writeCalls, 1);
    QCOMPARE(library.writeInputs.front().front().codec(), u"AAC"_s);
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(firstCompleted);
    QTRY_VERIFY(secondCompleted);
    QCOMPARE(std::get<FooyinTrackCommitFailure>(firstOutcome).code, FooyinTrackCommitErrorCode::Cancelled);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(secondOutcome));
}

void FooyinTrackTests::selectsLibraryThenFirstPlaylistOccurrence()
{
    for(const bool libraryPresent : {false, true}) {
        LibraryBoundary library;
        PlaylistBoundary playlists;
        playlists.supplyResidents(library.api);
        Fooyin::Track first = residentWithUserEdits(700);
        first.setTitle(u"First playlist occurrence"_s);
        Fooyin::Track laterOccurrence = first;
        laterOccurrence.setTitle(u"Later occurrence"_s);
        Fooyin::Track laterPlaylist = first;
        laterPlaylist.setTitle(u"Later playlist"_s);
        playlists.residents               = {{project(standaloneTrack()), first, laterOccurrence}, {laterPlaylist}};
        const Fooyin::Track authoritative = residentWithUserEdits(700);
        if(libraryPresent) {
            library.residents = {authoritative};
        }
        library.makeWriteAvailable();
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {memberTrack(), memberTrack(42, OtherSignedStream)}, &receiver,
               [&](FooyinTrackCommitOutcome value) {
                   outcome   = std::move(value);
                   completed = true;
               });

        const QString expectedTitle = libraryPresent ? authoritative.title() : first.title();
        QTRY_COMPARE(library.writeCalls, 1);
        QCOMPARE(library.writeInputs.front().size(), std::size_t{1});
        QCOMPARE(library.writeInputs.front().front().title(), expectedTitle);
        library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
        QTRY_VERIFY(completed);
        const auto* result = std::get_if<FooyinTrackCommitSuccess>(&outcome);
        QVERIFY(result);
        QCOMPARE(result->tracks.size(), std::size_t{2});
        for(const auto& track : result->tracks) {
            QCOMPARE(track.title(), expectedTitle);
            QCOMPARE(track.id(), 700);
        }
        QCOMPARE(result->newIdentityCount, qsizetype{0});
    }
}

void FooyinTrackTests::rejectsConflictingPersistedTrackIdMirror_data()
{
    mergesOnlyTransportAndPreservesResidentMetadata_data();
}

void FooyinTrackTests::rejectsConflictingPersistedTrackIdMirror()
{
    QFETCH(bool, playlistOnly);
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.supplyResidents(library.api);
    Fooyin::Track resident = residentWithUserEdits(710);
    resident.setExtraProperty(BandcampTrackIdProperty, u"43"_s);
    if(playlistOnly) {
        playlists.residents = {{resident}};
    }
    else {
        library.residents.push_back(resident);
    }
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    LogCapture capture(u"fy.trace.bandcamp.tracks.debug=true"_s);

    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    CorrelationContext correlation;
    correlation.albumId = 31;
    submit(
        gateway, {memberTrack()}, &receiver,
        [&](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        },
        1000, correlation);

    QTRY_VERIFY(completed);
    const auto* error = std::get_if<FooyinTrackCommitFailure>(&outcome);
    QVERIFY(error);
    QCOMPARE(error->code, FooyinTrackCommitErrorCode::InvalidIdentity);
    QCOMPARE(error->trackId, qint64{42});
    QCOMPARE(library.writeCalls, 0);

    bool foundFailure = false;
    for(const CapturedLog& message : capturedLogs()) {
        if(message.category == u"fy.bandcamp.tracks"_s && message.message.contains(u"track_commit_failed"_s)) {
            foundFailure = true;
            QVERIFY(message.message.startsWith(u"album=31 track=42 "_s));
            QVERIFY(message.message.contains(u"track_commit_failed "_s));
        }
    }
    QVERIFY(foundFailure);
}

void FooyinTrackTests::reportsWriteFailuresWithoutMutatingResidents()
{
    const QList<Fooyin::WriteResult> failedResults{
        {Fooyin::WriteState::Completed, 0, 0},
        {Fooyin::WriteState::Completed, 1, 1},
        {Fooyin::WriteState::Cancelled, 1, 0},
    };
    for(const Fooyin::WriteResult& writeResult : failedResults) {
        LibraryBoundary library;
        Fooyin::Track resident = residentWithUserEdits(701);
        library.residents.push_back(resident);
        library.makeWriteAvailable();
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;

        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        });
        QTRY_COMPARE(library.writeCalls, 1);
        library.finishWrite(0, writeResult);
        QTRY_VERIFY(completed);
        QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(outcome));
        QCOMPARE(library.residents.front().extraProperties().value(BandcampStreamUrlProperty),
                 resident.extraProperties().value(BandcampStreamUrlProperty));
    }

    LibraryBoundary retryLibrary;
    Fooyin::Track resident = residentWithUserEdits(702);
    retryLibrary.residents.push_back(resident);
    retryLibrary.makeWriteAvailable();
    retryLibrary.makeWriteAvailable();
    BandcampLibraryGateway retryGateway{retryLibrary.api};
    QObject retryReceiver;
    bool firstDone  = false;
    bool secondDone = false;
    FooyinTrackCommitOutcome first;
    FooyinTrackCommitOutcome second;

    auto retryRequest = [&](bool& completed, FooyinTrackCommitOutcome& outcome) {
        submit(retryGateway, {memberTrack()}, &retryReceiver, [&completed, &outcome](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        });
    };
    retryRequest(firstDone, first);
    QTRY_COMPARE(retryLibrary.writeCalls, 1);
    retryLibrary.finishWrite(0, {Fooyin::WriteState::Completed, 0, 0});
    QTRY_VERIFY(firstDone);
    QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(first));

    retryRequest(secondDone, second);
    QTRY_COMPARE(retryLibrary.writeCalls, 2);
    retryLibrary.finishWrite(1, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(secondDone);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(second));
}

void FooyinTrackTests::serializesOnlyWithinOneSubmission_data()
{
    mergesOnlyTransportAndPreservesResidentMetadata_data();
}

void FooyinTrackTests::serializesOnlyWithinOneSubmission()
{
    QFETCH(bool, playlistOnly);
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.supplyResidents(library.api);
    Fooyin::Track original = residentWithUserEdits(703);
    if(playlistOnly) {
        playlists.residents = {{original}};
    }
    else {
        library.residents.push_back(original);
    }
    library.makeWriteAvailable();
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    bool firstDone  = false;
    bool secondDone = false;
    FooyinTrackCommitOutcome first;
    FooyinTrackCommitOutcome second;
    submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
        first     = std::move(result);
        firstDone = true;
    });
    submit(gateway, {memberTrack(42, OtherSignedStream)}, &receiver, [&](FooyinTrackCommitOutcome result) {
        second     = std::move(result);
        secondDone = true;
    });

    QTRY_COMPARE(library.writeCalls, 1);
    QCOMPARE(library.snapshotCalls, 1);
    QCOMPARE(playlists.snapshotCalls, 1);
    Fooyin::Track latest = library.writeInputs.front().front();
    latest.setTitle(u"Edit visible when queued work starts"_s);
    latest.replaceExtraTag(u"MOOD"_s, u"latest mood"_s);
    latest.setHash(u"latest hash"_s);
    if(playlistOnly) {
        playlists.residents = {{latest}};
    }
    else {
        library.residents = {latest};
    }
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(firstDone);
    QTRY_COMPARE(library.writeCalls, 2);
    QCOMPARE(library.snapshotCalls, 2);
    QCOMPARE(playlists.snapshotCalls, 2);
    QCOMPARE(library.writeInputs.at(1).front().title(), u"Edit visible when queued work starts"_s);
    QCOMPARE(library.writeInputs.at(1).front().extraTag(u"MOOD"_s), QStringList{u"latest mood"_s});
    QCOMPARE(library.writeInputs.at(1).front().hash(), u"latest hash"_s);
    QCOMPARE(library.writeInputs.at(1).front().extraProperties().value(BandcampStreamUrlProperty), OtherSignedStream);
    library.finishWrite(1, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(secondDone);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(first));
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(second));
    QCOMPARE(std::get<FooyinTrackCommitSuccess>(second).tracks.front().title(), latest.title());
}

void FooyinTrackTests::timeoutDoesNotPoisonQueuedCommit()
{
    LibraryBoundary library;
    library.residents.push_back(residentWithUserEdits(704));
    library.makeWriteAvailable();
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    bool timedOut  = false;
    bool completed = false;
    FooyinTrackCommitOutcome timeoutOutcome;
    FooyinTrackCommitOutcome result;
    submit(
        gateway, {memberTrack()}, &receiver,
        [&](FooyinTrackCommitOutcome value) {
            timeoutOutcome = std::move(value);
            timedOut       = true;
        },
        5);
    submit(gateway, {memberTrack(42, OtherSignedStream)}, &receiver, [&](FooyinTrackCommitOutcome value) {
        result    = std::move(value);
        completed = true;
    });

    QTRY_VERIFY(timedOut);
    QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(timeoutOutcome));
    QCOMPARE(library.writeCalls, 1);
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_COMPARE(library.writeCalls, 2);
    library.finishWrite(1, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(result));
}

void FooyinTrackTests::activeDeadlineIncludesSnapshotWork()
{
    LibraryBoundary library;
    library.snapshotDelayMs = 30;
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    submit(
        gateway, {standaloneTrack()}, &receiver,
        [&](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        },
        1);

    QTRY_VERIFY(completed);
    const auto* error = std::get_if<FooyinTrackCommitFailure>(&outcome);
    QVERIFY(error);
    QCOMPARE(error->code, FooyinTrackCommitErrorCode::PersistenceFailed);
    QCOMPARE(library.snapshotCalls, 1);
    QCOMPARE(library.writeCalls, 0);
}

void FooyinTrackTests::guardsCallbacksAndObjectLifetimes()
{
    {
        LibraryBoundary library;
        library.residents.push_back(residentWithUserEdits(704));
        library.makeWriteAvailable();
        BandcampLibraryGateway gateway{library.api};
        library.writeHook = [&gateway]() {
            gateway.cancel();
        };
        QObject receiver;
        bool called = false;
        submit(gateway, {memberTrack()}, &receiver, [&called](FooyinTrackCommitOutcome) { called = true; });
        QTRY_COMPARE(library.writeCalls, 1);
        QCOMPARE(library.cancelCalls, 1);
        library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
        QTest::qWait(10);
        QVERIFY(!called);
    }

    {
        LibraryBoundary library;
        PlaylistBoundary playlists;
        playlists.supplyResidents(library.api);
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool called = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome value) {
            outcome = std::move(value);
            called  = true;
        });
        playlists.owner.reset();
        QTRY_VERIFY(called);
        const auto* error = std::get_if<FooyinTrackCommitFailure>(&outcome);
        QVERIFY(error);
        QCOMPARE(error->code, FooyinTrackCommitErrorCode::LibraryUnavailable);
        QCOMPARE(playlists.snapshotCalls, 0);
        QCOMPARE(library.snapshotCalls, 0);
        QCOMPARE(library.writeCalls, 0);
    }

    {
        LibraryBoundary library;
        BandcampLibraryGateway gateway{library.api};
        auto* receiver = new QObject;
        bool called    = false;
        submit(gateway, {standaloneTrack()}, receiver, [&called](FooyinTrackCommitOutcome) { called = true; });
        delete receiver;
        QTest::qWait(10);
        QVERIFY(!called);
        QCOMPARE(library.writeCalls, 0);
    }

    {
        LibraryBoundary library;
        library.residents.push_back(residentWithUserEdits(705));
        library.makeWriteAvailable();
        auto* gateway = new BandcampLibraryGateway{library.api};
        QObject receiver;
        bool called = false;
        submit(*gateway, {memberTrack()}, &receiver, [&called](FooyinTrackCommitOutcome) { called = true; });
        QTRY_COMPARE(library.writeCalls, 1);
        delete gateway;
        QCOMPARE(library.cancelCalls, 1);
        library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
        QTest::qWait(10);
        QVERIFY(!called);
    }

    {
        LibraryBoundary library;
        library.residents.push_back(residentWithUserEdits(706));
        library.makeWriteAvailable();
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool called = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {memberTrack()}, &receiver, [&called, &outcome](FooyinTrackCommitOutcome result) {
            called  = true;
            outcome = std::move(result);
        });
        QTRY_COMPARE(library.writeCalls, 1);
        library.owner.reset();
        library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
        QTRY_VERIFY(called);
        QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(outcome));
    }

    {
        LibraryBoundary library;
        auto* gateway = new BandcampLibraryGateway{library.api};
        QObject receiver;
        bool called = false;
        submit(*gateway, {standaloneTrack()}, &receiver, [&](FooyinTrackCommitOutcome) {
            QVERIFY(QThread::currentThread() == receiver.thread());
            called = true;
            delete gateway;
            gateway = nullptr;
        });
        QTRY_VERIFY(called);
        QVERIFY(gateway == nullptr);
    }
}

void FooyinTrackTests::correlatesAndRedactsEffectiveTransportChanges()
{
    LibraryBoundary library;
    Fooyin::Track resident = residentWithUserEdits(709);
    resident.setExtraProperty(BandcampStreamUrlProperty,
                              u"https://t4.bcbits.com/stream/oldhash?token=secret-old&ts=1793621000"_s);
    library.residents.push_back(resident);
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    LogCapture capture(u"fy.trace.bandcamp.tracks.debug=true"_s);

    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    CorrelationContext correlation;
    correlation.albumId = 9;
    submit(
        gateway, {memberTrack()}, &receiver,
        [&](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        },
        1000, correlation);
    QTRY_COMPARE(library.writeCalls, 1);
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));

    bool found = false;
    for(const CapturedLog& message : capturedLogs()) {
        if(message.category == u"fy.trace.bandcamp.tracks"_s && message.message.contains(u"transport_updated"_s)) {
            found = true;
            QVERIFY(message.message.startsWith(u"album=9 track=42 transport_updated "_s));
            QVERIFY(!message.message.contains(u"secret-old"_s));
            QVERIFY(!message.message.contains(u"token=one"_s));
        }
    }
    QVERIFY(found);
}

void FooyinTrackTests::importsIntoCapturedUiPlaylist()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    const Fooyin::UId selectedPlaylist = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    const Fooyin::UId playbackPlaylist = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    BandcampImportController controller{
        1, {{BandcampPageKind::Album, AlbumSource, {}}}, false, selectedPlaylist, library.api, playlists.api, {}};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(1));
    controller.finishResolution({1, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, 1);
    QCOMPARE(playlists.createCalls, 0);
    QCOMPARE(playlists.insertedPlaylists, QList<Fooyin::UId>{selectedPlaylist});
    QVERIFY(playbackPlaylist != selectedPlaylist);
}

void FooyinTrackTests::reportsCreatedPlaylistForOpeningAfterImport()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    BandcampImportController controller{
        1, {{BandcampPageKind::Album, AlbumSource, {}}}, true, {}, library.api, playlists.api, {}};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(1));
    controller.finishResolution({1, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, 1);
    QCOMPARE(playlists.createCalls, 1);
    QVERIFY(summary.createdPlaylistId.isValid());
    QCOMPARE(playlists.insertedPlaylists, QList<Fooyin::UId>{summary.createdPlaylistId});
}

void FooyinTrackTests::importsPlaylistOnlyResidentMetadata()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.supplyResidents(library.api);
    Fooyin::Track resident = makeFooyinTrack(importTrack(71, u"User-edited title"_s, AlbumSource));
    resident.setId(700);
    resident.replaceExtraTag(u"MOOD"_s, u"User mood"_s);
    const QString incomingStream = resident.extraProperties().value(BandcampStreamUrlProperty);
    resident.setExtraProperty(BandcampStreamUrlProperty, u"https://cdn.example.com/old.mp3"_s);
    playlists.residents = {{resident}};
    library.makeWriteAvailable();
    const Fooyin::UId selected = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    BandcampImportController controller{
        1, {{BandcampPageKind::Album, AlbumSource, {}}}, false, selected, library.api, playlists.api, {}};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);
    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(1));
    controller.finishResolution({1, 1, 1, 0, 0});

    QTRY_COMPARE(library.writeCalls, 1);
    QVERIFY(playlists.blocks.isEmpty());
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(finished);
    QCOMPARE(summary.added, 1);
    QCOMPARE(playlists.insertedPlaylists, QList<Fooyin::UId>{selected});
    QCOMPARE(playlists.blocks.first().size(), std::size_t{2});
    QCOMPARE(playlists.blocks.first().at(1).filepath(), bandcampTrackUri(72));
    for(const Fooyin::Track& chosen : {library.writeInputs.front().front(), playlists.blocks.first().front()}) {
        QCOMPARE(chosen.filepath(), bandcampTrackUri(71));
        QCOMPARE(chosen.id(), 700);
        QCOMPARE(chosen.title(), resident.title());
        QCOMPARE(chosen.extraTag(u"MOOD"_s), resident.extraTag(u"MOOD"_s));
        QCOMPARE(chosen.extraProperties().value(BandcampStreamUrlProperty), incomingStream);
    }
}

void FooyinTrackTests::reportsImportProgressAfterResolutionOrProbe_data()
{
    QTest::addColumn<bool>("probeStreams");
    QTest::newRow("without-probing") << false;
    QTest::newRow("with-probing") << true;
}

void FooyinTrackTests::reportsImportProgressAfterResolutionOrProbe()
{
    QFETCH(bool, probeStreams);

    LibraryBoundary host;
    PlaylistBoundary playlists;
    playlists.supplyResidents(host.api);
    const Fooyin::UId destination = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    PendingStreamProbe probe;
    BandcampImportController controller{22, {}, false, destination, host.api, playlists.api, {}, &probe, probeStreams};
    QList<BandcampImportProgress> progressUpdates;
    QObject::connect(&controller, &BandcampImportController::progressChanged, &controller,
                     [&progressUpdates](const BandcampImportProgress& progress) { progressUpdates.append(progress); });
    controller.start();

    BandcampOperationProgress resolutionProgress;
    resolutionProgress.operationId = 22;
    resolutionProgress.phase       = BandcampOperationPhase::Resolving;
    resolutionProgress.totalWork   = 1;
    controller.acceptProgress(resolutionProgress);
    QCOMPARE(progressUpdates.last().completedWork, qsizetype{0});
    QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Resolving);

    controller.acceptScopeResult(successfulTrackScope(22));
    QCOMPARE(progressUpdates.last().completedWork, qsizetype{0});
    QCOMPARE(progressUpdates.last().totalWork, qsizetype{1});
    QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Resolving);
    QCOMPARE(playlists.blocks.size(), 0);

    if(probeStreams) {
        QVERIFY(probe.hasPending());
    }

    controller.finishResolution({22, 1, 1, 0, 0});
    if(probeStreams) {
        QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Probing);
        QCOMPARE(progressUpdates.last().completedWork, qsizetype{0});
        QCOMPARE(progressUpdates.last().totalWork, qsizetype{1});

        probe.completePending();
        QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Committing);
        QCOMPARE(progressUpdates.last().completedWork, qsizetype{0});
    }
    else {
        QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Committing);
        QCOMPARE(progressUpdates.last().completedWork, qsizetype{0});
    }

    QTRY_COMPARE_WITH_TIMEOUT(playlists.blocks.size(), 1, 3000);
    QCOMPARE(progressUpdates.last().completedWork, qsizetype{1});
    QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Committing);
}

void FooyinTrackTests::countsFailedAndCancelledReleasesAsCompleted()
{
    LibraryBoundary host;
    PlaylistBoundary playlists;
    BandcampImportController controller{23, {}, true, {}, host.api, playlists.api, {}};
    QList<BandcampImportProgress> progressUpdates;
    QObject::connect(&controller, &BandcampImportController::progressChanged, &controller,
                     [&progressUpdates](const BandcampImportProgress& progress) { progressUpdates.append(progress); });
    controller.start();

    BandcampOperationProgress resolutionProgress;
    resolutionProgress.operationId = 23;
    resolutionProgress.phase       = BandcampOperationPhase::Resolving;
    resolutionProgress.totalWork   = 2;
    controller.acceptProgress(resolutionProgress);
    controller.acceptScopeResult(failedScope(23));
    BandcampScopeResult cancelled{23, 1, {BandcampPageKind::Track, TrackSource, {}}, ResolutionCancelled{}};
    controller.acceptScopeResult(cancelled);
    QCOMPARE(progressUpdates.last().completedWork, qsizetype{0});
    QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Resolving);

    controller.finishResolution({23, 2, 0, 1, 1});
    QCOMPARE(progressUpdates.last().completedWork, qsizetype{2});
    QCOMPARE(progressUpdates.last().phase, BandcampImportPhase::Committing);
    QVERIFY(controller.isFinished());
}

void FooyinTrackTests::createsNewForForcedAndInitiallyUnsuitableDestinations()
{
    const QList<BandcampPlaylistAvailability> unsuitable{
        BandcampPlaylistAvailability::Missing,
        BandcampPlaylistAvailability::Temporary,
        BandcampPlaylistAvailability::Automatic,
        BandcampPlaylistAvailability::Locked,
    };
    for(const BandcampPlaylistAvailability state : unsuitable) {
        LibraryBoundary library;
        PlaylistBoundary playlists;
        const Fooyin::UId selected = playlists.addPlaylist(state);
        BandcampImportController controller{
            2, {{BandcampPageKind::Album, AlbumSource, {}}}, false, selected, library.api, playlists.api, {}};
        bool finished = false;
        BandcampImportSummary summary;
        connectImportCompletion(controller, finished, summary);
        controller.start();
        controller.acceptScopeResult(successfulAlbumScope(2));
        controller.finishResolution({2, 1, 1, 0, 0});

        QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
        QCOMPARE(summary.added, 1);
        QCOMPARE(playlists.createCalls, 1);
        QCOMPARE(playlists.names, QStringList{u"Admitted album"_s});
        QCOMPARE(playlists.insertedPlaylists.size(), 1);
        QVERIFY(playlists.insertedPlaylists.first() != selected);
        QCOMPARE(playlists.blocks.first().at(0).extraProperties().value(BandcampSourceUrlProperty), AlbumSource);
    }

    LibraryBoundary library;
    PlaylistBoundary playlists;
    const Fooyin::UId selected = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    BandcampImportController forced{
        3, {{BandcampPageKind::Track, TrackSource, {}}}, true, selected, library.api, playlists.api, {}};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(forced, finished, summary);
    forced.start();
    forced.acceptScopeResult(successfulTrackScope(3, true));
    forced.finishResolution({3, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, 1);
    QCOMPARE(playlists.createCalls, 1);
    QCOMPARE(playlists.names, QStringList{u"Admitted album"_s});
    QVERIFY(playlists.insertedPlaylists.first() != selected);

    LibraryBoundary standaloneLibrary;
    PlaylistBoundary standalonePlaylists;
    BandcampImportController standalone{
        13, {{BandcampPageKind::Track, TrackSource, {}}}, true, {}, standaloneLibrary.api, standalonePlaylists.api, {}};
    bool standaloneFinished = false;
    BandcampImportSummary standaloneSummary;
    connectImportCompletion(standalone, standaloneFinished, standaloneSummary);
    standalone.start();
    standalone.acceptScopeResult(successfulTrackScope(13));
    standalone.finishResolution({13, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(standaloneFinished, 2000);
    QCOMPARE(standaloneSummary.added, 1);
    QCOMPARE(standalonePlaylists.names, QStringList{u"Standalone title"_s});
}

void FooyinTrackTests::preservesOrderedAlbumBlocksAndRepeatedScopes()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    BandcampImportController controller{4,
                                        {{BandcampPageKind::Album, AlbumSource, {}},
                                         {BandcampPageKind::Album, AlbumSource, {}},
                                         {BandcampPageKind::Track, TrackSource, {}}},
                                        true,
                                        {},
                                        library.api,
                                        playlists.api,
                                        {}};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(4, 0));
    controller.acceptScopeResult(successfulAlbumScope(4, 1));
    controller.acceptScopeResult(successfulTrackScope(4, true, 2));
    controller.finishResolution({4, 3, 3, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 3000);
    QCOMPARE(summary.added, 3);
    QCOMPARE(playlists.createCalls, 1);
    QCOMPARE(playlists.names, QStringList{u"Bandcamp"_s});
    QCOMPARE(playlists.blocks.size(), 3);
    QCOMPARE(playlists.blocks.at(0).size(), 2);
    QCOMPARE(playlists.blocks.at(1).size(), 2);
    QCOMPARE(playlists.blocks.at(2).size(), 1);
    QCOMPARE(playlists.blocks.at(0).at(0).filepath(), u"bandcamp://track/71"_s);
    QCOMPARE(playlists.blocks.at(1).at(0).filepath(), u"bandcamp://track/71"_s);
    QCOMPARE(playlists.blocks.at(0).at(1).album(), u"Admitted album"_s);
    QCOMPARE(playlists.blocks.at(2).at(0).album(), u"Admitted album"_s);
    QCOMPARE(playlists.blocks.at(2).at(0).extraProperties().value(BandcampSourceUrlProperty), AlbumSource);
    QCOMPARE(playlists.insertedPlaylists.at(0), playlists.insertedPlaylists.at(1));
    QCOMPARE(playlists.insertedPlaylists.at(1), playlists.insertedPlaylists.at(2));
}

void FooyinTrackTests::advancesAfterFailuresAndLeavesEmptyImportsUncreated()
{
    {
        LibraryBoundary library;
        PlaylistBoundary playlists;
        BandcampImportController controller{
            5, {{BandcampPageKind::Album, AlbumSource, {}}}, true, {}, library.api, playlists.api, {}};
        bool finished = false;
        BandcampImportSummary summary;
        connectImportCompletion(controller, finished, summary);
        controller.start();
        controller.acceptScopeResult(failedScope(5));
        controller.finishResolution({5, 1, 0, 1, 0});

        QTRY_VERIFY_WITH_TIMEOUT(finished, 1000);
        QCOMPARE(summary.added, 0);
        QCOMPARE(summary.failed, 1);
        QCOMPARE(playlists.createCalls, 0);
        QVERIFY(playlists.blocks.isEmpty());
    }

    {
        LibraryBoundary library;
        PlaylistBoundary playlists;
        const Fooyin::UId selected = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
        BandcampImportController controller{
            6,           {{BandcampPageKind::Album, AlbumSource, {}}, {BandcampPageKind::Track, TrackSource, {}}},
            false,       selected,
            library.api, playlists.api,
            {}};
        bool finished = false;
        BandcampImportSummary summary;
        connectImportCompletion(controller, finished, summary);
        controller.start();
        controller.acceptScopeResult(failedScope(6, 0));
        controller.acceptScopeResult(successfulTrackScope(6, false, 1));
        controller.finishResolution({6, 2, 1, 1, 0});

        QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
        QCOMPARE(summary.added, 1);
        QCOMPARE(summary.failed, 1);
        QCOMPARE(playlists.createCalls, 0);
        QCOMPARE(playlists.blocks.size(), 1);
        QCOMPARE(playlists.insertedPlaylists.first(), selected);
    }

    {
        LibraryBoundary library;
        Fooyin::Track resident = project(memberTrack(71, u"https://cdn.example.com/old.mp3?sig=old"_s));
        resident.setId(71);
        resident.setTitle(u"User-edited title"_s);
        resident.clearWasModified();
        library.residents.push_back(resident);
        library.makeWriteAvailable();
        PlaylistBoundary playlists;
        BandcampImportController controller{
            10,          {{BandcampPageKind::Album, AlbumSource, {}}, {BandcampPageKind::Track, TrackSource, {}}},
            true,        {},
            library.api, playlists.api,
            {}};
        bool finished = false;
        BandcampImportSummary summary;
        connectImportCompletion(controller, finished, summary);
        controller.start();
        controller.acceptScopeResult(successfulAlbumScope(10, 0));
        controller.acceptScopeResult(successfulTrackScope(10, false, 1));
        controller.finishResolution({10, 2, 2, 0, 0});

        QTRY_COMPARE_WITH_TIMEOUT(library.writeCalls, 1, 2000);
        library.finishWrite(0, Fooyin::WriteResult{Fooyin::WriteState::Completed, 0, 1});
        QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
        QCOMPARE(summary.added, 1);
        QCOMPARE(summary.failed, 1);
        QVERIFY(summary.stopReason.isEmpty());
        QCOMPARE(library.snapshotCalls, 2);
        QCOMPARE(playlists.createCalls, 1);
        QCOMPARE(playlists.names, QStringList{u"Bandcamp"_s});
        QCOMPARE(playlists.blocks.size(), 1);
    }
}

void FooyinTrackTests::stopsWhenDestinationChangesDuringPreparation()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    const Fooyin::UId selected = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    library.snapshotHook       = [&playlists, selected]() {
        playlists.availability.insert(selected.toString(QUuid::WithoutBraces), BandcampPlaylistAvailability::Locked);
    };
    BandcampOperationId cancelledOperation = 0;
    BandcampImportController controller{7,
                                        {{BandcampPageKind::Album, AlbumSource, {}}},
                                        false,
                                        selected,
                                        library.api,
                                        playlists.api,
                                        [&cancelledOperation](BandcampOperationId id) {
                                            cancelledOperation = id;
                                        }};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(7));
    controller.finishResolution({7, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, 0);
    QCOMPARE(summary.failed, 1);
    QCOMPARE(summary.stopReason, u"The selected playlist is no longer available or writable."_s);
    QCOMPARE(cancelledOperation, BandcampOperationId{7});
    QVERIFY(playlists.blocks.isEmpty());
    QCOMPARE(playlists.createCalls, 0);

    PlaylistBoundary unavailable;
    unavailable.owner.reset();
    BandcampImportController unavailableController{12,
                                                   {{BandcampPageKind::Track, TrackSource, {}}},
                                                   true,
                                                   {},
                                                   library.api,
                                                   unavailable.api,
                                                   [&cancelledOperation](BandcampOperationId id) {
                                                       cancelledOperation = id;
                                                   }};
    bool unavailableFinished = false;
    BandcampImportSummary unavailableSummary;
    connectImportCompletion(unavailableController, unavailableFinished, unavailableSummary);
    unavailableController.start();
    unavailableController.finishResolution({12, 0, 0, 0, 0});

    QVERIFY(unavailableFinished);
    QCOMPARE(unavailableSummary.failed, 0);
    QCOMPARE(unavailableSummary.stopReason, u"The playlist service is unavailable."_s);
    QCOMPARE(cancelledOperation, BandcampOperationId{12});
    QCOMPARE(unavailable.createCalls, 0);
}

void FooyinTrackTests::waitsForGatewayBeforeFinishingResolutionImport()
{
    LibraryBoundary library;
    library.residents.push_back(residentWithUserEdits(710));
    library.makeWriteAvailable();
    PlaylistBoundary playlists;
    const Fooyin::UId selected = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    BandcampImportController controller{
        8, {{BandcampPageKind::Track, TrackSource, {}}}, false, selected, library.api, playlists.api, {}};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    BandcampResolvedTrack member = memberTrack();
    BandcampScopeResult scope{8,
                              0,
                              {BandcampPageKind::Track, TrackSource, {}},
                              ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(member)}}};
    controller.start();
    controller.acceptScopeResult(scope);
    QTRY_COMPARE_WITH_TIMEOUT(library.writeCalls, 1, 2000);
    controller.finishResolution({8, 1, 1, 0, 0});
    QVERIFY(!finished);

    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, 1);
    QCOMPARE(playlists.blocks.size(), 1);
    QCOMPARE(playlists.insertedPlaylists.first(), selected);
}

void FooyinTrackTests::cancellationWithdrawsPendingGatewayHandoff()
{
    LibraryBoundary library;
    library.residents.push_back(residentWithUserEdits(711));
    library.makeWriteAvailable();
    PlaylistBoundary playlists;
    const Fooyin::UId selected             = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    BandcampOperationId cancelledOperation = 0;
    BandcampImportController controller{9,
                                        {{BandcampPageKind::Track, TrackSource, {}}},
                                        false,
                                        selected,
                                        library.api,
                                        playlists.api,
                                        [&cancelledOperation](BandcampOperationId id) {
                                            cancelledOperation = id;
                                        }};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);
    BandcampResolvedTrack member = memberTrack();
    controller.start();
    controller.acceptScopeResult({9,
                                  0,
                                  {BandcampPageKind::Track, TrackSource, {}},
                                  ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(member)}}});
    QTRY_COMPARE_WITH_TIMEOUT(library.writeCalls, 1, 2000);

    controller.cancel();
    QCOMPARE(library.cancelCalls, 1);
    QCOMPARE(cancelledOperation, BandcampOperationId{9});
    controller.finishResolution({9, 1, 1, 0, 0});
    QVERIFY(finished);
    QCOMPARE(summary.added, 0);
    QCOMPARE(summary.cancelled, 1);
    QVERIFY(playlists.blocks.isEmpty());

    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QCoreApplication::processEvents();
}

void FooyinTrackTests::unavailableProbeFallsBackAndLogsOncePerTrack()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    BandcampImportController controller{
        19, {{BandcampPageKind::Album, AlbumSource, {}}}, true, {}, library.api, playlists.api, {}, nullptr, true};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(19));
    controller.finishResolution({19, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, 1);
    QCOMPARE(summary.failed, 0);
    QCOMPARE(playlists.blocks.size(), 1);
    QCOMPARE(playlists.blocks.front().size(), std::size_t{2});

    const auto warnings = std::count_if(capturedLogs().cbegin(), capturedLogs().cend(), [](const CapturedLog& log) {
        return log.category == u"fy.bandcamp.tracks"_s && log.message.startsWith(u"track="_s)
            && log.message.contains(u"stream_probe_failed reason=remote_unavailable"_s);
    });
    QCOMPARE(warnings, 2);
    for(const qint64 trackId : {71, 72}) {
        const QString expected
            = u"track="_s + QString::number(trackId) + u" stream_probe_failed reason=remote_unavailable"_s;
        QCOMPARE(std::count_if(capturedLogs().cbegin(), capturedLogs().cend(),
                               [&expected](const CapturedLog& log) {
                                   return log.category == u"fy.bandcamp.tracks"_s && log.message.startsWith(expected);
                               }),
                 1);
    }
}

void FooyinTrackTests::failedProbesStillAdmitResolvedScope()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    auto audioLoader = std::make_shared<Fooyin::AudioLoader>();
    BandcampStreamProbe probe{audioLoader, nullptr};
    BandcampImportController controller{
        20, {{BandcampPageKind::Album, AlbumSource, {}}}, true, {}, library.api, playlists.api, {}, &probe, true};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(20));
    controller.finishResolution({20, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, qsizetype{1});
    QCOMPARE(summary.failed, qsizetype{0});
    QCOMPARE(summary.cancelled, qsizetype{0});
    QCOMPARE(playlists.blocks.size(), 1);
    QCOMPARE(playlists.blocks.front().size(), std::size_t{2});
    for(const Fooyin::Track& track : playlists.blocks.front()) {
        QVERIFY(track.codec().isEmpty());
        QCOMPARE(track.sampleRate(), 0);
        QVERIFY(track.bitDepth() <= 0);
    }
}

void FooyinTrackTests::capacityRejectedProbeRetriesWhenSlotFrees()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    auto audioLoader = std::make_shared<Fooyin::AudioLoader>();
    BandcampStreamProbe probe{audioLoader, nullptr};
    QObject externalContext;
    bool firstCompleted  = false;
    bool secondCompleted = false;
    const auto firstSubmission
        = probe.probe({standaloneTrack(901)}, &externalContext,
                      [&firstCompleted](QList<BandcampStreamProbeResult>) { firstCompleted = true; });
    const auto secondSubmission
        = probe.probe({standaloneTrack(902)}, &externalContext,
                      [&secondCompleted](QList<BandcampStreamProbeResult>) { secondCompleted = true; });
    QVERIFY(firstSubmission.token > 0);
    QVERIFY(secondSubmission.token > 0);

    const Fooyin::UId destination = playlists.addPlaylist(BandcampPlaylistAvailability::Writable);
    BandcampImportController controller{24, {}, false, destination, library.api, playlists.api, {}, &probe, true};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);
    LogCapture logs{u"fy.bandcamp.tracks.debug=true\nfy.trace.bandcamp.tracks.debug=true"_s};

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(24));
    controller.finishResolution({24, 1, 1, 0, 0});

    QTRY_VERIFY_WITH_TIMEOUT(firstCompleted && secondCompleted, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QCOMPARE(summary.added, qsizetype{1});
    QCOMPARE(summary.failed, qsizetype{0});
    QCOMPARE(playlists.blocks.size(), 1);

    const auto captured = capturedLogsSnapshot();
    for(const qint64 trackId : {71, 72}) {
        const QString capacityMessage
            = u"track="_s + QString::number(trackId) + u" stream_probe_failed reason=capacity_rejected"_s;
        const QString startedMessage = u"track="_s + QString::number(trackId) + u" stream_probe_started"_s;
        const auto capacityLog = std::find_if(captured.cbegin(), captured.cend(), [&capacityMessage](const auto& log) {
            return log.category == u"fy.bandcamp.tracks"_s && log.message.startsWith(capacityMessage);
        });
        const auto startedLog  = std::find_if(captured.cbegin(), captured.cend(), [&startedMessage](const auto& log) {
            return log.category == u"fy.trace.bandcamp.tracks"_s && log.message.startsWith(startedMessage);
        });
        QVERIFY(capacityLog != captured.cend());
        QVERIFY(startedLog != captured.cend());
        QVERIFY(capacityLog < startedLog);
    }
}

void FooyinTrackTests::cancellationDuringProbeSkipsResolvedScope_data()
{
    QTest::addColumn<bool>("album");
    QTest::addColumn<bool>("resolutionFinished");
    QTest::newRow("album-resolution-pending") << true << false;
    QTest::newRow("album-resolution-finished") << true << true;
    QTest::newRow("track-resolution-pending") << false << false;
    QTest::newRow("track-resolution-finished") << false << true;
}

void FooyinTrackTests::cancellationDuringProbeSkipsResolvedScope()
{
    QFETCH(bool, album);
    QFETCH(bool, resolutionFinished);
    LibraryBoundary library;
    PlaylistBoundary playlists;
    auto audioLoader = std::make_shared<Fooyin::AudioLoader>();
    BandcampStreamProbe probe{audioLoader, nullptr};
    BandcampOperationId cancelledOperation = 0;
    BandcampImportController controller{
        18,
        {{album ? BandcampPageKind::Album : BandcampPageKind::Track, album ? AlbumSource : TrackSource, {}},
         {BandcampPageKind::Track, TrackSource, {}}},
        true,
        {},
        library.api,
        playlists.api,
        [&cancelledOperation](BandcampOperationId id) { cancelledOperation = id; },
        &probe,
        true};
    bool finished   = false;
    int completions = 0;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);
    QObject::connect(&controller, &BandcampImportController::finished, &controller,
                     [&completions](const BandcampImportSummary&) { ++completions; });

    controller.start();
    controller.acceptScopeResult(album ? successfulAlbumScope(18) : successfulTrackScope(18));
    controller.acceptScopeResult(successfulTrackScope(18, false, 1));
    if(resolutionFinished) {
        controller.finishResolution({18, 2, 2, 0, 0});
    }
    controller.cancel();
    controller.cancel();
    if(!resolutionFinished) {
        QVERIFY(!finished);
        controller.finishResolution({18, 2, 2, 0, 0});
    }

    QVERIFY(finished);
    QCOMPARE(cancelledOperation, BandcampOperationId{18});
    QCOMPARE(summary.added, qsizetype{0});
    QCOMPARE(summary.failed, qsizetype{0});
    QCOMPARE(summary.cancelled, qsizetype{2});
    QCOMPARE(completions, 1);
    QCOMPARE(playlists.createCalls, 0);
    QVERIFY(playlists.blocks.isEmpty());
    QCOMPARE(library.snapshotCalls, 0);
    QCOMPARE(library.writeCalls, 0);
    QTRY_VERIFY_WITH_TIMEOUT(probe.children().isEmpty(), 2000);
    QCOMPARE(completions, 1);
    QCOMPARE(library.snapshotCalls, 0);
    QVERIFY(playlists.blocks.isEmpty());
}

void FooyinTrackTests::cancellationDuringProbePreservesAlreadyInsertedScopes()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    auto audioLoader = std::make_shared<Fooyin::AudioLoader>();
    BandcampStreamProbe probe{audioLoader, nullptr};
    BandcampImportController controller{21,
                                        {{BandcampPageKind::Album, AlbumSource, {}},
                                         {BandcampPageKind::Track, TrackSource, {}},
                                         {BandcampPageKind::Album, AlbumSource, {}}},
                                        true,
                                        {},
                                        library.api,
                                        playlists.api,
                                        {},
                                        &probe,
                                        true};
    bool finished = false;
    BandcampImportSummary summary;
    connectImportCompletion(controller, finished, summary);

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(21));
    QTRY_COMPARE_WITH_TIMEOUT(playlists.blocks.size(), 1, 2000);
    controller.acceptScopeResult(successfulTrackScope(21, false, 1));
    controller.acceptScopeResult(successfulAlbumScope(21, 2));
    controller.cancel();
    controller.finishResolution({21, 3, 3, 0, 0});

    QVERIFY(finished);
    QCOMPARE(summary.added, qsizetype{1});
    QCOMPARE(summary.failed, qsizetype{0});
    QCOMPARE(summary.cancelled, qsizetype{2});
    QTRY_VERIFY_WITH_TIMEOUT(probe.children().isEmpty(), 2000);
    QCOMPARE(playlists.blocks.size(), 1);
    QCOMPARE(playlists.blocks.front().size(), std::size_t{2});
    QCOMPARE(playlists.blocks.front().front().filepath(), u"bandcamp://track/71"_s);
    QCOMPARE(library.snapshotCalls, 1);
}

void FooyinTrackTests::deletingControllerFromPlaylistCallbackWithdrawsResolution()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    BandcampImportController* controller   = nullptr;
    BandcampOperationId cancelledOperation = 0;
    playlists.insertHook                   = [&controller]() {
        BandcampImportController* doomed = controller;
        controller                       = nullptr;
        delete doomed;
    };
    controller = new BandcampImportController{
        14,
        {{BandcampPageKind::Album, AlbumSource, {}}, {BandcampPageKind::Track, TrackSource, {}}},
        true,
        {},
        library.api,
        playlists.api,
        [&cancelledOperation](BandcampOperationId id) {
            cancelledOperation = id;
        }};
    QPointer<BandcampImportController> guard{controller};
    bool finished = false;
    BandcampImportSummary summary;
    QObject::connect(controller, &BandcampImportController::finished, controller,
                     [&finished, &summary](const BandcampImportSummary& value) {
                         finished = true;
                         summary  = value;
                     });

    controller->start();
    controller->acceptScopeResult(successfulAlbumScope(14, 0));
    controller->acceptScopeResult(successfulTrackScope(14, false, 1));

    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 2000);
    QCOMPARE(cancelledOperation, BandcampOperationId{14});
    QCOMPARE(playlists.blocks.size(), 1);
    QVERIFY(!finished);
}

void FooyinTrackTests::cancellationDuringPlaylistCreationDoesNotInsert()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    BandcampImportController* controllerPointer = nullptr;
    bool finished                               = false;
    bool finishedDuringCreation                 = false;
    BandcampImportSummary summary;
    BandcampOperationId cancelledOperation = 0;
    BandcampImportController controller{15,
                                        {{BandcampPageKind::Album, AlbumSource, {}}},
                                        true,
                                        {},
                                        library.api,
                                        playlists.api,
                                        [&controllerPointer, &cancelledOperation](BandcampOperationId id) {
                                            cancelledOperation = id;
                                            controllerPointer->finishResolution({id, 1, 1, 0, 0});
                                        }};
    controllerPointer = &controller;
    connectImportCompletion(controller, finished, summary);
    playlists.createHook = [&controller, &finished, &finishedDuringCreation]() {
        controller.cancel();
        finishedDuringCreation = finished;
    };

    controller.start();
    controller.acceptScopeResult(successfulAlbumScope(15));

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QVERIFY(!finishedDuringCreation);
    QCOMPARE(cancelledOperation, BandcampOperationId{15});
    QCOMPARE(summary.added, 0);
    QCOMPARE(summary.cancelled, 1);
    QCOMPARE(playlists.createCalls, 1);
    QVERIFY(playlists.blocks.isEmpty());
}

void FooyinTrackTests::cancellationDuringInsertionDefersSummaryAndCountsAcceptedBlock()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    BandcampImportController* controllerPointer = nullptr;
    bool finished                               = false;
    bool finishedDuringInsertion                = false;
    BandcampImportSummary summary;
    BandcampOperationId cancelledOperation = 0;
    BandcampImportController controller{16,
                                        {{BandcampPageKind::Track, TrackSource, {}}},
                                        true,
                                        {},
                                        library.api,
                                        playlists.api,
                                        [&controllerPointer, &cancelledOperation](BandcampOperationId id) {
                                            cancelledOperation = id;
                                            controllerPointer->finishResolution({id, 1, 1, 0, 0});
                                        }};
    controllerPointer = &controller;
    connectImportCompletion(controller, finished, summary);
    playlists.insertHook = [&controller, &finished, &finishedDuringInsertion]() {
        controller.cancel();
        finishedDuringInsertion = finished;
    };

    controller.start();
    controller.acceptScopeResult(successfulTrackScope(16));

    QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
    QVERIFY(!finishedDuringInsertion);
    QCOMPARE(cancelledOperation, BandcampOperationId{16});
    QCOMPARE(summary.added, 1);
    QCOMPARE(summary.failed, 0);
    QCOMPARE(summary.cancelled, 0);
    QCOMPARE(playlists.blocks.size(), 1);
}

void FooyinTrackTests::deletingControllerFromIntegrationErrorWithdrawsResolution()
{
    LibraryBoundary library;
    PlaylistBoundary playlists;
    playlists.owner.reset();

    BandcampImportController* controller = nullptr;
    QPointer<BandcampImportController> guard;
    QObject connectionContext;
    BandcampOperationId cancelledOperation = 0;
    bool finished                          = false;
    int integrationErrors                  = 0;
    controller                             = new BandcampImportController{17,
                                                                          {{BandcampPageKind::Album, AlbumSource, {}}},
                                              true,
                                                                          {},
                                              library.api,
                                              playlists.api,
                                              [&cancelledOperation](BandcampOperationId id) {
                                                  cancelledOperation = id;
                                              }};
    guard                                  = controller;
    QObject::connect(controller, &BandcampImportController::integrationError, &connectionContext,
                     [&controller, &integrationErrors](const QString&) {
                         ++integrationErrors;
                         delete controller;
                         controller = nullptr;
                     });
    QObject::connect(controller, &BandcampImportController::finished, &connectionContext,
                     [&finished](const BandcampImportSummary&) { finished = true; });

    controller->start();
    QCoreApplication::processEvents();

    QVERIFY(guard.isNull());
    QCOMPARE(integrationErrors, 1);
    QCOMPARE(cancelledOperation, BandcampOperationId{17});
    QVERIFY(!finished);
    QCOMPARE(playlists.createCalls, 0);
    QVERIFY(playlists.blocks.isEmpty());
}

void FooyinTrackTests::resolvesUniqueBandcampReleaseUrls()
{
    Fooyin::Track local;
    local.setFilePath(u"/music/local.flac"_s);

    Fooyin::Track nonCanonical = project(standaloneTrack(44));
    nonCanonical.setExtraProperty(BandcampSourceUrlProperty, u"https://artist.bandcamp.com/album/parent?utm=x"_s);

    Fooyin::Track foreignPath = project(standaloneTrack(46));
    foreignPath.setFilePath(u"/music/copied.flac"_s);

    QVERIFY(!bandcampReleaseUrl(local).has_value());
    QVERIFY(!bandcampReleaseUrl(nonCanonical).has_value());
    QVERIFY(!bandcampReleaseUrl(foreignPath).has_value());

    const Fooyin::TrackList tracks{project(memberTrack(42)),     local,
                                   project(memberTrack(43)),     nonCanonical,
                                   project(standaloneTrack(45)), project(memberTrack(46))};
    QCOMPARE(bandcampReleaseUrls(tracks), (QStringList{AlbumSource, TrackSource}));
}

int runFooyinTrackTests(int argc, char** argv)
{
    FooyinTrackTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampfooyintracktests.moc"
