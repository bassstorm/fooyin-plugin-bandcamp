#include "bandcampfakenetwork.h"
#include "bandcamplogcapture.h"
#include "bandcampplayback.h"
#include "bandcampplaybackpreparation.h"
#include "bandcamptestpages.h"

#include <core/engine/audioloader.h>
#include <core/network/remoteioservice.h>
#include <core/network/remotestreamdevice.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QPromise>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QString AlbumUrl{u"https://artist.bandcamp.com/album/parent"_s};

QString playbackStream(qint64 expiryMs)
{
    return u"https://t4.bcbits.com/stream/playback?signature=private&ts="_s + QString::number(expiryMs / 1000);
}

Fooyin::Track playbackTrack(qint64 expiryMs)
{
    BandcampTrack source;
    source.id        = 456;
    source.title     = u"Source title"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(expiryMs);
    auto track       = makeFooyinTrack({source, std::nullopt});
    track.setTitle(u"User-edited title"_s);
    track.replaceExtraTag(u"MOOD"_s, u"User mood"_s);
    track.setExtraProperty(u"_USER_PROPERTY"_s, u"preserve me"_s);
    return track;
}

QByteArray playbackEntry(qint64 id, qint64 expiryMs)
{
    return "{\"id\":" + QByteArray::number(id) + ",\"title\":\"Selected\",\"file\":{\"mp3-128\":\""
         + playbackStream(expiryMs).toUtf8() + "\"}}";
}

struct PlaybackLibrary
{
    PlaybackLibrary()
    {
        api.owner         = &owner;
        api.playlistOwner = &owner;
        api.tracks        = [this]() {
            ++libraryReads;
            return library;
        };
        api.playlistTracks = [this]() {
            return playlist;
        };
        api.metadataStore = []() {
            return std::shared_ptr<Fooyin::TrackMetadataStore>{};
        };
        api.writeTrackMetadata = [this](const Fooyin::TrackList& targets) {
            writes.push_back(targets);
            pending = std::make_shared<QPromise<Fooyin::WriteResult>>();
            pending->start();
            return Fooyin::WriteRequest{[this]() { ++cancellations; }, pending->future()};
        };
    }

    void finishWrite(bool success)
    {
        const int count = static_cast<int>(writes.back().size());
        pending->addResult(
            Fooyin::WriteResult{Fooyin::WriteState::Completed, success ? count : 0, success ? 0 : count});
        pending->finish();
    }

    void finishCancelledWrite()
    {
        pending->addResult(Fooyin::WriteResult{Fooyin::WriteState::Cancelled, 0, 0});
        pending->finish();
    }

    QObject owner;
    FooyinLibraryApi api;
    Fooyin::TrackList library;
    Fooyin::TrackList playlist;
    std::vector<Fooyin::TrackList> writes;
    std::shared_ptr<QPromise<Fooyin::WriteResult>> pending;
    int libraryReads{0};
    int cancellations{0};
};

struct RemoteState
{
    std::mutex mutex;
    QList<QUrl> urls;
    std::atomic_int created{0};
    std::atomic_int openCalls{0};
    std::atomic_int readinessWaitCalls{0};
    std::atomic_int readsBeforeReadiness{0};
    std::atomic_int reconnectDisabled{0};
    std::atomic_bool nonBlockingEnabled{false};
    int succeedsOn{1};
};

class FixtureRemoteDevice final : public QIODevice,
                                  public Fooyin::RemoteStreamDevice
{
public:
    FixtureRemoteDevice(std::shared_ptr<RemoteState> state, int attempt)
        : m_state{std::move(state)}
        , m_attempt{attempt}
    { }

    [[nodiscard]] bool isSequential() const override
    {
        return true;
    }

    [[nodiscard]] bool readWouldBlock() const override
    {
        return false;
    }

    [[nodiscard]] qsizetype bufferedByteCount() const override
    {
        return 0;
    }

    void setNonBlockingReadsEnabled(bool enabled) override
    {
        m_state->nonBlockingEnabled.store(enabled, std::memory_order_release);
    }

    void setReconnectOnFinishedEnabled(bool enabled) override
    {
        if(!enabled) {
            ++m_state->reconnectDisabled;
        }
    }

    void setReadCancellationToken(std::stop_token) override { }

    bool open(OpenMode mode) override
    {
        ++m_state->openCalls;
        return QIODevice::open(mode);
    }

    bool waitForReadyRead(int) override
    {
        ++m_state->readinessWaitCalls;
        m_readinessWaited = true;
        return m_attempt >= m_state->succeedsOn;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if(!m_readinessWaited) {
            ++m_state->readsBeforeReadiness;
        }
        if(maxSize <= 0) {
            return 0;
        }
        data[0] = 'x';
        return 1;
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1;
    }

private:
    std::shared_ptr<RemoteState> m_state;
    int m_attempt{0};
    bool m_readinessWaited{false};
};

class FixtureRemoteIo final : public Fooyin::RemoteIoService
{
public:
    FixtureRemoteIo(std::shared_ptr<QNetworkAccessManager> network, Fooyin::SettingsManager* settings,
                    std::shared_ptr<RemoteState> state)
        : Fooyin::RemoteIoService{std::move(network), settings}
        , m_state{std::move(state)}
    { }

    Fooyin::RemoteStreamSource createStreamSource(const QUrl& url) const override
    {
        const int attempt = ++m_state->created;
        {
            const std::scoped_lock lock{m_state->mutex};
            m_state->urls.append(url);
        }
        auto device = std::make_unique<FixtureRemoteDevice>(m_state, attempt);
        Fooyin::RemoteStreamSource source;
        source.remoteDevice = device.get();
        source.openTimeout  = std::chrono::seconds{5};
        source.device       = std::move(device);
        return source;
    }

private:
    std::shared_ptr<RemoteState> m_state;
};

struct InnerState
{
    QString sourcePath;
    QString trackPath;
    Fooyin::AudioDecoder::DecoderOptions options;
    bool hadTransportProperty{false};
    bool sourceOpen{false};
    bool firstBytePreserved{false};
    bool hasRemoteDevice{false};
    bool receivedPlaybackHint{false};
    bool trackRemote{false};
    bool seekable{true};
    bool accept{true};
    bool throwOnStart{false};
    bool blockInitialization{false};
    std::atomic_bool initEntered{false};
    std::atomic_bool initAbortObserved{false};
    std::mutex initMutex;
    std::condition_variable initChanged;
    int initCalls{0};
    int startCalls{0};
    int seekCalls{0};
    int readCalls{0};
    int stopCalls{0};
};

class FixtureRemoteDecoder final : public Fooyin::AudioDecoder
{
public:
    explicit FixtureRemoteDecoder(std::shared_ptr<InnerState> state)
        : m_state{std::move(state)}
    { }

    QStringList extensions() const override
    {
        return {u"mp3"_s};
    }

    bool supportsRemoteSources() const override
    {
        return true;
    }

    bool isSeekable() const override
    {
        return m_state->seekable;
    }

    std::optional<Fooyin::AudioFormat> init(const Fooyin::AudioSource& source, const Fooyin::Track& track,
                                            DecoderOptions options) override
    {
        ++m_state->initCalls;
        m_state->sourcePath           = source.filepath;
        m_state->trackPath            = track.filepath();
        m_state->options              = options;
        m_state->receivedPlaybackHint = playbackHints().testFlag(RepeatTrackEnabled);
        m_state->hadTransportProperty = track.hasExtraProperty(BandcampStreamUrlProperty);
        m_state->sourceOpen           = source.device && source.device->isOpen();
        m_state->firstBytePreserved   = source.device && source.device->bytesAvailable() > 0;
        m_state->hasRemoteDevice      = source.remoteStreamDevice != nullptr;
        m_state->trackRemote          = track.isRemote();
        if(m_state->blockInitialization) {
            std::unique_lock lock{m_state->initMutex};
            m_state->initEntered.store(true, std::memory_order_release);
            m_state->initChanged.notify_all();
            const bool aborted = m_state->initChanged.wait_for(lock, std::chrono::milliseconds{1500},
                                                               [this]() { return abortToken().stop_requested(); });
            m_state->initAbortObserved.store(aborted, std::memory_order_release);
        }
        if(abortToken().stop_requested()) {
            return {};
        }
        if(!m_state->accept || !m_state->sourceOpen || !m_state->hasRemoteDevice) {
            return {};
        }
        return Fooyin::AudioFormat{Fooyin::SampleFormat::S16, 44100, 2};
    }

    void stop() override
    {
        ++m_state->stopCalls;
    }

    void start() override
    {
        ++m_state->startCalls;
        if(m_state->throwOnStart) {
            throw std::runtime_error{"private decoder error"};
        }
    }

    void seek(uint64_t) override
    {
        ++m_state->seekCalls;
    }

    Fooyin::AudioDecoder::ReadResult readAudio(size_t) override
    {
        ++m_state->readCalls;
        return ReadResult::endOfStream();
    }

    Fooyin::AudioBuffer readBuffer(size_t) override
    {
        return {};
    }

protected:
    void interruptRead() override
    {
        m_state->initChanged.notify_all();
    }

private:
    std::shared_ptr<InnerState> m_state;
};

struct PlaybackFixture
{
    PlaybackFixture(int remoteOpenSucceedsOn, FooyinLibraryApi libraryApi = {})
    {
        settingsPath = QCoreApplication::applicationDirPath()
                     + QStringLiteral("/bandcamp-playback-settings-%1.ini").arg(QCoreApplication::applicationPid());
        QFile::remove(settingsPath);
        settings                = std::make_unique<Fooyin::SettingsManager>(settingsPath);
        network                 = std::make_shared<FakeNetworkManager>();
        service                 = std::make_unique<BandcampResolutionService>(network.get());
        audioLoader             = std::make_shared<Fooyin::AudioLoader>();
        remoteState             = std::make_shared<RemoteState>();
        remoteState->succeedsOn = remoteOpenSucceedsOn;
        remoteIo                = std::make_unique<FixtureRemoteIo>(network, settings.get(), remoteState);
        bridge = std::make_shared<BandcampPlaybackBridge>(service.get(), audioLoader, remoteIo.get(), settings.get(),
                                                          std::move(libraryApi));
    }

    ~PlaybackFixture()
    {
        if(bridge) {
            bridge->shutdown();
        }
        bridge.reset();
        remoteIo.reset();
        service.reset();
        audioLoader.reset();
        network.reset();
        settings.reset();
        QFile::remove(settingsPath);
    }

    QString settingsPath;
    std::unique_ptr<Fooyin::SettingsManager> settings;
    std::shared_ptr<FakeNetworkManager> network;
    std::unique_ptr<BandcampResolutionService> service;
    std::shared_ptr<Fooyin::AudioLoader> audioLoader;
    std::shared_ptr<RemoteState> remoteState;
    std::unique_ptr<FixtureRemoteIo> remoteIo;
    std::shared_ptr<BandcampPlaybackBridge> bridge;
};

class PlaybackTests : public QObject
{
    Q_OBJECT

private slots:
    void cachedPreparationDoesNotCreateGateway();
    void refreshedPreparationDoesNotWaitForPersistence();
    void cancellationDuringPlaybackHandoffSkipsPersistence();
    void invalidAndUnusablePlaybackTransportsFail();
    void serviceDestroyedBeforeStartDoesNotProduceTwoOutcomes();
    void shutdownWithdrawsPendingPersistence();
    void serviceTeardownFailsPendingPreparation();
    void hostSchemeSelectionAndBoundedSameUrlReopen();
    void decoderOpenFailureDoesNotRetry();
    void shutdownWithdrawsPendingPreparation();
    void stopTokenWakesPendingPreparation();
    void stopBeforeMainThreadPreparationNeverStartsRefresh();
    void abortDuringInnerInitializationIsForwarded();
    void shutdownAbortsInnerInitialization();
    void preparationRejectsMainThreadCall();
    void delegateExceptionLatchesPlaybackFailure();
    void playbackMakesSingleAttemptPerPage();
};

void PlaybackTests::cachedPreparationDoesNotCreateGateway()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    const qint64 now          = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    const Fooyin::Track track = playbackTrack(now + 7'200'000);
    BandcampPlaybackPreparation preparation{&service, track, {}, {}};
    QString streamUrl;
    int preparedCount{0};
    int finishedCount{0};
    int failedCount{0};
    connect(&preparation, &BandcampPlaybackPreparation::prepared, &preparation, [&](const QString& value) {
        streamUrl = value;
        ++preparedCount;
    });
    connect(&preparation, &BandcampPlaybackPreparation::failed, &preparation, [&]() { ++failedCount; });
    connect(&preparation, &BandcampPlaybackPreparation::finished, &preparation, [&]() { ++finishedCount; });
    preparation.start(now);

    QCOMPARE(preparedCount, 1);
    QCOMPARE(finishedCount, 1);
    QCOMPARE(failedCount, 0);
    QCOMPARE(streamUrl, track.extraProperties().value(BandcampStreamUrlProperty));
    QVERIFY(network.requested.isEmpty());
    QVERIFY(std::none_of(preparation.children().cbegin(), preparation.children().cend(),
                         [](QObject* child) { return dynamic_cast<BandcampLibraryGateway*>(child) != nullptr; }));
}

void PlaybackTests::refreshedPreparationDoesNotWaitForPersistence()
{
    for(int scenario = 0; scenario < 3; ++scenario) {
        PlaybackLibrary host;
        PlaybackFixture fixture{1, host.api};
        const qint64 now         = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
        const qint64 freshExpiry = now + 10'800'000;
        Fooyin::Track original   = playbackTrack(scenario == 1 ? now + 7'200'000 : now - 60'000);
        original.setId(123);
        if(scenario == 1) {
            original.setExtraProperty(BandcampExpiresAtProperty, u"1"_s);
        }
        host.library      = {original};
        auto playlistCopy = original;
        playlistCopy.setId(124);
        playlistCopy.setTitle(u"Playlist edit"_s);
        host.playlist = {playlistCopy};
        fixture.network->setResponder([now, freshExpiry](const QNetworkRequest&) {
            return FakeResponse{.body
                                = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                            {playbackEntry(789, now + 7'200'000), playbackEntry(456, freshExpiry)})};
        });

        auto bridge = fixture.bridge;
        auto work   = std::async(std::launch::async, [bridge, original]() { return bridge->prepare(original, {}); });
        QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
        const auto result = work.get();
        QVERIFY(std::holds_alternative<BandcampPlaybackBridge::PreparedTransport>(result));
        QCOMPARE(std::get<BandcampPlaybackBridge::PreparedTransport>(result).streamUrl, playbackStream(freshExpiry));
        QTRY_COMPARE_WITH_TIMEOUT(host.writes.size(), std::size_t{1}, 3000);
        QVERIFY(host.pending);
        QVERIFY(!host.pending->future().isFinished());
        const auto& written = host.writes.front().front();
        QCOMPARE(written.id(), 123);
        QCOMPARE(written.filepath(), original.filepath());
        QCOMPARE(written.title(), u"User-edited title"_s);
        QCOMPARE(written.extraTag(u"MOOD"_s), original.extraTag(u"MOOD"_s));
        QCOMPARE(written.extraProperties().value(u"_USER_PROPERTY"_s), u"preserve me"_s);
        QCOMPARE(written.extraProperties().value(BandcampStreamUrlProperty), playbackStream(freshExpiry));
        QCOMPARE(fixture.network->requested.size(), 1);

        host.finishWrite(scenario != 2);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.service->findChildren<BandcampPlaybackPreparation*>().isEmpty(), 3000);
    }
}

void PlaybackTests::cancellationDuringPlaybackHandoffSkipsPersistence()
{
    PlaybackLibrary host;
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    const qint64 now       = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    Fooyin::Track original = playbackTrack(now - 60'000);
    original.setId(123);
    host.library = {original};
    network.setResponder([now](const QNetworkRequest&) {
        return FakeResponse{
            .body = albumPage(R"({"type":"album","id":40,"title":"Parent"})", {playbackEntry(456, now + 7'200'000)})};
    });

    BandcampPlaybackPreparation preparation{&service, original, {}, host.api};
    int prepared{0};
    int failed{0};
    int finished{0};
    connect(&preparation, &BandcampPlaybackPreparation::prepared, &preparation, [&](const QString&) {
        ++prepared;
        preparation.cancel();
    });
    connect(&preparation, &BandcampPlaybackPreparation::failed, &preparation, [&]() { ++failed; });
    connect(&preparation, &BandcampPlaybackPreparation::finished, &preparation, [&]() { ++finished; });
    preparation.start(now);

    QTRY_COMPARE_WITH_TIMEOUT(finished, 1, 3000);
    QCOMPARE(prepared, 1);
    QCOMPARE(failed, 0);
    QCOMPARE(network.requested.size(), 1);
    QCOMPARE(host.libraryReads, 0);
    QVERIFY(host.writes.empty());
    QVERIFY(!preparation.findChild<BandcampLibraryGateway*>());
    QCoreApplication::processEvents();
    QCOMPARE(finished, 1);
}

void PlaybackTests::invalidAndUnusablePlaybackTransportsFail()
{
    FakeNetworkManager network;
    BandcampResolutionService service{&network};
    const qint64 now          = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    const Fooyin::Track valid = playbackTrack(now + 7'200'000);
    for(const auto& [property, value] :
        QList<QPair<QString, QString>>{{StreamProviderProperty, u"other"_s},
                                       {BandcampSourceUrlProperty, u"https://artist.bandcamp.com/music"_s},
                                       {BandcampStreamUrlProperty, u"http://t4.bcbits.com/stream/abc?ts=123"_s},
                                       {BandcampTrackIdProperty, u"999"_s}}) {
        auto invalid = valid;
        invalid.setExtraProperty(property, value);
        BandcampPlaybackPreparation preparation{&service, invalid, {}, {}};
        int failures{0};
        int finishes{0};
        connect(&preparation, &BandcampPlaybackPreparation::failed, &preparation, [&]() { ++failures; });
        connect(&preparation, &BandcampPlaybackPreparation::finished, &preparation, [&]() { ++finishes; });
        preparation.start(now);
        QCOMPARE(failures, 1);
        QCOMPARE(finishes, 1);
        QVERIFY(preparation.children().isEmpty());
    }
    QVERIFY(network.requested.isEmpty());

    for(int scenario = 0; scenario < 2; ++scenario) {
        network.setResponder([now, scenario](const QNetworkRequest&) {
            const qint64 id     = scenario == 0 ? 789 : 456;
            const qint64 expiry = scenario == 0 ? now + 7'200'000 : now - 60'000;
            return FakeResponse{
                .body = albumPage(R"({"type":"album","id":40,"title":"Parent"})", {playbackEntry(id, expiry)})};
        });
        BandcampPlaybackPreparation preparation{&service, playbackTrack(now - 60'000), {}, {}};
        int failures{0};
        int finishes{0};
        int prepared{0};
        connect(&preparation, &BandcampPlaybackPreparation::failed, &preparation, [&]() { ++failures; });
        connect(&preparation, &BandcampPlaybackPreparation::finished, &preparation, [&]() { ++finishes; });
        connect(&preparation, &BandcampPlaybackPreparation::prepared, &preparation, [&]() { ++prepared; });
        preparation.start(now);
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 1, 3000);
        QCOMPARE(failures, 1);
        QCOMPARE(prepared, 0);
        QVERIFY(preparation.children().isEmpty());
    }
    QCOMPARE(network.requested.size(), 2);
}

void PlaybackTests::shutdownWithdrawsPendingPersistence()
{
    PlaybackLibrary host;
    PlaybackFixture fixture{1, host.api};
    const qint64 now       = (QDateTime::currentMSecsSinceEpoch() / 1000) * 1000;
    Fooyin::Track original = playbackTrack(now - 60'000);
    original.setId(123);
    host.library = {original};
    fixture.network->setResponder([now](const QNetworkRequest&) {
        return FakeResponse{
            .body = albumPage(R"({"type":"album","id":40,"title":"Parent"})", {playbackEntry(456, now + 7'200'000)})};
    });
    auto bridge = fixture.bridge;
    auto work   = std::async(std::launch::async, [bridge, original]() { return bridge->prepare(original, {}); });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::PreparedTransport>(work.get()));
    QTRY_COMPARE_WITH_TIMEOUT(host.writes.size(), std::size_t{1}, 3000);

    fixture.bridge->shutdown();
    QCOMPARE(host.cancellations, 1);
    host.finishCancelledWrite();
    QTRY_VERIFY_WITH_TIMEOUT(fixture.service->findChildren<BandcampPlaybackPreparation*>().isEmpty(), 3000);
}

void PlaybackTests::serviceDestroyedBeforeStartDoesNotProduceTwoOutcomes()
{
    FakeNetworkManager network;
    auto service     = std::make_unique<BandcampResolutionService>(&network);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    BandcampPlaybackPreparation preparation{service.get(), playbackTrack(now - 60'000), {}, {}};
    int failures{0};
    int finishes{0};
    int prepared{0};
    connect(&preparation, &BandcampPlaybackPreparation::failed, &preparation, [&]() { ++failures; });
    connect(&preparation, &BandcampPlaybackPreparation::finished, &preparation, [&]() { ++finishes; });
    connect(&preparation, &BandcampPlaybackPreparation::prepared, &preparation, [&]() { ++prepared; });
    service.reset();
    QCOMPARE(failures, 0);
    preparation.start(now);
    QCOMPARE(failures, 1);
    QCOMPARE(finishes, 1);
    QCOMPARE(prepared, 0);
    preparation.start(now);
    preparation.cancel();
    QCOMPARE(failures, 1);
    QCOMPARE(finishes, 1);
    QVERIFY(network.requested.isEmpty());
}

void PlaybackTests::serviceTeardownFailsPendingPreparation()
{
    PlaybackFixture fixture{1};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    fixture.network->setResponder(
        [](const QNetworkRequest&) { return FakeResponse{.holdDelivery = true, .holdAfterAbort = true}; });
    const Fooyin::Track track = playbackTrack(now - 60'000);
    auto bridge               = fixture.bridge;
    auto work = std::async(std::launch::async, [bridge, track]() { return bridge->prepare(track, {}); });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->requested.size(), 1, 3000);
    fixture.service.reset();
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 1000);
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(work.get()));
    fixture.network->finishAbortedReplies();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->liveReplyCount(), qsizetype{0}, 2000);
}

void PlaybackTests::playbackMakesSingleAttemptPerPage()
{
    PlaybackFixture fixture{1};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    fixture.network->setResponder([](const QNetworkRequest&) { return FakeResponse{.status = 503}; });
    const Fooyin::Track track = playbackTrack(now - 60'000);
    auto bridge               = fixture.bridge;
    auto work = std::async(std::launch::async, [bridge, track]() { return bridge->prepare(track, {}); });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(work.get()));
    QCOMPARE(fixture.network->requested.size(), 1);
}

void PlaybackTests::hostSchemeSelectionAndBoundedSameUrlReopen()
{
    PlaybackFixture fixture{2};
    auto inner = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry            = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track original   = playbackTrack(expiry);
    const QString originalUri      = original.filepath();
    const QString originalTitle    = original.title();
    const QStringList originalMood = original.extraTag(u"MOOD"_s);
    const QString originalStream   = original.extraProperties().value(BandcampStreamUrlProperty);
    auto candidates                = fixture.audioLoader->decodersForTrack(original);
    QCOMPARE(candidates.size(), std::size_t{1});
    QCOMPARE(candidates.front()->supportedSchemes(), QStringList{u"bandcamp"_s});

    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work                     = std::async(std::launch::async, [decoder, original]() mutable {
        decoder->setPlaybackHints(Fooyin::AudioDecoder::RepeatTrackEnabled);
        Fooyin::AudioSource source;
        source.filepath = original.filepath();
        const auto format
            = decoder->init(source, original, Fooyin::AudioDecoder::UpdateTracks | Fooyin::AudioDecoder::NoLooping);
        const bool initialized = format.has_value();
        const bool seekable    = decoder->isSeekable();
        decoder->seek(137339);
        decoder->stop();
        return std::pair{initialized, seekable};
    });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    const auto [initialized, seekable] = work.get();
    QVERIFY(initialized);
    QVERIFY(!seekable);
    QCOMPARE(inner->initCalls, 1);
    QCOMPARE(inner->sourcePath, u"https://bandcamp.invalid/track.mp3"_s);
    QCOMPARE(inner->trackPath, u"https://bandcamp.invalid/track.mp3"_s);
    QVERIFY(inner->trackRemote);
    QVERIFY(!inner->sourcePath.contains(u"signature="_s));
    QVERIFY(!inner->trackPath.contains(u"signature="_s));
    QCOMPARE(inner->seekCalls, 0);
    QVERIFY(!inner->hadTransportProperty);
    QVERIFY(inner->sourceOpen);
    QVERIFY(inner->firstBytePreserved);
    QVERIFY(inner->hasRemoteDevice);
    QVERIFY(inner->receivedPlaybackHint);
    QCOMPARE(inner->options, Fooyin::AudioDecoder::UpdateTracks | Fooyin::AudioDecoder::NoLooping);
    QCOMPARE(inner->stopCalls, 1);
    QCOMPARE(fixture.remoteState->created.load(), 2);
    QCOMPARE(fixture.remoteState->openCalls.load(), 2);
    QVERIFY(fixture.remoteState->reconnectDisabled.load() >= 2);
    QCOMPARE(fixture.remoteState->readinessWaitCalls.load(), 2);
    QCOMPARE(fixture.remoteState->readsBeforeReadiness.load(), 0);
    QVERIFY(!fixture.remoteState->nonBlockingEnabled.load(std::memory_order_acquire));
    {
        const std::scoped_lock lock{fixture.remoteState->mutex};
        QCOMPARE(fixture.remoteState->urls.size(), 2);
        QCOMPARE(fixture.remoteState->urls.at(0), QUrl{originalStream});
        QCOMPARE(fixture.remoteState->urls.at(1), QUrl{originalStream});
        QVERIFY(fixture.remoteState->urls.at(0).query().contains(u"signature=private"_s));
        QVERIFY(fixture.remoteState->urls.at(0).query().contains(u"ts="_s));
    }
    QVERIFY(fixture.network->requested.isEmpty());
    QCOMPARE(original.filepath(), originalUri);
    QCOMPARE(original.title(), originalTitle);
    QCOMPARE(original.extraTag(u"MOOD"_s), originalMood);
    QCOMPARE(original.extraProperties().value(BandcampStreamUrlProperty), originalStream);
}

void PlaybackTests::decoderOpenFailureDoesNotRetry()
{
    PlaybackFixture fixture{1};
    auto inner    = std::make_shared<InnerState>();
    inner->accept = false;
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry       = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track track = playbackTrack(expiry);
    auto candidates           = fixture.audioLoader->decodersForTrack(track);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work                     = std::async(std::launch::async, [decoder, track]() {
        Fooyin::AudioSource source;
        source.filepath = track.filepath();
        return decoder->init(source, track, Fooyin::AudioDecoder::None).has_value();
    });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    QVERIFY(!work.get());
    QCOMPARE(inner->initCalls, 1);
    QCOMPARE(fixture.remoteState->created.load(), 1);
    QCOMPARE(fixture.remoteState->openCalls.load(), 1);
}

void PlaybackTests::shutdownWithdrawsPendingPreparation()
{
    PlaybackFixture fixture{1};
    const qint64 now    = QDateTime::currentMSecsSinceEpoch();
    const qint64 expiry = now - 60'000;
    fixture.network->setResponder([now](const QNetworkRequest&) {
        const auto responseBody = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                            {streamEntry(R"("id":456,"title":"Selected")")});
        return FakeResponse{.body = responseBody, .holdDelivery = true, .holdAfterAbort = true};
    });
    const Fooyin::Track track = playbackTrack(expiry);
    auto bridge               = fixture.bridge;
    auto work = std::async(std::launch::async, [bridge, track]() { return bridge->prepare(track, {}); });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->requested.size(), 1, 5000);
    fixture.bridge->shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 2000);
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(work.get()));
    Fooyin::RemoteStreamSource lateSource;
    QVERIFY(!fixture.bridge->createRemoteStreamSource(QUrl{playbackStream(now)}, lateSource));
    fixture.network->finishAbortedReplies();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->liveReplyCount(), qsizetype{0}, 2000);
    fixture.bridge.reset();
    QCoreApplication::processEvents();
}

void PlaybackTests::preparationRejectsMainThreadCall()
{
    PlaybackFixture fixture{1};
    const qint64 expiry = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const auto result   = fixture.bridge->prepare(playbackTrack(expiry), {});
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(result));
    QVERIFY(fixture.network->requested.isEmpty());
}

void PlaybackTests::delegateExceptionLatchesPlaybackFailure()
{
    PlaybackFixture fixture{1};
    auto inner          = std::make_shared<InnerState>();
    inner->throwOnStart = true;
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry       = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track track = playbackTrack(expiry);
    auto candidates           = fixture.audioLoader->decodersForTrack(track);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    auto work = std::async(std::launch::async, [decoder, track]() {
        Fooyin::AudioSource source;
        source.filepath   = track.filepath();
        const auto format = decoder->init(source, track, Fooyin::AudioDecoder::None);
        if(!format) {
            return false;
        }
        decoder->start();
        const auto result = decoder->readAudio(1);
        decoder->stop();
        return result.status == Fooyin::AudioDecoder::ReadStatus::Error;
    });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    QVERIFY(work.get());
    QCOMPARE(inner->startCalls, 1);
    QCOMPARE(inner->readCalls, 0);
    QVERIFY(std::any_of(capturedLogs().cbegin(), capturedLogs().cend(),
                        [](const CapturedLog& log) { return log.message.contains(u"playback_start_failed"_s); }));
    QVERIFY(std::none_of(capturedLogs().cbegin(), capturedLogs().cend(), [](const CapturedLog& log) {
        return log.message.contains(u"signature=private"_s) || log.message.contains(u"private decoder error"_s);
    }));
}

void PlaybackTests::abortDuringInnerInitializationIsForwarded()
{
    PlaybackFixture fixture{1};
    auto inner                 = std::make_shared<InnerState>();
    inner->blockInitialization = true;
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry          = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track original = playbackTrack(expiry);
    auto candidates              = fixture.audioLoader->decodersForTrack(original);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work                     = std::async(std::launch::async, [decoder, original]() {
        Fooyin::AudioSource source;
        source.filepath = original.filepath();
        return decoder->init(source, original, Fooyin::AudioDecoder::None).has_value();
    });
    QTRY_VERIFY_WITH_TIMEOUT(inner->initEntered.load(std::memory_order_acquire), 3000);
    decoder->requestAbort();
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 1000);
    QVERIFY(!work.get());
    QVERIFY(inner->initAbortObserved.load(std::memory_order_acquire));
    QCOMPARE(inner->stopCalls, 1);
    QCOMPARE(fixture.remoteState->created.load(), 1);
}

void PlaybackTests::shutdownAbortsInnerInitialization()
{
    PlaybackFixture fixture{1};
    auto inner                 = std::make_shared<InnerState>();
    inner->blockInitialization = true;
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry          = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track original = playbackTrack(expiry);
    auto candidates              = fixture.audioLoader->decodersForTrack(original);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work                     = std::async(std::launch::async, [decoder, original]() {
        Fooyin::AudioSource source;
        source.filepath = original.filepath();
        return decoder->init(source, original, Fooyin::AudioDecoder::None).has_value();
    });
    QTRY_VERIFY_WITH_TIMEOUT(inner->initEntered.load(std::memory_order_acquire), 3000);
    fixture.bridge->shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 1000);
    QVERIFY(!work.get());
    QVERIFY(inner->initAbortObserved.load(std::memory_order_acquire));
    QCOMPARE(inner->stopCalls, 1);
}

void PlaybackTests::stopTokenWakesPendingPreparation()
{
    PlaybackFixture fixture{1};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    fixture.network->setResponder([](const QNetworkRequest&) {
        const auto responseBody = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                            {streamEntry(R"("id":456,"title":"Selected")")});
        return FakeResponse{.body = responseBody, .holdDelivery = true, .holdAfterAbort = true};
    });
    const Fooyin::Track track = playbackTrack(now - 60'000);
    std::stop_source cancellation;
    auto bridge = fixture.bridge;
    auto work   = std::async(std::launch::async, [bridge, track, token = cancellation.get_token()]() {
        return bridge->prepare(track, token);
    });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->requested.size(), 1, 5000);
    cancellation.request_stop();
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 500);
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(work.get()));
    fixture.bridge->shutdown();
    fixture.network->finishAbortedReplies();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->liveReplyCount(), qsizetype{0}, 2000);
}

void PlaybackTests::stopBeforeMainThreadPreparationNeverStartsRefresh()
{
    PlaybackFixture fixture{1};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    fixture.network->setResponder([](const QNetworkRequest&) {
        const auto responseBody = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                            {streamEntry(R"("id":456,"title":"Selected")")});
        return FakeResponse{.body = responseBody, .holdDelivery = true, .holdAfterAbort = true};
    });
    const Fooyin::Track track = playbackTrack(now - 60'000);
    auto bridge               = fixture.bridge;

    // 1. Pre-cancelled stop_token: prepare() returns before the queued main-thread
    //    beginPreparation runs, and pumping the main thread afterwards must not start a refresh.
    {
        std::stop_source preCancelled;
        preCancelled.request_stop();
        auto work = std::async(std::launch::async, [bridge, track, token = preCancelled.get_token()]() {
            return bridge->prepare(track, token);
        });
        QCOMPARE(work.wait_for(std::chrono::milliseconds{1000}), std::future_status::ready);
        const auto outcome = work.get();
        QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(outcome));
        QCOMPARE(std::get<BandcampPlaybackBridge::Failure>(outcome).reason, u"Playback preparation was cancelled."_s);
        QCoreApplication::processEvents();
        QVERIFY(fixture.network->requested.isEmpty());
    }

    // 2. Stop requested while beginPreparation is still queued on the main thread.
    {
        std::stop_source immediateStop;
        std::atomic_bool enteredPrepare{false};
        auto work
            = std::async(std::launch::async, [bridge, track, &enteredPrepare, token = immediateStop.get_token()]() {
                  enteredPrepare.store(true, std::memory_order_release);
                  return bridge->prepare(track, token);
              });
        while(!enteredPrepare.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        immediateStop.request_stop();
        QCOMPARE(work.wait_for(std::chrono::milliseconds{1000}), std::future_status::ready);
        const auto outcome = work.get();
        QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(outcome));
        QCOMPARE(std::get<BandcampPlaybackBridge::Failure>(outcome).reason, u"Playback preparation was cancelled."_s);
        QCoreApplication::processEvents();
        QVERIFY(fixture.network->requested.isEmpty());
    }
}
} // namespace

int runPlaybackTests(int argc, char** argv)
{
    PlaybackTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampplaybacktests.moc"
