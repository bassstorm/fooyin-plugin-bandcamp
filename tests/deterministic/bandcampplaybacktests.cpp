#include "bandcampfakenetwork.h"
#include "bandcamplogcapture.h"
#include "bandcampplayback.h"
#include "bandcampplaybackpreparation.h"
#include "bandcampstreamprobe.h"
#include "bandcamptestpages.h"
#include "bandcamptrackuri.h"

#include <core/engine/audioloader.h>
#include <core/network/remoteioservice.h>
#include <core/network/remotestreamdevice.h>
#include <core/scripting/scriptparser.h>
#include <core/scripting/scriptregistry.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QPromise>
#include <QtTest>

#include <algorithm>
#include <array>
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
    std::atomic_int bufferChecks{0};
    std::atomic_int readsBeforeReadiness{0};
    std::atomic_int readsWithoutCancellationToken{0};
    std::atomic_int reconnectDisabled{0};
    std::atomic_bool nonBlockingEnabled{false};
    std::atomic_bool reconnectEnabled{true};
    std::atomic_int cancellationTokensInstalled{0};
    std::atomic_int cancellationTokensCleared{0};
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
        ++m_state->bufferChecks;
        m_bufferReady = m_attempt >= m_state->succeedsOn;
        return m_bufferReady ? 1 : 0;
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
        m_state->reconnectEnabled.store(enabled, std::memory_order_release);
    }

    void setReadCancellationToken(std::stop_token token) override
    {
        token.stop_possible() ? ++m_state->cancellationTokensInstalled : ++m_state->cancellationTokensCleared;
        m_cancellation = token;
    }

    bool open(OpenMode mode) override
    {
        ++m_state->openCalls;
        m_cancellation = {};
        return QIODevice::open(mode);
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if(!m_bufferReady) {
            ++m_state->readsBeforeReadiness;
        }
        if(!m_cancellation.stop_possible()) {
            ++m_state->readsWithoutCancellationToken;
        }
        if(m_cancellation.stop_requested()) {
            return -1;
        }
        if(maxSize <= 0) {
            return 0;
        }
        std::fill_n(data, maxSize, 'x');
        return maxSize;
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1;
    }

private:
    std::shared_ptr<RemoteState> m_state;
    int m_attempt{0};
    mutable bool m_bufferReady{false};
    std::stop_token m_cancellation;
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
        source.openTimeout  = std::chrono::milliseconds{100};
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
    bool reportsChangedTrack{false};
    Fooyin::Track changedTrack;
    Fooyin::SampleFormat sampleFormat{Fooyin::SampleFormat::S16};
    int sampleRate{44'100};
    int channels{2};
    bool consumeBeforeReject{false};
    bool readToReadLimit{false};
    bool readExactlyLimit{false};
    bool inspectDeviceContract{false};
    bool sourceIsSequential{false};
    bool sourceSeekSucceeded{false};
    QString inputCodec;
    QString inputEncoding;
    int inputSampleRate{0};
    int inputChannels{0};
    int inputBitDepth{0};
    int inputBitrate{0};
    bool seekable{true};
    bool accept{true};
    bool throwOnStart{false};
    bool throwOnInit{false};
    bool blockInitialization{false};
    std::atomic_bool initEntered{false};
    std::atomic_bool initAbortObserved{false};
    std::mutex initMutex;
    std::condition_variable initChanged;
    int initCalls{0};
    qint64 readBytes{0};
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
        m_state->inputCodec           = track.codec();
        m_state->inputEncoding        = track.encoding();
        m_state->inputSampleRate      = track.sampleRate();
        m_state->inputChannels        = track.channels();
        m_state->inputBitDepth        = track.bitDepth();
        m_state->inputBitrate         = track.bitrate();
        if(m_state->inspectDeviceContract && source.device) {
            m_state->sourceIsSequential  = source.device->isSequential();
            m_state->sourceSeekSucceeded = source.device->seek(100'000);
        }
        if(m_state->consumeBeforeReject && source.device) {
            char byte = '\0';
            source.device->read(&byte, 1);
        }
        if(m_state->readToReadLimit && source.device) {
            std::array<char, 64 * 1024> buffer{};
            qint64 count = 0;
            do {
                count = source.device->read(buffer.data(), static_cast<qint64>(buffer.size()));
                if(count > 0) {
                    m_state->readBytes += count;
                }
            } while(count > 0);
        }
        if(m_state->readExactlyLimit && source.device) {
            QByteArray buffer(1024 * 1024, '\0');
            m_state->readBytes = source.device->read(buffer.data(), buffer.size());
        }
        if(m_state->throwOnInit) {
            throw std::runtime_error{"private decoder initialization error"};
        }
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
        return Fooyin::AudioFormat{m_state->sampleFormat, m_state->sampleRate, m_state->channels};
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

    bool trackHasChanged() const override
    {
        return m_state->reportsChangedTrack && m_state->options.testFlag(UpdateTracks);
    }

    Fooyin::Track changedTrack() const override
    {
        return m_state->changedTrack;
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
    PlaybackFixture(int remoteOpenSucceedsOn, FooyinLibraryApi libraryApi = {},
                    BandcampLibraryGateway* persistenceGateway = nullptr, QObject* persistenceContext = nullptr)
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
        bridge
            = std::make_shared<BandcampPlaybackBridge>(service.get(), audioLoader, remoteIo.get(), settings.get(),
                                                       std::move(libraryApi), persistenceGateway, persistenceContext);
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
    void streamProbeExtractsDecodedProperties_data();
    void streamProbeExtractsDecodedProperties();
    void streamProbeUsesHostRemoteBuffer();
    void playbackUsesHostRemoteBuffer();
    void cancellationWhileWaitingForHostRemoteBuffer();
    void streamProbeUsesFormatWithoutInheritedTrackProperties();
    void streamProbeRetriesWithFreshRemoteSources();
    void streamProbeRejectsReadLimitAfterValidFormat();
    void streamProbeReadLimitStopsFallback();
    void streamProbePreservesWithinBudgetSuccess();
    void streamProbeReportsTimeoutAndCapacityOutcomes();
    void streamProbeFailureDiagnosticsSurviveDestroyedContext();
    void streamProbeCancellationIsSilent();
    void streamProbeLogsRecoveredDecoderExceptions();
    void probeShutdownDetachesWorkerWithoutCallback();
    void playbackPropertiesPreserveTrackAndPersistByBandcampIdentity_data();
    void playbackPropertiesPreserveTrackAndPersistByBandcampIdentity();
    void runtimePlaybackPropertiesAreSurfacedAndPersisted();
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

void PlaybackTests::streamProbeExtractsDecodedProperties_data()
{
    QTest::addColumn<QString>("codec");
    QTest::addColumn<QString>("encoding");
    QTest::addColumn<Fooyin::SampleFormat>("sampleFormat");
    QTest::addColumn<int>("bitDepth");
    QTest::newRow("mp3-float-pcm") << u"MP3"_s << u"Lossy"_s << Fooyin::SampleFormat::F32 << -1;
    QTest::newRow("flac-24-bit") << u"FLAC"_s << u"Lossless"_s << Fooyin::SampleFormat::S24In32 << 24;
}

void PlaybackTests::streamProbeExtractsDecodedProperties()
{
    QFETCH(QString, codec);
    QFETCH(QString, encoding);
    QFETCH(Fooyin::SampleFormat, sampleFormat);
    QFETCH(int, bitDepth);
    PlaybackFixture fixture{1};
    auto inner                 = std::make_shared<InnerState>();
    inner->reportsChangedTrack = true;
    inner->sampleFormat        = sampleFormat;
    inner->sampleRate          = 48'000;
    inner->channels            = 1;
    inner->changedTrack.setCodec(codec);
    inner->changedTrack.setEncoding(encoding);
    inner->changedTrack.setChannels(2);
    inner->changedTrack.setBitDepth(Fooyin::AudioFormat{sampleFormat, inner->sampleRate, inner->channels}.bitdepth());
    inner->changedTrack.setBitrate(256);
    fixture.audioLoader->addDecoder(
        u"Fixture decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 456;
    source.title     = u"Probe target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);
    const BandcampResolvedTrack resolved{source, std::nullopt};

    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> result;
    const auto submission = probe.probe({resolved}, &callbackContext, [&](QList<BandcampStreamProbeResult> values) {
        result    = std::move(values);
        completed = true;
    });
    QVERIFY(submission.token > 0);
    QVERIFY(!submission.rejection);

    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.front().trackId, 456);
    QVERIFY(!result.front().failure);
    QCOMPARE(result.front().properties.codec, codec);
    QCOMPARE(result.front().properties.encoding, encoding);
    QCOMPARE(result.front().properties.sampleRate, 48'000);
    QCOMPARE(result.front().properties.channels, 1);
    QCOMPARE(result.front().properties.bitDepth, bitDepth);
    QCOMPARE(result.front().properties.bitrate, 256);
    const Fooyin::Track projected = makeFooyinImportTrack({resolved, result.front().properties});
    QCOMPARE(projected.bitDepth(), bitDepth);
    Fooyin::ScriptRegistry registry;
    registry.finalise();
    Fooyin::ScriptParser scripting{&registry};
    const QString displayedDepth = bitDepth > 0 ? QString::number(bitDepth) : QString{};
    QCOMPARE(projected.techInfo(u"BITDEPTH"_s), displayedDepth);
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, projected), displayedDepth);
    QCOMPARE(scripting.evaluate(u"%encoding%"_s, projected), encoding);
    QCOMPARE(inner->options, Fooyin::AudioDecoder::UpdateTracks);
    QCOMPARE(inner->initCalls, 1);
    QCOMPARE(fixture.remoteState->created.load(std::memory_order_acquire), 1);
    QCOMPARE(fixture.remoteState->cancellationTokensInstalled.load(std::memory_order_acquire), 1);
    QCOMPARE(fixture.remoteState->cancellationTokensCleared.load(std::memory_order_acquire), 1);
    QCOMPARE(fixture.remoteState->readsWithoutCancellationToken.load(), 0);
    QVERIFY(!fixture.remoteState->nonBlockingEnabled.load(std::memory_order_acquire));
    QVERIFY(fixture.remoteState->reconnectEnabled.load(std::memory_order_acquire));
}

void PlaybackTests::streamProbeUsesHostRemoteBuffer()
{
    PlaybackFixture fixture{1};
    fixture.network->setResponder(
        [](const QNetworkRequest&) { return FakeResponse{.body = QByteArray(512, 'x'), .delayMs = 25}; });
    Fooyin::RemoteIoService remote{fixture.network, fixture.settings.get()};
    auto inner = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Fixture decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    BandcampTrack source;
    source.id        = 456;
    source.title     = u"Host remote probe target"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    BandcampStreamProbe probe{fixture.audioLoader, &remote};
    QObject callbackContext;
    int completions = 0;
    QList<BandcampStreamProbeResult> result;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            result = std::move(values);
                                            ++completions;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QCOMPARE(result.size(), 1);
    QVERIFY(!result.front().failure);
    QCOMPARE(result.front().properties.sampleRate, 44'100);
    QCOMPARE(result.front().properties.channels, 2);
    QCOMPARE(inner->initCalls, 1);
    QVERIFY(inner->firstBytePreserved);
    QCOMPARE(fixture.network->requested.size(), 1);
}

void PlaybackTests::playbackUsesHostRemoteBuffer()
{
    PlaybackFixture fixture{1};
    fixture.network->setResponder(
        [](const QNetworkRequest&) { return FakeResponse{.body = QByteArray(512, 'x'), .delayMs = 25}; });
    Fooyin::RemoteIoService remote{fixture.network, fixture.settings.get()};
    fixture.bridge = std::make_shared<BandcampPlaybackBridge>(fixture.service.get(), fixture.audioLoader, &remote,
                                                              fixture.settings.get(), FooyinLibraryApi{});
    auto inner     = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    auto decoder     = input.decoder();
    const auto track = playbackTrack(QDateTime::currentMSecsSinceEpoch() + 30 * 24 * 60 * 60'000LL);
    auto work        = std::async(std::launch::async, [&decoder, track]() {
        Fooyin::AudioSource source;
        source.filepath = track.filepath();
        return decoder->init(source, track, Fooyin::AudioDecoder::UpdateTracks).has_value();
    });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    QVERIFY(work.get());
    QCOMPARE(inner->initCalls, 1);
    QVERIFY(inner->firstBytePreserved);
    QCOMPARE(fixture.network->requested.size(), 1);
    decoder->stop();
    fixture.bridge->shutdown();
}

void PlaybackTests::cancellationWhileWaitingForHostRemoteBuffer()
{
    PlaybackFixture fixture{1};
    fixture.network->setResponder(
        [](const QNetworkRequest&) { return FakeResponse{.body = QByteArray(512, 'x'), .holdDelivery = true}; });
    Fooyin::RemoteIoService remote{fixture.network, fixture.settings.get()};
    fixture.bridge = std::make_shared<BandcampPlaybackBridge>(fixture.service.get(), fixture.audioLoader, &remote,
                                                              fixture.settings.get(), FooyinLibraryApi{});
    auto inner     = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    BandcampTrack source;
    source.id        = 456;
    source.title     = u"Cancelled host remote target"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 30 * 24 * 60 * 60'000LL);
    const auto track = makeFooyinTrack({source, std::nullopt});

    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    BandcampStreamProbe probe{fixture.audioLoader, &remote};
    QObject callbackContext;
    int completions = 0;
    QList<BandcampStreamProbeResult> result;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            result = std::move(values);
                                            ++completions;
                                        });
    QVERIFY(submission.token > 0);
    const auto input = bandcampInputCreator(fixture.bridge);
    auto decoder     = input.decoder();
    auto work        = std::async(std::launch::async, [&decoder, track]() {
        Fooyin::AudioSource inputSource;
        inputSource.filepath = track.filepath();
        return decoder->init(inputSource, track, Fooyin::AudioDecoder::UpdateTracks).has_value();
    });
    QTRY_COMPARE_WITH_TIMEOUT(fixture.network->requested.size(), 2, 5000);
    probe.cancel(submission.token);
    decoder->requestAbort();
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 1000);
    QVERIFY(!work.get());
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.front().failure, BandcampStreamProbeFailure::Cancelled);
    QCOMPARE(inner->initCalls, 0);
    const auto captured = capturedLogsSnapshot();
    QVERIFY(std::none_of(captured.cbegin(), captured.cend(), [](const CapturedLog& log) {
        return log.message.contains(u"stream_probe_failed"_s) || log.message.contains(u"playback_stream_open_failed"_s);
    }));
    decoder->stop();
    fixture.bridge->shutdown();
}

void PlaybackTests::streamProbeUsesFormatWithoutInheritedTrackProperties()
{
    PlaybackFixture fixture{1};
    auto inner                   = std::make_shared<InnerState>();
    inner->sampleFormat          = Fooyin::SampleFormat::S24In32;
    inner->sampleRate            = 96'000;
    inner->channels              = 1;
    inner->inspectDeviceContract = true;
    fixture.audioLoader->addDecoder(
        u"Fixture decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 457;
    source.title     = u"Mono probe target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> result;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            result    = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(result.size(), 1);
    QVERIFY(!result.front().failure);
    QCOMPARE(result.front().properties.sampleRate, 96'000);
    QCOMPARE(result.front().properties.channels, 1);
    QCOMPARE(result.front().properties.bitDepth, 24);
    QCOMPARE(result.front().properties.bitrate, 0);
    QCOMPARE(inner->inputSampleRate, 0);
    QCOMPARE(inner->inputChannels, 2);
    QCOMPARE(inner->inputBitDepth, -1);
    QCOMPARE(inner->inputBitrate, 0);
    QVERIFY(inner->sourceIsSequential);
    QVERIFY(!inner->sourceSeekSucceeded);
}

void PlaybackTests::streamProbeRetriesWithFreshRemoteSources()
{
    PlaybackFixture fixture{1};
    auto failed                 = std::make_shared<InnerState>();
    failed->accept              = false;
    failed->consumeBeforeReject = true;
    auto successful             = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Rejecting fixture decoder"_s, [failed]() { return std::make_unique<FixtureRemoteDecoder>(failed); }, 10);
    fixture.audioLoader->addDecoder(
        u"Accepting fixture decoder"_s, [successful]() { return std::make_unique<FixtureRemoteDecoder>(successful); },
        20);

    BandcampTrack source;
    source.id        = 458;
    source.title     = u"Retry probe target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> result;
    LogCapture logs{u"fy.bandcamp.tracks.debug=true\nfy.trace.bandcamp.tracks.debug=true"_s};
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            result    = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(result.size(), 1);
    QVERIFY(!result.front().failure);
    QCOMPARE(result.front().properties.sampleRate, 44'100);
    QCOMPARE(failed->initCalls, 1);
    QCOMPARE(successful->initCalls, 1);
    QCOMPARE(fixture.remoteState->created.load(std::memory_order_acquire), 2);
    const auto captured = capturedLogsSnapshot();
    QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.trace.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=458 stream_probe_started"_s);
                           }),
             1);
    QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.trace.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=458 stream_probe_completed elapsedMs="_s)
                                   && log.message.contains(u"codec="_s) && log.message.contains(u"bitrate="_s);
                           }),
             1);
    QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=458 release_probe_completed elapsedMs="_s)
                                   && log.message.contains(u"trackCount=1"_s);
                           }),
             1);
}

void PlaybackTests::streamProbeRejectsReadLimitAfterValidFormat()
{
    PlaybackFixture fixture{1};
    auto inner             = std::make_shared<InnerState>();
    inner->readToReadLimit = true;
    fixture.audioLoader->addDecoder(
        u"Success-shaped limit decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 462;
    source.title     = u"Read limit target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> results;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            results   = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(results.size(), 1);
    QVERIFY(results.front().failure);
    QCOMPARE(*results.front().failure, BandcampStreamProbeFailure::ReadLimitReached);
    QVERIFY(results.front().properties.isEmpty());
    QCOMPARE(inner->readBytes, qint64{1024 * 1024});
    const auto failures = capturedLogsSnapshot();
    QCOMPARE(std::count_if(failures.cbegin(), failures.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=462 stream_probe_failed"_s);
                           }),
             1);
    QVERIFY(std::any_of(failures.cbegin(), failures.cend(), [](const CapturedLog& log) {
        return log.message.startsWith(u"track=462 stream_probe_failed reason=read_limit_reached"_s);
    }));
}

void PlaybackTests::streamProbeReadLimitStopsFallback()
{
    PlaybackFixture fixture{1};
    auto limited             = std::make_shared<InnerState>();
    limited->readToReadLimit = true;
    limited->accept          = false;
    auto later               = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Limited rejecting decoder"_s, [limited]() { return std::make_unique<FixtureRemoteDecoder>(limited); }, 10);
    fixture.audioLoader->addDecoder(
        u"Later successful decoder"_s, [later]() { return std::make_unique<FixtureRemoteDecoder>(later); }, 20);

    BandcampTrack source;
    source.id        = 463;
    source.title     = u"Fallback limit target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> results;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            results   = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(results.size(), 1);
    QVERIFY(results.front().failure);
    QCOMPARE(*results.front().failure, BandcampStreamProbeFailure::ReadLimitReached);
    QVERIFY(results.front().properties.isEmpty());
    QCOMPARE(limited->readBytes, qint64{1024 * 1024});
    QCOMPARE(later->initCalls, 0);
    QCOMPARE(fixture.remoteState->created.load(std::memory_order_acquire), 1);
}

void PlaybackTests::streamProbePreservesWithinBudgetSuccess()
{
    PlaybackFixture fixture{1};
    auto inner              = std::make_shared<InnerState>();
    inner->readExactlyLimit = true;
    fixture.audioLoader->addDecoder(
        u"Within-budget decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 464;
    source.title     = u"Within-budget target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> results;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            results   = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(results.size(), 1);
    QVERIFY(!results.front().failure);
    QCOMPARE(results.front().properties.sampleRate, 44'100);
    QCOMPARE(inner->readBytes, qint64{1024 * 1024});
}

void PlaybackTests::streamProbeReportsTimeoutAndCapacityOutcomes()
{
    PlaybackFixture timeoutFixture{2};
    auto timeoutDecoder = std::make_shared<InnerState>();
    timeoutFixture.audioLoader->addDecoder(
        u"Timeout fixture decoder"_s,
        [timeoutDecoder]() { return std::make_unique<FixtureRemoteDecoder>(timeoutDecoder); }, 10);
    BandcampTrack timeoutTrack;
    timeoutTrack.id        = 459;
    timeoutTrack.title     = u"Timeout probe target"_s;
    timeoutTrack.artist    = u"Probe artist"_s;
    timeoutTrack.sourceUrl = AlbumUrl;
    timeoutTrack.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    BandcampStreamProbe timeoutProbe{timeoutFixture.audioLoader, timeoutFixture.remoteIo.get()};
    QObject timeoutContext;
    bool timeoutCompleted = false;
    QList<BandcampStreamProbeResult> timeoutResults;
    const auto timeoutSubmission = timeoutProbe.probe({BandcampResolvedTrack{timeoutTrack, std::nullopt}},
                                                      &timeoutContext, [&](QList<BandcampStreamProbeResult> values) {
                                                          timeoutResults   = std::move(values);
                                                          timeoutCompleted = true;
                                                      });
    QVERIFY(timeoutSubmission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(timeoutCompleted, 5000);
    QCOMPARE(timeoutResults.size(), 1);
    QVERIFY(timeoutResults.front().failure);
    QCOMPARE(*timeoutResults.front().failure, BandcampStreamProbeFailure::TimedOut);

    PlaybackFixture capacityFixture{1};
    auto capacityDecoder = std::make_shared<InnerState>();
    capacityFixture.audioLoader->addDecoder(
        u"Capacity fixture decoder"_s,
        [capacityDecoder]() { return std::make_unique<FixtureRemoteDecoder>(capacityDecoder); }, 10);
    BandcampStreamProbe capacityProbe{capacityFixture.audioLoader, capacityFixture.remoteIo.get()};
    QObject capacityContext;
    const auto completion = [](QList<BandcampStreamProbeResult>) {
    };
    BandcampTrack firstTrack  = timeoutTrack;
    firstTrack.id             = 460;
    BandcampTrack secondTrack = timeoutTrack;
    secondTrack.id            = 461;
    const auto firstSubmission
        = capacityProbe.probe({BandcampResolvedTrack{firstTrack, std::nullopt}}, &capacityContext, completion);
    const auto secondSubmission
        = capacityProbe.probe({BandcampResolvedTrack{secondTrack, std::nullopt}}, &capacityContext, completion);
    QVERIFY(firstSubmission.token > 0);
    QVERIFY(secondSubmission.token > 0);

    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    const auto rejected
        = capacityProbe.probe({BandcampResolvedTrack{timeoutTrack, std::nullopt}}, &capacityContext, completion);
    QCOMPARE(rejected.token, quint64{0});
    QVERIFY(rejected.rejection);
    QCOMPARE(*rejected.rejection, BandcampStreamProbeFailure::CapacityRejected);
    const auto capacityLogs = capturedLogsSnapshot();
    QCOMPARE(std::count_if(capacityLogs.cbegin(), capacityLogs.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=459 stream_probe_failed"_s);
                           }),
             1);
    QVERIFY(std::any_of(capacityLogs.cbegin(), capacityLogs.cend(), [](const CapturedLog& log) {
        return log.message.startsWith(u"track=459 stream_probe_failed reason=capacity_rejected"_s);
    }));
    capacityProbe.shutdown();
}

void PlaybackTests::streamProbeFailureDiagnosticsSurviveDestroyedContext()
{
    PlaybackFixture fixture{1};
    auto inner    = std::make_shared<InnerState>();
    inner->accept = false;
    fixture.audioLoader->addDecoder(
        u"Rejecting fixture decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 465;
    source.title     = u"Destroyed callback target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    auto callbackContext  = std::make_unique<QObject>();
    bool callbackCalled   = false;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, callbackContext.get(),
                                        [&](QList<BandcampStreamProbeResult>) { callbackCalled = true; });
    QVERIFY(submission.token > 0);
    callbackContext.reset();

    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            const auto snapshot = capturedLogsSnapshot();
            return std::any_of(snapshot.cbegin(), snapshot.cend(), [](const CapturedLog& log) {
                return log.message.startsWith(u"track=465 stream_probe_failed reason=decoder_exhausted"_s);
            });
        }(),
        5000);
    const auto captured = capturedLogsSnapshot();
    QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=465 stream_probe_failed"_s);
                           }),
             1);
    QVERIFY(!callbackCalled);
}

void PlaybackTests::streamProbeCancellationIsSilent()
{
    PlaybackFixture fixture{1};
    auto inner                 = std::make_shared<InnerState>();
    inner->blockInitialization = true;
    fixture.audioLoader->addDecoder(
        u"Cancellable fixture decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 466;
    source.title     = u"Cancelled probe target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    LogCapture logs{u"fy.bandcamp.tracks.debug=true\nfy.trace.bandcamp.tracks.debug=true"_s};
    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> results;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            results   = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(inner->initEntered.load(std::memory_order_acquire), 5000);
    probe.cancel(submission.token);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(results.size(), 1);
    QVERIFY(results.front().failure);
    QCOMPARE(*results.front().failure, BandcampStreamProbeFailure::Cancelled);
    const auto captured = capturedLogsSnapshot();
    QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.trace.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=466 stream_probe_started"_s);
                           }),
             1);
    QVERIFY(std::none_of(captured.cbegin(), captured.cend(), [](const CapturedLog& log) {
        return log.message.startsWith(u"track=466 stream_probe_failed"_s)
            || (log.category == u"fy.trace.bandcamp.tracks"_s
                && log.message.startsWith(u"track=466 stream_probe_completed"_s));
    }));
}

void PlaybackTests::streamProbeLogsRecoveredDecoderExceptions()
{
    PlaybackFixture fixture{1};
    auto throwing         = std::make_shared<InnerState>();
    throwing->throwOnInit = true;
    auto successful       = std::make_shared<InnerState>();
    fixture.audioLoader->addDecoder(
        u"Throwing fixture decoder"_s, [throwing]() { return std::make_unique<FixtureRemoteDecoder>(throwing); }, 10);
    fixture.audioLoader->addDecoder(
        u"Successful fixture decoder"_s, [successful]() { return std::make_unique<FixtureRemoteDecoder>(successful); },
        20);

    BandcampTrack source;
    source.id        = 467;
    source.title     = u"Recovered exception target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);

    LogCapture logs{u"fy.bandcamp.tracks.warning=true"_s};
    BandcampStreamProbe probe{fixture.audioLoader, fixture.remoteIo.get()};
    QObject callbackContext;
    bool completed = false;
    QList<BandcampStreamProbeResult> results;
    const auto submission = probe.probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                        [&](QList<BandcampStreamProbeResult> values) {
                                            results   = std::move(values);
                                            completed = true;
                                        });
    QVERIFY(submission.token > 0);
    QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
    QCOMPARE(results.size(), 1);
    QVERIFY(!results.front().failure);
    QCOMPARE(throwing->initCalls, 1);
    QCOMPARE(successful->initCalls, 1);
    const auto captured = capturedLogsSnapshot();
    QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                           [](const CapturedLog& log) {
                               return log.category == u"fy.bandcamp.tracks"_s
                                   && log.message.startsWith(u"track=467 stream_probe_decoder_exception_recovered"_s);
                           }),
             1);
    QVERIFY(std::none_of(captured.cbegin(), captured.cend(), [](const CapturedLog& log) {
        return log.message.contains(u"signature=private"_s)
            || log.message.contains(u"private decoder initialization error"_s);
    }));
}

void PlaybackTests::probeShutdownDetachesWorkerWithoutCallback()
{
    PlaybackFixture fixture{1};
    auto inner                 = std::make_shared<InnerState>();
    inner->blockInitialization = true;
    fixture.audioLoader->addDecoder(
        u"Fixture decoder"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);

    BandcampTrack source;
    source.id        = 456;
    source.title     = u"Probe target"_s;
    source.artist    = u"Probe artist"_s;
    source.sourceUrl = AlbumUrl;
    source.streamUrl = playbackStream(QDateTime::currentMSecsSinceEpoch() + 60'000);
    auto probe       = std::make_unique<BandcampStreamProbe>(fixture.audioLoader, fixture.remoteIo.get());
    QObject callbackContext;
    bool callbackCalled   = false;
    const auto submission = probe->probe({BandcampResolvedTrack{source, std::nullopt}}, &callbackContext,
                                         [&](QList<BandcampStreamProbeResult>) { callbackCalled = true; });
    QVERIFY(submission.token > 0);

    QTRY_VERIFY_WITH_TIMEOUT(inner->initEntered.load(std::memory_order_acquire), 5000);
    probe->shutdown();
    probe.reset();
    QTRY_VERIFY_WITH_TIMEOUT(inner->initAbortObserved.load(std::memory_order_acquire), 5000);
    QVERIFY(!callbackCalled);
}

void PlaybackTests::playbackPropertiesPreserveTrackAndPersistByBandcampIdentity_data()
{
    QTest::addColumn<QString>("codec");
    QTest::addColumn<QString>("encoding");
    QTest::addColumn<Fooyin::SampleFormat>("sampleFormat");
    QTest::addColumn<int>("bitDepth");
    QTest::newRow("mp3-float-pcm") << u"MP3"_s << u"Lossy"_s << Fooyin::SampleFormat::F32 << -1;
    QTest::newRow("flac-24-bit") << u"FLAC"_s << u"Lossless"_s << Fooyin::SampleFormat::S24In32 << 24;
}

void PlaybackTests::playbackPropertiesPreserveTrackAndPersistByBandcampIdentity()
{
    QFETCH(QString, codec);
    QFETCH(QString, encoding);
    QFETCH(Fooyin::SampleFormat, sampleFormat);
    QFETCH(int, bitDepth);
    PlaybackLibrary host;
    const qint64 expiry    = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    Fooyin::Track resident = playbackTrack(expiry);
    resident.setId(812);
    resident.setBitDepth(32);
    host.library = {resident};
    BandcampLibraryGateway persistenceGateway{host.api};
    PlaybackFixture fixture{1, host.api, &persistenceGateway, &host.owner};

    auto inner                 = std::make_shared<InnerState>();
    inner->reportsChangedTrack = true;
    inner->sampleFormat        = sampleFormat;
    inner->changedTrack.setTitle(u"Decoder-owned title must not escape"_s);
    inner->changedTrack.setCodec(codec);
    inner->changedTrack.setEncoding(encoding);
    inner->changedTrack.setBitDepth(Fooyin::AudioFormat{sampleFormat, inner->sampleRate, inner->channels}.bitdepth());
    inner->changedTrack.setBitrate(256);
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    auto candidates = fixture.audioLoader->decodersForTrack(resident);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work                     = std::async(std::launch::async, [decoder, resident]() {
        Fooyin::AudioSource source;
        source.filepath   = resident.filepath();
        const auto format = decoder->init(source, resident, Fooyin::AudioDecoder::None);
        return format.has_value() ? std::optional{decoder->changedTrack()} : std::nullopt;
    });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    const auto changed = work.get();
    QVERIFY(changed);
    QCOMPARE(changed->filepath(), resident.filepath());
    QCOMPARE(changed->title(), u"User-edited title"_s);
    QCOMPARE(changed->extraTag(u"MOOD"_s), QStringList{u"User mood"_s});
    QCOMPARE(changed->codec(), codec);
    QCOMPARE(changed->encoding(), encoding);
    QCOMPARE(changed->sampleRate(), 44'100);
    QCOMPARE(changed->channels(), 2);
    QCOMPARE(changed->bitDepth(), bitDepth);
    QCOMPARE(changed->bitrate(), 256);
    QCOMPARE(inner->options, Fooyin::AudioDecoder::UpdateTracks);
    Fooyin::ScriptRegistry registry;
    registry.finalise();
    Fooyin::ScriptParser scripting{&registry};
    const QString displayedDepth = bitDepth > 0 ? QString::number(bitDepth) : QString{};
    QCOMPARE(changed->techInfo(u"BITDEPTH"_s), displayedDepth);
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, *changed), displayedDepth);

    QTRY_COMPARE(host.writes.size(), std::size_t{1});
    const Fooyin::Track persisted = host.writes.front().front();
    QCOMPARE(persisted.id(), 812);
    QCOMPARE(parseBandcampTrackUri(persisted.filepath()), std::optional<qint64>{456});
    QCOMPARE(persisted.title(), u"User-edited title"_s);
    QCOMPARE(persisted.extraProperties().value(BandcampStreamUrlProperty),
             resident.extraProperties().value(BandcampStreamUrlProperty));
    QCOMPARE(persisted.extraProperties().value(BandcampSourceUrlProperty),
             resident.extraProperties().value(BandcampSourceUrlProperty));
    QCOMPARE(persisted.extraProperties().value(BandcampExpiresAtProperty),
             resident.extraProperties().value(BandcampExpiresAtProperty));
    QCOMPARE(persisted.extraProperties().value(StreamProviderProperty),
             resident.extraProperties().value(StreamProviderProperty));
    QCOMPARE(persisted.codec(), codec);
    QCOMPARE(persisted.encoding(), encoding);
    QCOMPARE(persisted.bitDepth(), bitDepth);
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, persisted), displayedDepth);
    QCOMPARE(persisted.sampleRate(), 44'100);
    host.finishWrite(true);
    QTRY_COMPARE(host.writes.size(), std::size_t{1});
}

void PlaybackTests::runtimePlaybackPropertiesAreSurfacedAndPersisted()
{
    PlaybackLibrary host;
    const qint64 expiry    = (QDateTime::currentMSecsSinceEpoch() / 1000 + 30 * 24 * 60 * 60) * 1000;
    Fooyin::Track resident = playbackTrack(expiry);
    resident.setId(813);
    resident.setSampleRate(44'100);
    resident.setChannels(2);
    resident.setBitDepth(32);
    resident.clearWasModified();
    host.library = {resident};
    BandcampLibraryGateway persistenceGateway{host.api};
    PlaybackFixture fixture{1, host.api, &persistenceGateway, &host.owner};

    auto inner          = std::make_shared<InnerState>();
    inner->sampleFormat = Fooyin::SampleFormat::F32;
    fixture.audioLoader->addDecoder(
        u"Fixture MP3"_s, [inner]() { return std::make_unique<FixtureRemoteDecoder>(inner); }, 10);
    const auto input = bandcampInputCreator(fixture.bridge);
    fixture.audioLoader->addDecoder(u"Bandcamp"_s, input.decoder, input.priority);

    auto candidates = fixture.audioLoader->decodersForTrack(resident);
    QCOMPARE(candidates.size(), std::size_t{1});
    Fooyin::AudioDecoder* decoder = candidates.front().get();
    auto work                     = std::async(std::launch::async, [decoder, resident]() {
        Fooyin::AudioSource source;
        source.filepath = resident.filepath();
        return decoder->init(source, resident, Fooyin::AudioDecoder::None);
    });
    QTRY_VERIFY_WITH_TIMEOUT(work.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready, 5000);
    QVERIFY(work.get().has_value());
    QCOMPARE(host.writes.size(), std::size_t{0});
    QCOMPARE(inner->inputSampleRate, 0);
    QCOMPARE(inner->inputChannels, 2);
    QCOMPARE(inner->inputBitDepth, -1);
    QCOMPARE(inner->inputBitrate, 0);

    inner->changedTrack.setTitle(u"Decoder title must not escape"_s);
    inner->changedTrack.setCodec(u"MP3"_s);
    inner->changedTrack.setEncoding(u"Lossy"_s);
    inner->changedTrack.setSampleRate(22'050);
    inner->changedTrack.setChannels(2);
    inner->changedTrack.setBitDepth(32);
    inner->changedTrack.setBitrate(256);
    inner->reportsChangedTrack = true;

    QVERIFY(decoder->trackHasChanged());
    const Fooyin::Track changed = decoder->changedTrack();
    QCOMPARE(changed.filepath(), resident.filepath());
    QCOMPARE(changed.title(), u"User-edited title"_s);
    QCOMPARE(changed.extraTag(u"MOOD"_s), QStringList{u"User mood"_s});
    QCOMPARE(changed.codec(), u"MP3"_s);
    QCOMPARE(changed.encoding(), u"Lossy"_s);
    QCOMPARE(changed.sampleRate(), 44'100);
    QCOMPARE(changed.channels(), 2);
    QCOMPARE(changed.bitDepth(), -1);
    QCOMPARE(changed.bitrate(), 256);
    Fooyin::ScriptRegistry registry;
    registry.finalise();
    Fooyin::ScriptParser scripting{&registry};
    QCOMPARE(changed.techInfo(u"BITDEPTH"_s), QString{});
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, changed), QString{});

    QTRY_COMPARE(host.writes.size(), std::size_t{1});
    const Fooyin::Track persisted = host.writes.front().front();
    QCOMPARE(persisted.id(), 813);
    QCOMPARE(parseBandcampTrackUri(persisted.filepath()), std::optional<qint64>{456});
    QCOMPARE(persisted.title(), u"User-edited title"_s);
    QCOMPARE(persisted.extraProperties().value(BandcampStreamUrlProperty),
             resident.extraProperties().value(BandcampStreamUrlProperty));
    QCOMPARE(persisted.codec(), u"MP3"_s);
    QCOMPARE(persisted.encoding(), u"Lossy"_s);
    QCOMPARE(persisted.sampleRate(), 44'100);
    QCOMPARE(persisted.channels(), 2);
    QCOMPARE(persisted.bitDepth(), -1);
    QCOMPARE(persisted.bitrate(), 256);
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, persisted), QString{});
    host.library = {persisted};
    host.finishWrite(true);
    QTRY_COMPARE(host.writes.size(), std::size_t{1});

    inner->changedTrack = Fooyin::Track{};
    inner->changedTrack.setBitDepth(32);
    inner->changedTrack.setBitrate(128);
    QVERIFY(decoder->trackHasChanged());
    const Fooyin::Track partial = decoder->changedTrack();
    QCOMPARE(partial.encoding(), u"Lossy"_s);
    QCOMPARE(partial.bitDepth(), -1);
    QCOMPARE(scripting.evaluate(u"[%bitdepth%]"_s, partial), QString{});
    QTRY_COMPARE(host.writes.size(), std::size_t{2});
    QCOMPARE(host.writes.back().front().bitDepth(), -1);
    QCOMPARE(host.writes.back().front().encoding(), u"Lossy"_s);
    QCOMPARE(host.writes.back().front().bitrate(), 128);
    host.finishWrite(true);
}

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
    QVERIFY(fixture.remoteState->bufferChecks.load() >= 2);
    QCOMPARE(fixture.remoteState->readsBeforeReadiness.load(), 0);
    QCOMPARE(fixture.remoteState->readsWithoutCancellationToken.load(), 0);
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
