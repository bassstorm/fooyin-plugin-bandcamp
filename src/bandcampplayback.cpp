#include "bandcampplayback.h"

#include "bandcamplogging.h"
#include "bandcampplaybackpreparation.h"
#include "bandcampremotestream.h"
#include "bandcampsettings.h"
#include "bandcamptrackuri.h"

#include <core/coresettings.h>
#include <core/engine/audioloader.h>
#include <core/network/remoteioservice.h>
#include <core/network/remotestreamdevice.h>
#include <utils/settings/settingsmanager.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QIODevice>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
// Playback is waiting on a user action, so each page gets a single attempt. The user's attempt timeout still applies,
// but no retry follows, so cooldowns never run.
constexpr int PlaybackAttemptsPerPage = 1;
constexpr auto ReopenDelay            = std::chrono::milliseconds{100};
// Stream open and decoder init after the stream URL is known.
constexpr int DecoderInitTimeoutMs = 10'000;

// The track page and its parent album page are fetched at most once each, before the stream URL is known.
// The library write runs after playback is prepared, so it is not part of this budget.
constexpr int playbackWaiterBudgetMs(int attemptTimeoutMs)
{
    return 2 * attemptTimeoutMs + DecoderInitTimeoutMs;
}

// Covers the window before the host thread arms the waiter with the user-derived deadline.
constexpr auto PreArmWaiterBudget = std::chrono::milliseconds{playbackWaiterBudgetMs(MaxAttemptTimeoutMs)};

struct PlaybackWaiter
{
    std::mutex mutex;
    std::condition_variable changed;
    bool completed{false};
    bool cancelled{false};
    std::optional<BandcampPlaybackBridge::PreparedTransport> transport;
    QString failure;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + PreArmWaiterBudget;

    QPointer<BandcampPlaybackPreparation> operation;
};

void completeWaiter(const std::shared_ptr<PlaybackWaiter>& waiter, QString failure)
{
    {
        const std::scoped_lock lock{waiter->mutex};
        if(waiter->completed || waiter->transport) {
            return;
        }
        waiter->failure   = std::move(failure);
        waiter->completed = true;
    }
    waiter->changed.notify_all();
}

void playbackDiagnostic(qint64 trackId, const QString& event, const QString& reason)
{
    const CorrelationContext context{.trackId = trackId};
    qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(context, event,
                                                             {DiagnosticField::text(QStringLiteral("reason"), reason)});
}

} // namespace

struct BandcampPlaybackBridge::State
{
    QPointer<BandcampResolutionService> service;
    std::shared_ptr<Fooyin::AudioLoader> audioLoader;
    QPointer<Fooyin::RemoteIoService> remoteIo;
    std::mutex remoteIoMutex;
    QPointer<Fooyin::SettingsManager> settings;
    FooyinLibraryApi libraryApi;
    QPointer<BandcampLibraryGateway> persistenceGateway;
    QPointer<QObject> persistenceContext;
    std::mutex persistenceMutex;
    std::atomic_bool shuttingDown{false};
    std::stop_source shutdownSource;
    std::mutex waitersMutex;
    std::vector<std::weak_ptr<PlaybackWaiter>> waiters;
    std::mutex operationsMutex;
    std::vector<QPointer<BandcampPlaybackPreparation>> operations;

    void removeWaiter(const std::shared_ptr<PlaybackWaiter>& waiter)
    {
        const std::scoped_lock lock{waitersMutex};
        std::erase_if(waiters, [&waiter](const auto& candidate) {
            const auto current = candidate.lock();
            return !current || current == waiter;
        });
    }

    void removeOperation(BandcampPlaybackPreparation* operation)
    {
        const std::scoped_lock lock{operationsMutex};
        std::erase_if(operations,
                      [operation](const auto& current) { return current.isNull() || current == operation; });
    }
};

namespace {
void beginPreparation(const std::shared_ptr<BandcampPlaybackBridge::State>& state,
                      const std::shared_ptr<PlaybackWaiter>& waiter, const Fooyin::Track& track)
{
    if(state->shuttingDown.load(std::memory_order_acquire)) {
        completeWaiter(waiter, QStringLiteral("Playback preparation was withdrawn."));
        return;
    }
    {
        const std::scoped_lock lock{waiter->mutex};
        if(waiter->cancelled || waiter->completed) {
            waiter->completed = true;
            waiter->failure   = QStringLiteral("Playback preparation was cancelled.");
            waiter->changed.notify_all();
            return;
        }
    }

    if(!state->service || !state->settings || !state->audioLoader) {
        completeWaiter(waiter, QStringLiteral("Playback services are unavailable."));
        return;
    }

    FySettings settings;
    const auto loaded        = loadBandcampSettings(settings);
    OperationOptions options = makeOperationOptions(loaded);
    options.maxAttempts      = PlaybackAttemptsPerPage;
    const auto deadline      = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds{playbackWaiterBudgetMs(options.attemptTimeoutMs)};
    {
        const std::scoped_lock lock{waiter->mutex};
        if(state->shuttingDown.load(std::memory_order_acquire)) {
            waiter->completed = true;
            waiter->failure   = QStringLiteral("Playback preparation was withdrawn.");
            waiter->changed.notify_all();
            return;
        }
        if(waiter->cancelled || waiter->completed) {
            waiter->completed = true;
            waiter->failure   = QStringLiteral("Playback preparation was cancelled.");
            waiter->changed.notify_all();
            return;
        }
        waiter->deadline = deadline;
    }
    waiter->changed.notify_all();

    auto* operation = new BandcampPlaybackPreparation{
        state->service, track, options, state->libraryApi, state->service, state->persistenceGateway.data()};
    {
        const std::scoped_lock lock{state->operationsMutex, waiter->mutex};
        if(state->shuttingDown.load(std::memory_order_acquire) || waiter->cancelled || waiter->completed) {
            delete operation;
            if(!waiter->completed) {
                waiter->completed = true;
                waiter->failure   = state->shuttingDown.load(std::memory_order_acquire)
                                      ? QStringLiteral("Playback preparation was withdrawn.")
                                      : QStringLiteral("Playback preparation was cancelled.");
            }
            waiter->changed.notify_all();
            return;
        }
        waiter->operation = operation;
        state->operations.emplace_back(operation);
    }
    const std::weak_ptr weakState{state};
    QObject::connect(operation, &BandcampPlaybackPreparation::prepared, operation,
                     [waiter, deadline](const QString& streamUrl) {
                         {
                             const std::scoped_lock lock{waiter->mutex};
                             if(waiter->cancelled || waiter->completed) {
                                 return;
                             }
                             waiter->transport = BandcampPlaybackBridge::PreparedTransport{streamUrl, deadline};
                         }
                         waiter->changed.notify_all();
                     });
    QObject::connect(operation, &BandcampPlaybackPreparation::failed, operation,
                     [waiter](const QString& reason) { completeWaiter(waiter, reason); });
    QObject::connect(operation, &BandcampPlaybackPreparation::finished, operation, [weakState, waiter, operation]() {
        {
            const std::scoped_lock lock{waiter->mutex};
            if(waiter->operation == operation) {
                waiter->operation.clear();
            }
        }
        if(const auto locked = weakState.lock()) {
            locked->removeOperation(operation);
        }
        operation->deleteLater();
    });
    operation->start(QDateTime::currentMSecsSinceEpoch());
}

void cancelPreparation(const std::weak_ptr<BandcampPlaybackBridge::State>& weakState,
                       const std::shared_ptr<PlaybackWaiter>& waiter)
{
    const auto application = QCoreApplication::instance();
    if(!application) {
        return;
    }
    QMetaObject::invokeMethod(
        application,
        [weakState, waiter]() {
            if(const auto state = weakState.lock()) {
                QPointer<BandcampPlaybackPreparation> operation;
                {
                    const std::scoped_lock lock{waiter->mutex};
                    operation = waiter->operation;
                }
                if(operation) {
                    operation->cancel();
                }
            }
        },
        Qt::QueuedConnection);
}
} // namespace

BandcampPlaybackBridge::BandcampPlaybackBridge(BandcampResolutionService* service,
                                               std::shared_ptr<Fooyin::AudioLoader> audioLoader,
                                               Fooyin::RemoteIoService* remoteIo, Fooyin::SettingsManager* settings,
                                               FooyinLibraryApi libraryApi, BandcampLibraryGateway* persistenceGateway,
                                               QObject* persistenceContext)
    : m_state{std::make_shared<State>()}
{
    m_state->service            = service;
    m_state->audioLoader        = std::move(audioLoader);
    m_state->remoteIo           = remoteIo;
    m_state->settings           = settings;
    m_state->libraryApi         = std::move(libraryApi);
    m_state->persistenceGateway = persistenceGateway;
    m_state->persistenceContext = persistenceContext;
}

BandcampPlaybackBridge::~BandcampPlaybackBridge() = default;

std::shared_ptr<Fooyin::AudioLoader> BandcampPlaybackBridge::audioLoader() const
{
    return m_state->audioLoader;
}

bool BandcampPlaybackBridge::createRemoteStreamSource(const QUrl& url, Fooyin::RemoteStreamSource& source) const
{
    const auto state = m_state;
    const std::scoped_lock lock{state->remoteIoMutex};
    if(state->shuttingDown.load(std::memory_order_acquire) || !state->remoteIo) {
        return false;
    }
    source = state->remoteIo->createStreamSource(url);
    return source.device && source.remoteDevice;
}

std::stop_token BandcampPlaybackBridge::shutdownToken() const
{
    return m_state->shutdownSource.get_token();
}

BandcampPlaybackBridge::Preparation BandcampPlaybackBridge::prepare(const Fooyin::Track& track,
                                                                    std::stop_token cancellation) const
{
    const auto state       = m_state;
    const auto application = QCoreApplication::instance();
    if(!application || QThread::currentThread() == application->thread()) {
        return Failure{QStringLiteral("Playback preparation requires a worker thread.")};
    }
    if(cancellation.stop_requested()) {
        return Failure{QStringLiteral("Playback preparation was cancelled.")};
    }

    auto waiter = std::make_shared<PlaybackWaiter>();
    {
        const std::scoped_lock lock{state->waitersMutex};
        if(state->shuttingDown.load(std::memory_order_acquire)) {
            return Failure{QStringLiteral("Playback preparation is shutting down.")};
        }
        state->waiters.emplace_back(waiter);
    }

    const std::weak_ptr weakState{state};
    if(!application
       || !QMetaObject::invokeMethod(
           application,
           [weakState, waiter, track]() {
               if(const auto locked = weakState.lock()) {
                   beginPreparation(locked, waiter, track);
               }
               else {
                   completeWaiter(waiter, QStringLiteral("Playback preparation was withdrawn."));
               }
           },
           Qt::QueuedConnection)) {
        state->removeWaiter(waiter);
        return Failure{QStringLiteral("The host event loop is unavailable.")};
    }

    std::stop_callback cancellationWake{cancellation, [waiter]() {
                                            {
                                                const std::scoped_lock lock{waiter->mutex};
                                                waiter->cancelled = true;
                                                waiter->completed = true;
                                                waiter->failure = QStringLiteral("Playback preparation was cancelled.");
                                            }
                                            waiter->changed.notify_all();
                                        }};
    std::unique_lock lock{waiter->mutex};
    auto withdraw = [&](QString reason) {
        waiter->cancelled = true;
        waiter->completed = true;
        waiter->failure   = reason;
        lock.unlock();
        waiter->changed.notify_all();
        cancelPreparation(weakState, waiter);
        state->removeWaiter(waiter);
        return Preparation{Failure{std::move(reason)}};
    };

    while(!waiter->transport && !waiter->completed && !waiter->cancelled) {
        const auto deadline = waiter->deadline;
        if(waiter->changed.wait_until(lock, deadline) == std::cv_status::timeout
           && std::chrono::steady_clock::now() >= waiter->deadline) {
            return withdraw(QStringLiteral("Playback preparation exceeded its deadline."));
        }
    }
    if(cancellation.stop_requested() || waiter->cancelled) {
        return withdraw(QStringLiteral("Playback preparation was cancelled."));
    }
    if(waiter->transport) {
        auto result = *waiter->transport;
        lock.unlock();
        state->removeWaiter(waiter);
        return result;
    }
    const QString reason
        = waiter->failure.isEmpty() ? QStringLiteral("Bandcamp could not prepare a playable stream.") : waiter->failure;
    lock.unlock();
    state->removeWaiter(waiter);
    return Failure{reason};
}

void BandcampPlaybackBridge::persistStreamProperties(const Fooyin::Track& track,
                                                     const BandcampStreamProperties& properties) const
{
    const auto state = m_state;
    if(state->shuttingDown.load(std::memory_order_acquire) || properties.isEmpty()) {
        return;
    }
    const auto id = parseBandcampTrackUri(track.filepath());
    if(!id) {
        return;
    }
    const CorrelationContext correlation{.trackId = *id};
    const std::scoped_lock lock{state->persistenceMutex};
    if(state->shuttingDown.load(std::memory_order_acquire) || !state->persistenceGateway
       || !state->persistenceContext) {
        return;
    }
    const QPointer<BandcampLibraryGateway> gateway{state->persistenceGateway};
    const QPointer<QObject> context{state->persistenceContext};
    QMetaObject::invokeMethod(
        context,
        [gateway, context, id = *id, properties, correlation]() mutable {
            if(gateway && context) {
                gateway->updateStreamProperties(id, properties, context, correlation, [](FooyinTrackCommitOutcome) { });
            }
        },
        Qt::QueuedConnection);
}

void BandcampPlaybackBridge::shutdown()
{
    const auto state = m_state;
    state->shutdownSource.request_stop();
    {
        const std::scoped_lock lock{state->waitersMutex};
        state->shuttingDown.store(true, std::memory_order_release);
        for(const auto& weakWaiter : state->waiters) {
            if(const auto waiter = weakWaiter.lock()) {
                {
                    const std::scoped_lock waiterLock{waiter->mutex};
                    waiter->cancelled = true;
                    if(!waiter->transport) {
                        waiter->completed = true;
                        waiter->failure   = QStringLiteral("Playback preparation was withdrawn.");
                    }
                }
                waiter->changed.notify_all();
            }
        }
    }
    {
        const std::scoped_lock lock{state->remoteIoMutex};
        state->remoteIo.clear();
    }
    {
        const std::scoped_lock lock{state->persistenceMutex};
        state->persistenceGateway.clear();
        state->persistenceContext.clear();
    }
    auto cancelOperations = [state]() {
        std::vector<QPointer<BandcampPlaybackPreparation>> operations;
        {
            const std::scoped_lock lock{state->operationsMutex};
            operations = std::exchange(state->operations, {});
        }
        for(const QPointer<BandcampPlaybackPreparation>& operation : operations) {
            if(operation) {
                operation->cancel();
            }
        }
    };
    if(const auto application = QCoreApplication::instance();
       application && QThread::currentThread() != application->thread()) {
        QMetaObject::invokeMethod(application, std::move(cancelOperations), Qt::QueuedConnection);
    }
    else {
        cancelOperations();
    }
}

namespace {
class BandcampAudioDecoder final : public Fooyin::AudioDecoder
{
public:
    explicit BandcampAudioDecoder(std::shared_ptr<BandcampPlaybackBridge> bridge)
        : m_bridge{std::move(bridge)}
    { }

    ~BandcampAudioDecoder() override
    {
        stop();
    }

    [[nodiscard]] QStringList extensions() const override
    {
        return {};
    }

    [[nodiscard]] QStringList supportedSchemes() const override
    {
        return {u"bandcamp"_s};
    }

    [[nodiscard]] bool isSeekable() const override
    {
        if(!m_device || m_device->isSequential()) {
            return false;
        }
        const auto inner = innerDecoder();
        if(!inner) {
            return false;
        }
        try {
            return inner->isSeekable();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_seekability_failed"));
            return false;
        }
    }

    [[nodiscard]] bool allowsConcurrentDecoding() const override
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return true;
        }
        try {
            return inner->allowsConcurrentDecoding();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_decoder_policy_failed"));
            return false;
        }
    }

    [[nodiscard]] int playbackPrebufferMs() const override
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return 0;
        }
        try {
            return inner->playbackPrebufferMs();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_prebuffer_failed"));
            return 0;
        }
    }

    [[nodiscard]] bool needsMoreInput() const override
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return false;
        }
        try {
            return inner->needsMoreInput();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_input_state_failed"));
            return false;
        }
    }

    [[nodiscard]] QStringList takeWarnings() override
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return {};
        }
        try {
            return inner->takeWarnings();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_decoder_warning_failed"));
            return {};
        }
    }

    [[nodiscard]] RepeatHandling repeatHandling() const override
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return RepeatHandling::EngineTransition;
        }
        try {
            return inner->repeatHandling();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_repeat_policy_failed"));
            return RepeatHandling::EngineTransition;
        }
    }

    [[nodiscard]] int bitrate() const override
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return 0;
        }
        try {
            return inner->bitrate();
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_bitrate_failed"));
            return 0;
        }
    }

    std::optional<Fooyin::AudioFormat> init(const Fooyin::AudioSource& source, const Fooyin::Track& track,
                                            DecoderOptions options) override
    {
        stop();
        const auto id = parseBandcampTrackUri(track.filepath());
        if(!id || source.filepath != track.filepath()) {
            playbackDiagnostic(id.value_or(0), QStringLiteral("playback_identity_failed"),
                               QStringLiteral("The virtual track identity is invalid."));
            return {};
        }
        m_trackId.store(*id, std::memory_order_release);
        {
            const std::scoped_lock lock{m_changedTrackMutex};
            m_sourceTrack     = track;
            m_changedTrack    = {};
            m_hasChangedTrack = false;
            m_streamFormat.reset();
        }
        m_delegateFailureLogged.store(false, std::memory_order_release);
        m_delegateFault.store(false, std::memory_order_release);
        if(!m_bridge) {
            playbackDiagnostic(*id, QStringLiteral("playback_prepare_failed"),
                               QStringLiteral("The playback bridge is unavailable."));
            return {};
        }

        const auto shutdownToken = m_bridge->shutdownToken();
        std::stop_callback shutdownWake{shutdownToken, [this]() { requestAbort(); }};
        if(shutdownToken.stop_requested()) {
            playbackDiagnostic(*id, QStringLiteral("playback_prepare_failed"),
                               QStringLiteral("Playback is shutting down."));
            return {};
        }

        try {
            auto prepared = m_bridge->prepare(track, abortToken());
            if(const auto* failure = std::get_if<BandcampPlaybackBridge::Failure>(&prepared)) {
                playbackDiagnostic(*id, QStringLiteral("playback_prepare_failed"), failure->reason);
                return {};
            }
            const auto& transport = std::get<BandcampPlaybackBridge::PreparedTransport>(prepared);
            m_deadline            = transport.deadline;
            if(attemptStopped()) {
                playbackDiagnostic(*id, QStringLiteral("playback_prepare_failed"),
                                   QStringLiteral("The playback deadline expired."));
                return {};
            }

            const QString safeUrl = QStringLiteral("https://bandcamp.invalid/track.mp3");
            Fooyin::Track decoderTrack{safeUrl};
            decoderTrack.setMetadataWasRead(true);
            Fooyin::AudioSource decoderSource;
            if(!openRemoteSource(transport.streamUrl, m_deadline)) {
                if(!abortToken().stop_requested()) {
                    playbackDiagnostic(*id, QStringLiteral("playback_stream_open_failed"),
                                       QStringLiteral("The Bandcamp stream could not be opened."));
                }
                return {};
            }
            decoderSource.filepath           = safeUrl;
            decoderSource.device             = m_device.get();
            decoderSource.remoteStreamDevice = m_remoteDevice;

            auto candidates = m_bridge->audioLoader()->decodersForFile(safeUrl);
            if(candidates.empty()) {
                playbackDiagnostic(*id, QStringLiteral("playback_decoder_unavailable"),
                                   QStringLiteral("No Fooyin MP3 decoder is available."));
                stop();
                return {};
            }

            for(auto& candidate : candidates) {
                if(attemptStopped()) {
                    break;
                }
                if(!candidate) {
                    continue;
                }
                const qint64 sourcePosition = m_device->pos();
                auto inner                  = std::shared_ptr<Fooyin::AudioDecoder>{std::move(candidate)};
                setInnerDecoder(inner);
                if(attemptStopped()) {
                    stop();
                    return {};
                }
                inner->setPlaybackHints(playbackHints());
                if(attemptStopped()) {
                    stop();
                    return {};
                }

                const auto remainingOpenTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    m_deadline - std::chrono::steady_clock::now());
                if(remainingOpenTime <= std::chrono::milliseconds{0}) {
                    stop();
                    return {};
                }
                decoderSource.remoteOpenTimeout = m_hostOpenTimeout > std::chrono::milliseconds{0}
                                                    ? std::min(m_hostOpenTimeout, remainingOpenTime)
                                                    : remainingOpenTime;
                // Request source encoding even when the host did not request metadata updates.
                const auto format
                    = inner->init(decoderSource, decoderTrack, options | Fooyin::AudioDecoder::UpdateTracks);
                if(format) {
                    if(m_remoteDevice) {
                        m_remoteDevice->setReconnectOnFinishedEnabled(false);
                    }
                    if(attemptStopped()) {
                        stop();
                        return {};
                    }
                    {
                        const std::scoped_lock lock{m_changedTrackMutex};
                        m_streamFormat = *format;
                    }
                    BandcampStreamProperties properties = bandcampDecoderStreamProperties(
                        inner->trackHasChanged() ? inner->changedTrack() : Fooyin::Track{}, *format);
                    if(properties.bitrate <= 0) {
                        properties.bitrate = inner->bitrate();
                    }
                    publishStreamProperties(properties);
                    return format;
                }

                if(m_remoteDevice) {
                    m_remoteDevice->setNonBlockingReadsEnabled(false);
                    m_remoteDevice->setReadCancellationToken(abortToken());
                    m_remoteDevice->setReconnectOnFinishedEnabled(false);
                }
                try {
                    inner->stop();
                }
                catch(...) {
                    reportDelegateFailure(QStringLiteral("playback_stop_failed"));
                }
                (void)takeInnerDecoder();
                if(attemptStopped()) {
                    break;
                }
                if(m_device->isSequential()) {
                    if(sourcePosition < 0 || m_device->pos() != sourcePosition) {
                        break;
                    }
                }
                else if(!m_device->seek(0)) {
                    break;
                }
            }
            if(attemptStopped()) {
                stop();
                return {};
            }
            playbackDiagnostic(*id, QStringLiteral("playback_decoder_open_failed"),
                               QStringLiteral("No ordered Fooyin decoder could initialize the stream."));
            stop();
            return {};
        }
        catch(...) {
            playbackDiagnostic(*id, QStringLiteral("playback_init_failed"),
                               QStringLiteral("Initialising Bandcamp playback failed."));
            stop();
            return {};
        }
    }

    void start() override
    {
        const auto inner = innerDecoder();
        if(inner) {
            try {
                inner->start();
            }
            catch(...) {
                reportDelegateFailure(QStringLiteral("playback_start_failed"));
            }
        }
    }

    void stop() override
    {
        const auto inner = takeInnerDecoder();
        if(inner) {
            try {
                inner->stop();
            }
            catch(...) {
                reportDelegateFailure(QStringLiteral("playback_stop_failed"));
            }
        }
        m_remoteDevice = nullptr;
        m_device.reset();
        m_hostOpenTimeout = {};
        m_trackId.store(0, std::memory_order_release);
        {
            const std::scoped_lock lock{m_changedTrackMutex};
            m_sourceTrack     = {};
            m_changedTrack    = {};
            m_hasChangedTrack = false;
            m_streamFormat.reset();
        }
    }

    void seek(uint64_t positionMs) override
    {
        const auto inner = innerDecoder();
        if(!inner || !m_device || m_device->isSequential()) {
            return;
        }
        try {
            if(inner->isSeekable()) {
                inner->seek(positionMs);
            }
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_seek_failed"));
        }
    }

    Fooyin::AudioDecoder::ReadResult readAudio(size_t bytes) override
    {
        if(m_delegateFault.load(std::memory_order_acquire)) {
            return ReadResult::errorResult(QStringLiteral("Bandcamp decoder delegation failed."));
        }
        const auto inner = innerDecoder();
        if(!inner) {
            return ReadResult::errorResult(QStringLiteral("Bandcamp decoder is not initialised."));
        }
        try {
            auto result = inner->readAudio(bytes);
            if(result.status == ReadStatus::Error) {
                result.error = QStringLiteral("Bandcamp stream decoding failed.");
            }
            return result;
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_read_failed"));
            return ReadResult::errorResult(QStringLiteral("Bandcamp stream decoding failed."));
        }
    }

    Fooyin::AudioBuffer readBuffer(size_t bytes) override
    {
        if(m_delegateFault.load(std::memory_order_acquire)) {
            return {};
        }
        const auto inner = innerDecoder();
        if(!inner) {
            return {};
        }
        try {
            return inner->readBuffer(bytes);
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_read_failed"));
            return {};
        }
    }

    [[nodiscard]] bool trackHasChanged() const override
    {
        refreshInnerStreamProperties();
        const std::scoped_lock lock{m_changedTrackMutex};
        return m_hasChangedTrack;
    }

    [[nodiscard]] Fooyin::Track changedTrack() const override
    {
        refreshInnerStreamProperties();
        const std::scoped_lock lock{m_changedTrackMutex};
        return m_hasChangedTrack ? m_changedTrack : Fooyin::Track{};
    }

    [[nodiscard]] std::optional<TimedTrackChange> takeTimedTrackChange() override
    {
        return {};
    }

protected:
    void playbackHintsChanged(PlaybackHints hints) override
    {
        const auto inner = innerDecoder();
        if(inner) {
            try {
                inner->setPlaybackHints(hints);
            }
            catch(...) {
                reportDelegateFailure(QStringLiteral("playback_hints_failed"));
            }
        }
    }

    void interruptRead() override
    {
        const auto inner = innerDecoder();
        if(inner) {
            try {
                inner->requestAbort();
            }
            catch(...) {
                reportDelegateFailure(QStringLiteral("playback_abort_failed"));
            }
        }
    }

private:
    void refreshInnerStreamProperties() const
    {
        const auto inner = innerDecoder();
        if(!inner) {
            return;
        }
        try {
            if(!inner->trackHasChanged()) {
                return;
            }
            std::optional<Fooyin::AudioFormat> format;
            {
                const std::scoped_lock lock{m_changedTrackMutex};
                format = m_streamFormat;
            }
            if(!format) {
                return;
            }
            BandcampStreamProperties properties = bandcampDecoderStreamProperties(inner->changedTrack(), *format);
            if(properties.bitrate <= 0) {
                properties.bitrate = inner->bitrate();
            }
            publishStreamProperties(properties);
        }
        catch(...) {
            reportDelegateFailure(QStringLiteral("playback_track_update_failed"));
        }
    }

    void publishStreamProperties(const BandcampStreamProperties& properties) const
    {
        Fooyin::Track source;
        bool changed = false;
        {
            const std::scoped_lock lock{m_changedTrackMutex};
            source                = m_sourceTrack;
            Fooyin::Track updated = m_hasChangedTrack ? m_changedTrack : m_sourceTrack;
            applyBandcampStreamProperties(updated, properties);
            if(bandcampStreamProperties(updated)
               != bandcampStreamProperties(m_hasChangedTrack ? m_changedTrack : m_sourceTrack)) {
                m_changedTrack    = std::move(updated);
                m_hasChangedTrack = true;
                changed           = true;
            }
        }
        if(changed && m_bridge) {
            m_bridge->persistStreamProperties(source, properties);
        }
    }

    [[nodiscard]] bool attemptStopped() const
    {
        return abortToken().stop_requested() || m_delegateFault.load(std::memory_order_acquire)
            || std::chrono::steady_clock::now() >= m_deadline;
    }

    [[nodiscard]] std::shared_ptr<Fooyin::AudioDecoder> innerDecoder() const
    {
        const std::scoped_lock lock{m_innerMutex};
        return m_inner;
    }

    void setInnerDecoder(std::shared_ptr<Fooyin::AudioDecoder> inner)
    {
        const std::scoped_lock lock{m_innerMutex};
        m_inner = std::move(inner);
    }

    std::shared_ptr<Fooyin::AudioDecoder> takeInnerDecoder()
    {
        const std::scoped_lock lock{m_innerMutex};
        return std::exchange(m_inner, {});
    }

    void reportDelegateFailure(const QString& event) const
    {
        m_delegateFault.store(true, std::memory_order_release);
        if(!m_delegateFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            playbackDiagnostic(m_trackId.load(std::memory_order_acquire), event,
                               QStringLiteral("The Fooyin decoder operation failed."));
        }
    }

    bool openRemoteSource(const QString& streamUrl, std::chrono::steady_clock::time_point deadline)
    {
        if(!m_bridge) {
            return false;
        }
        for(int attempt{0}; attempt < 2; ++attempt) {
            if(abortToken().stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            Fooyin::RemoteStreamSource remote;
            if(!m_bridge->createRemoteStreamSource(QUrl{streamUrl, QUrl::StrictMode}, remote)) {
                return false;
            }
            if(abortToken().stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            bool ready = false;
            if(remote.device->isOpen() || remote.device->open(QIODevice::ReadOnly)) {
                remote.remoteDevice->setReconnectOnFinishedEnabled(false);
                remote.remoteDevice->setReadCancellationToken(abortToken());
                const auto timeout      = remote.openTimeout > std::chrono::milliseconds{0}
                                            ? remote.openTimeout
                                            : std::chrono::milliseconds{DecoderInitTimeoutMs};
                const auto openDeadline = std::min(deadline, std::chrono::steady_clock::now() + timeout);
                if(waitForRemoteStreamData(*remote.remoteDevice, abortToken(), openDeadline)) {
                    remote.remoteDevice->setNonBlockingReadsEnabled(true);
                    ready = !remote.device->peek(1).isEmpty();
                    remote.remoteDevice->setNonBlockingReadsEnabled(false);
                }
            }
            if(ready) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if(remaining <= std::chrono::milliseconds{0}) {
                    return false;
                }
                m_hostOpenTimeout = remote.openTimeout;
                m_remoteDevice    = remote.remoteDevice;
                m_device          = std::move(remote.device);
                return true;
            }
            if(attempt != 0 || abortToken().stop_requested()
               || std::chrono::steady_clock::now() + ReopenDelay >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(ReopenDelay);
        }
        return false;
    }

    std::shared_ptr<BandcampPlaybackBridge> m_bridge;
    mutable std::mutex m_changedTrackMutex;
    mutable Fooyin::Track m_sourceTrack;
    mutable Fooyin::Track m_changedTrack;
    mutable bool m_hasChangedTrack{false};
    std::optional<Fooyin::AudioFormat> m_streamFormat;
    mutable std::mutex m_innerMutex;
    std::shared_ptr<Fooyin::AudioDecoder> m_inner;
    std::unique_ptr<QIODevice> m_device;
    Fooyin::RemoteStreamDevice* m_remoteDevice{nullptr};
    std::chrono::milliseconds m_hostOpenTimeout{0};
    std::chrono::steady_clock::time_point m_deadline;
    std::atomic<qint64> m_trackId{0};
    mutable std::atomic_bool m_delegateFault{false};
    mutable std::atomic_bool m_delegateFailureLogged{false};
};
} // namespace

Fooyin::InputCreator bandcampInputCreator(const std::weak_ptr<BandcampPlaybackBridge>& bridge)
{
    Fooyin::InputCreator creator;
    creator.priority = Fooyin::DefaultInputPriority;
    creator.decoder  = [bridge]() -> std::unique_ptr<Fooyin::AudioDecoder> {
        const auto locked = bridge.lock();
        return std::make_unique<BandcampAudioDecoder>(locked);
    };
    return creator;
}
} // namespace Fooyin::Bandcamp
