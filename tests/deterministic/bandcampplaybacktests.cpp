#include "bandcampplayback.h"
#include "bandcampfakenetwork.h"
#include "bandcamplogcapture.h"
#include "bandcamptestpages.h"

#include <core/engine/audioloader.h>
#include <core/network/remoteioservice.h>
#include <core/network/remotestreamdevice.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QString AlbumUrl{u"https://artist.bandcamp.com/album/parent"_s};

QString playbackStream(qint64 expiryMs)
{
    return u"https://t4.bcbits.com/stream/playback?signature=private&ts="_s
        + QString::number(expiryMs / 1000);
}

Fooyin::Track playbackTrack(qint64 expiryMs)
{
    BandcampTrack source;
    source.id = 456;
    source.title = u"Source title"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(expiryMs);
    auto track = makeFooyinTrack({source, std::nullopt});
    track.setTitle(u"User-edited title"_s);
    track.replaceExtraTag(u"MOOD"_s, u"User mood"_s);
    track.setExtraProperty(u"_USER_PROPERTY"_s, u"preserve me"_s);
    return track;
}

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

class FixtureRemoteDevice final : public QIODevice, public Fooyin::RemoteStreamDevice
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
        source.openTimeout = std::chrono::seconds{5};
        source.device = std::move(device);
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
        m_state->sourcePath = source.filepath;
        m_state->trackPath = track.filepath();
        m_state->options = options;
        m_state->receivedPlaybackHint = playbackHints().testFlag(RepeatTrackEnabled);
        m_state->hadTransportProperty = track.hasExtraProperty(BandcampStreamUrlProperty);
        m_state->sourceOpen = source.device && source.device->isOpen();
        m_state->firstBytePreserved = source.device && source.device->bytesAvailable() > 0;
        m_state->hasRemoteDevice = source.remoteStreamDevice != nullptr;
        m_state->trackRemote = track.isRemote();
        if(m_state->blockInitialization) {
            std::unique_lock lock{m_state->initMutex};
            m_state->initEntered.store(true, std::memory_order_release);
            m_state->initChanged.notify_all();
            const bool aborted = m_state->initChanged.wait_for(
                lock, std::chrono::milliseconds{1500}, [this]() { return abortToken().stop_requested(); });
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
    PlaybackFixture(int remoteOpenSucceedsOn)
    {
        settingsPath = QCoreApplication::applicationDirPath()
            + QStringLiteral("/bandcamp-playback-settings-%1.ini").arg(QCoreApplication::applicationPid());
        QFile::remove(settingsPath);
        settings = std::make_unique<Fooyin::SettingsManager>(settingsPath);
        network = std::make_shared<FakeNetworkManager>();
        service = std::make_unique<BandcampService>(network.get());
        audioLoader = std::make_shared<Fooyin::AudioLoader>();
        remoteState = std::make_shared<RemoteState>();
        remoteState->succeedsOn = remoteOpenSucceedsOn;
        remoteIo = std::make_unique<FixtureRemoteIo>(network, settings.get(), remoteState);
        bridge = std::make_shared<BandcampPlaybackBridge>(service.get(), audioLoader, remoteIo.get(),
                                                          settings.get(), FooyinLibraryApi{});
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
    std::unique_ptr<BandcampService> service;
    std::shared_ptr<Fooyin::AudioLoader> audioLoader;
    std::shared_ptr<RemoteState> remoteState;
    std::unique_ptr<FixtureRemoteIo> remoteIo;
    std::shared_ptr<BandcampPlaybackBridge> bridge;
};

class PlaybackTests : public QObject
{
    Q_OBJECT

private slots:
    void hostSchemeSelectionAndBoundedSameUrlReopen();
    void decoderOpenFailureDoesNotRetry();
    void shutdownWithdrawsPendingPreparation();
    void stopTokenWakesPendingPreparation();
    void stopBeforeMainThreadPreparationNeverStartsRefresh();
    void abortDuringInnerInitializationIsForwarded();
    void shutdownAbortsInnerInitialization();
    void preparationRejectsMainThreadCall();
    void delegateExceptionLatchesPlaybackFailure();
};

void PlaybackTests::hostSchemeSelectionAndBoundedSameUrlReopen()
{
    PlaybackFixture fixture{2};
    auto inner = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(u"Fixture MP3"_s, [inner]() {
        return std::make_unique<FixtureRemoteDecoder>(inner);
    }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track original = playbackTrack(expiry);
    const QString originalUri = original.filepath();
    const QString originalTitle = original.title();
    const QStringList originalMood = original.extraTag(u"MOOD"_s);
    const QString originalStream = original.extraProperties().value(BandcampStreamUrlProperty);
    auto candidates = fixture.audioLoader->decodersForTrack(original);
    QCOMPARE(candidates.size(), std::size_t{1});
    QCOMPARE(candidates.front()->supportedSchemes(), QStringList{u"bandcamp"_s});

    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work = std::async(std::launch::async, [decoder, original]() mutable {
        decoder->setPlaybackHints(Fooyin::AudioDecoder::RepeatTrackEnabled);
        Fooyin::AudioSource source;
        source.filepath = original.filepath();
        const auto format = decoder->init(source, original,
                                          Fooyin::AudioDecoder::UpdateTracks | Fooyin::AudioDecoder::NoLooping);
        const bool initialized = format.has_value();
        const bool seekable = decoder->isSeekable();
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
    auto inner = std::make_shared<InnerState>();
    inner->accept = false;
    fixture.audioLoader->addDecoder(u"Fixture MP3"_s, [inner]() {
        return std::make_unique<FixtureRemoteDecoder>(inner);
    }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track track = playbackTrack(expiry);
    auto candidates = fixture.audioLoader->decodersForTrack(track);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work = std::async(std::launch::async, [decoder, track]() {
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 expiry = now - 60'000;
    fixture.network->setResponder([now](const QNetworkRequest&) {
        const auto responseBody = albumPage(R"({"type":"album","id":40,"title":"Parent"})",
                                            {streamEntry(R"("id":456,"title":"Selected")")});
        return FakeResponse{.body = responseBody, .holdDelivery = true, .holdAfterAbort = true};
    });
    const Fooyin::Track track = playbackTrack(expiry);
    auto bridge = fixture.bridge;
    auto work = std::async(std::launch::async, [bridge, track]() {
        return bridge->prepare(track, {});
    });
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
    const auto result = fixture.bridge->prepare(playbackTrack(expiry), {});
    QVERIFY(std::holds_alternative<BandcampPlaybackBridge::Failure>(result));
    QVERIFY(fixture.network->requested.isEmpty());
}

void PlaybackTests::delegateExceptionLatchesPlaybackFailure()
{
    PlaybackFixture fixture{1};
    auto inner = std::make_shared<InnerState>();
    inner->throwOnStart = true;
    fixture.audioLoader->addDecoder(u"Fixture MP3"_s, [inner]() {
        return std::make_unique<FixtureRemoteDecoder>(inner);
    }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track track = playbackTrack(expiry);
    auto candidates = fixture.audioLoader->decodersForTrack(track);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    auto work = std::async(std::launch::async, [decoder, track]() {
        Fooyin::AudioSource source;
        source.filepath = track.filepath();
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
    QVERIFY(std::any_of(capturedLogs().cbegin(), capturedLogs().cend(), [](const CapturedLog& log) {
        return log.message.contains(u"playback.start.failed"_s);
    }));
    QVERIFY(std::none_of(capturedLogs().cbegin(), capturedLogs().cend(), [](const CapturedLog& log) {
        return log.message.contains(u"signature=private"_s) || log.message.contains(u"private decoder error"_s);
    }));
}

void PlaybackTests::abortDuringInnerInitializationIsForwarded()
{
    PlaybackFixture fixture{1};
    auto inner = std::make_shared<InnerState>();
    inner->blockInitialization = true;
    fixture.audioLoader->addDecoder(u"Fixture MP3"_s, [inner]() {
        return std::make_unique<FixtureRemoteDecoder>(inner);
    }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track original = playbackTrack(expiry);
    auto candidates = fixture.audioLoader->decodersForTrack(original);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work = std::async(std::launch::async, [decoder, original]() {
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
    auto inner = std::make_shared<InnerState>();
    inner->blockInitialization = true;
    fixture.audioLoader->addDecoder(u"Fixture MP3"_s, [inner]() {
        return std::make_unique<FixtureRemoteDecoder>(inner);
    }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    const qint64 expiry = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    const Fooyin::Track original = playbackTrack(expiry);
    auto candidates = fixture.audioLoader->decodersForTrack(original);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work = std::async(std::launch::async, [decoder, original]() {
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
    auto work = std::async(std::launch::async, [bridge, track, token = cancellation.get_token()]() {
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
    auto bridge = fixture.bridge;

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
        QCOMPARE(std::get<BandcampPlaybackBridge::Failure>(outcome).reason,
                 u"Playback preparation was cancelled."_s);
        QCoreApplication::processEvents();
        QVERIFY(fixture.network->requested.isEmpty());
    }

    // 2. Stop requested while beginPreparation is still queued on the main thread.
    {
        std::stop_source immediateStop;
        std::atomic_bool enteredPrepare{false};
        auto work = std::async(std::launch::async,
                               [bridge, track, &enteredPrepare, token = immediateStop.get_token()]() {
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
        QCOMPARE(std::get<BandcampPlaybackBridge::Failure>(outcome).reason,
                 u"Playback preparation was cancelled."_s);
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
