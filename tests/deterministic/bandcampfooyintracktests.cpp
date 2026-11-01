#include "bandcampfooyintrack.h"
#include "bandcamplogging.h"
#include "bandcamptrackuri.h"

#include <QPromise>
#include <QThread>
#include <QtTest>

#include <limits>
#include <memory>
#include <stdexcept>
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

class LibraryBoundary
{
public:
    LibraryBoundary()
    {
        api.owner = owner.get();
        api.tracks = [this]() {
            ++snapshotCalls;
            if(snapshotDelayMs > 0) {
                QThread::msleep(static_cast<unsigned long>(snapshotDelayMs));
            }
            if(throwOnSnapshot) {
                throw std::runtime_error{"snapshot failed"};
            }
            if(snapshotFailuresRemaining > 0) {
                --snapshotFailuresRemaining;
                throw std::runtime_error{"transient snapshot failure"};
            }
            return residents;
        };
        api.metadataStore = [this]() -> std::shared_ptr<Fooyin::TrackMetadataStore> {
            if(throwOnMetadataStore) {
                throw std::runtime_error{"store failed"};
            }
            return store;
        };
        api.writeTrackMetadata = [this](const Fooyin::TrackList& tracks) {
            ++writeCalls;
            if(throwOnWrite) {
                throw std::runtime_error{"write failed"};
            }
            writeInputs.push_back(tracks);
            if(invalidWriteRequest) {
                return Fooyin::WriteRequest{};
            }
            if(promiseIndex >= promises.size()) {
                throw std::runtime_error{"write promise unavailable"};
            }
            auto promise = promises.at(promiseIndex++);
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
    bool throwOnSnapshot{false};
    bool throwOnMetadataStore{false};
    bool throwOnWrite{false};
    bool invalidWriteRequest{false};
    int snapshotFailuresRemaining{0};
    int snapshotDelayMs{0};
};

void submit(BandcampLibraryGateway& gateway, QList<BandcampResolvedTrack> incoming, QObject* context,
            BandcampLibraryGateway::Completion completion, int timeoutMs = 1000, CorrelationContext correlation = {})
{
    gateway.commit(std::move(incoming), timeoutMs, context, std::move(correlation), std::move(completion));
}

struct CapturedMessage
{
    QString category;
    QString text;
};

QList<CapturedMessage>* activeMessages{nullptr};

void captureMessage(QtMsgType, const QMessageLogContext& context, const QString& message)
{
    if(activeMessages) {
        activeMessages->append({QString::fromUtf8(context.category), message});
    }
}

class MessageCapture
{
public:
    MessageCapture()
        : m_previousHandler{qInstallMessageHandler(captureMessage)}
    {
        activeMessages = &messages;
        m_traceWasEnabled = BANDCAMP_TRACE_TRACKS().isDebugEnabled();
        QLoggingCategory::setFilterRules(QStringLiteral("fy.trace.bandcamp.tracks.debug=true"));
    }

    ~MessageCapture()
    {
        if(!m_traceWasEnabled) {
            QLoggingCategory::setFilterRules(QStringLiteral("fy.trace.bandcamp.tracks.debug=false"));
        }
        activeMessages = nullptr;
        qInstallMessageHandler(m_previousHandler);
    }

    QList<CapturedMessage> messages;

private:
    QtMessageHandler m_previousHandler{nullptr};
    bool m_traceWasEnabled{false};
};
} // namespace

class FooyinTrackTests : public QObject
{
    Q_OBJECT

private slots:
    void canonicalTrackUriRoundTrips();
    void rejectsMalformedTrackUris();
    void projectsTrustedAlbumMetadataWithoutInference();
    void standaloneProjectionLeavesParentFieldsAbsent();
    void parsesOnlyObservedBandcampExpiryForm();
    void preservesLargeRepeatedInputOccurrences();
    void mergesOnlyTransportAndPreservesResidentMetadata();
    void rejectsConflictingPersistedTrackIdMirror();
    void reportsWriteFailuresWithoutMutatingResidents();
    void serializesOnlyWithinOneSubmission();
    void timeoutDoesNotPoisonQueuedCommit();
    void activeDeadlineIncludesSnapshotWork();
    void guardsCallbacksAndObjectLifetimes();
    void containsDependencyAndFutureExceptions();
    void correlatesAndRedactsEffectiveTransportChanges();
};

void FooyinTrackTests::canonicalTrackUriRoundTrips()
{
    QCOMPARE(bandcampTrackUri(1), u"bandcamp://track/1"_s);
    QCOMPARE(parseBandcampTrackUri(u"bandcamp://track/1"_s), std::optional<qint64>{1});

    constexpr qint64 MaxId = std::numeric_limits<qint64>::max();
    const QString maxUri = u"bandcamp://track/9223372036854775807"_s;
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

    auto otherScope = memberTrack();
    otherScope.track.title = u"Other selection title"_s;
    QCOMPARE(project(otherScope).filepath(), track.filepath());

    auto missingTrackDate = memberTrack();
    missingTrackDate.track.releaseDate = {};
    QVERIFY(project(missingTrackDate).date().isEmpty());
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
    QVERIFY(!bandcampStreamExpiryMs(
        u"https://t4.bcbits.com/stream/abc?ts=not-a-number"_s));
    QVERIFY(!bandcampStreamExpiryMs(
        u"https://t4.bcbits.com/stream/abc?ts=1793621993&ts=1793622993"_s));
    QVERIFY(!bandcampStreamExpiryMs(
        u"https://t4.bcbits.com/stream/abc?ts=9223372036854776"_s));
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

void FooyinTrackTests::mergesOnlyTransportAndPreservesResidentMetadata()
{
    LibraryBoundary library;
    const Fooyin::Track resident = residentWithUserEdits(700);
    library.residents.push_back(resident);
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    QList<BandcampResolvedTrack> incoming{memberTrack(), memberTrack(42, OtherSignedStream)};
    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    CorrelationContext correlation;
    correlation.jobId     = 9;
    correlation.requestId = 14;
    submit(gateway, std::move(incoming), &receiver, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    }, 1000, correlation);

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
    QCOMPARE(library.residents.front().extraProperties().value(BandcampStreamUrlProperty),
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

void FooyinTrackTests::rejectsConflictingPersistedTrackIdMirror()
{
    LibraryBoundary library;
    Fooyin::Track resident = residentWithUserEdits(710);
    resident.setExtraProperty(BandcampTrackIdProperty, u"43"_s);
    library.residents.push_back(resident);
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;
    MessageCapture capture;

    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    CorrelationContext correlation;
    correlation.jobId     = 31;
    correlation.requestId = 7;
    submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    }, 1000, correlation);

    QTRY_VERIFY(completed);
    const auto* error = std::get_if<FooyinTrackCommitFailure>(&outcome);
    QVERIFY(error);
    QCOMPARE(error->code, FooyinTrackCommitErrorCode::InvalidIdentity);
    QCOMPARE(error->trackId, qint64{42});
    QCOMPARE(library.writeCalls, 0);

    bool foundFailure = false;
    for(const CapturedMessage& message : capture.messages) {
        if(message.category == u"fy.bandcamp.tracks"_s
           && message.text.contains(u"track.commit.failed"_s)) {
            foundFailure = true;
            QVERIFY(message.text.startsWith(u"bandcamp://track/42 "_s));
            QVERIFY(message.text.contains(u"job=\"31\""_s));
            QVERIFY(message.text.contains(u"request=\"7\""_s));
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
        {Fooyin::WriteState::Completed, 2, 0},
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
        submit(retryGateway, {memberTrack()}, &retryReceiver,
               [&completed, &outcome](FooyinTrackCommitOutcome result) {
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

void FooyinTrackTests::serializesOnlyWithinOneSubmission()
{
    LibraryBoundary library;
    Fooyin::Track original = residentWithUserEdits(703);
    library.residents.push_back(original);
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
    submit(gateway, {memberTrack(42, OtherSignedStream)}, &receiver,
           [&](FooyinTrackCommitOutcome result) {
               second     = std::move(result);
               secondDone = true;
           });

    QTRY_COMPARE(library.writeCalls, 1);
    QCOMPARE(library.snapshotCalls, 1);
    Fooyin::Track latest = library.writeInputs.front().front();
    latest.setTitle(u"Edit visible when queued work starts"_s);
    latest.replaceExtraTag(u"MOOD"_s, u"latest mood"_s);
    latest.setHash(u"latest hash"_s);
    library.residents = {latest};
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(firstDone);
    QTRY_COMPARE(library.writeCalls, 2);
    QCOMPARE(library.snapshotCalls, 2);
    QCOMPARE(library.writeInputs.at(1).front().title(), u"Edit visible when queued work starts"_s);
    QCOMPARE(library.writeInputs.at(1).front().extraTag(u"MOOD"_s), QStringList{u"latest mood"_s});
    QCOMPARE(library.writeInputs.at(1).front().hash(), u"latest hash"_s);
    QCOMPARE(library.writeInputs.at(1).front().extraProperties().value(BandcampStreamUrlProperty), OtherSignedStream);
    library.finishWrite(1, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(secondDone);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(first));
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(second));
}

void FooyinTrackTests::timeoutDoesNotPoisonQueuedCommit()
{
    LibraryBoundary library;
    library.residents.push_back(residentWithUserEdits(704));
    library.makeWriteAvailable();
    library.makeWriteAvailable();
    BandcampLibraryGateway gateway{library.api};
    QObject receiver;

    bool timedOut = false;
    bool completed = false;
    FooyinTrackCommitOutcome timeoutOutcome;
    FooyinTrackCommitOutcome result;
    submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome value) {
        timeoutOutcome = std::move(value);
        timedOut = true;
    }, 5);
    submit(gateway, {memberTrack(42, OtherSignedStream)}, &receiver,
           [&](FooyinTrackCommitOutcome value) {
               result = std::move(value);
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
    submit(gateway, {standaloneTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
        outcome   = std::move(result);
        completed = true;
    }, 1);

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
        BandcampLibraryGateway gateway{library.api};
        auto* receiver = new QObject;
        bool called = false;
        submit(gateway, {standaloneTrack()}, receiver,
               [&called](FooyinTrackCommitOutcome) { called = true; });
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
        submit(*gateway, {memberTrack()}, &receiver,
               [&called](FooyinTrackCommitOutcome) { called = true; });
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
        submit(gateway, {memberTrack()}, &receiver,
               [&called, &outcome](FooyinTrackCommitOutcome result) {
                   called = true;
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

void FooyinTrackTests::containsDependencyAndFutureExceptions()
{
    {
        LibraryBoundary library;
        library.owner.reset();
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {standaloneTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
            outcome   = std::move(result);
            completed = true;
        });
        QTRY_VERIFY(completed);
        const auto* error = std::get_if<FooyinTrackCommitFailure>(&outcome);
        QVERIFY(error);
        QCOMPARE(error->code, FooyinTrackCommitErrorCode::LibraryUnavailable);
        QCOMPARE(library.snapshotCalls, 0);
        QCOMPARE(library.writeCalls, 0);
    }

    {
        LibraryBoundary library;
        library.throwOnSnapshot = true;
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {standaloneTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
            outcome = std::move(result);
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(outcome));
    }

    {
        LibraryBoundary library;
        library.throwOnMetadataStore = true;
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {standaloneTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
            outcome = std::move(result);
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(outcome));
        QCOMPARE(library.snapshotCalls, 1);
    }

    {
        LibraryBoundary library;
        library.residents.push_back(residentWithUserEdits(707));
        library.throwOnWrite = true;
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
            outcome = std::move(result);
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(outcome));
    }

    {
        LibraryBoundary library;
        library.residents.push_back(residentWithUserEdits(708));
        auto promise = std::make_shared<QPromise<Fooyin::WriteResult>>();
        promise->start();
        library.promises.push_back(promise);
        BandcampLibraryGateway gateway{library.api};
        QObject receiver;
        bool completed = false;
        FooyinTrackCommitOutcome outcome;
        submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
            outcome = std::move(result);
            completed = true;
        });
        QTRY_COMPARE(library.writeCalls, 1);
        promise->setException(std::make_exception_ptr(std::runtime_error{"future failed"}));
        promise->finish();
        QTRY_VERIFY(completed);
        QVERIFY(std::holds_alternative<FooyinTrackCommitFailure>(outcome));
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
    MessageCapture capture;

    bool completed = false;
    FooyinTrackCommitOutcome outcome;
    CorrelationContext correlation;
    correlation.jobId = 9;
    correlation.requestId = 14;
    submit(gateway, {memberTrack()}, &receiver, [&](FooyinTrackCommitOutcome result) {
        outcome = std::move(result);
        completed = true;
    }, 1000, correlation);
    QTRY_COMPARE(library.writeCalls, 1);
    library.finishWrite(0, {Fooyin::WriteState::Completed, 1, 0});
    QTRY_VERIFY(completed);
    QVERIFY(std::holds_alternative<FooyinTrackCommitSuccess>(outcome));

    bool found = false;
    for(const CapturedMessage& message : capture.messages) {
        if(message.category == u"fy.trace.bandcamp.tracks"_s
           && message.text.contains(u"transport.updated"_s)) {
            found = true;
            QVERIFY(message.text.startsWith(u"bandcamp://track/42 "_s));
            QVERIFY(message.text.contains(u"job=\"9\""_s));
            QVERIFY(message.text.contains(u"request=\"14\""_s));
            QVERIFY(!message.text.contains(u"secret-old"_s));
            QVERIFY(!message.text.contains(u"token=one"_s));
        }
    }
    QVERIFY(found);
}

int runFooyinTrackTests(int argc, char** argv)
{
    FooyinTrackTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampfooyintracktests.moc"
