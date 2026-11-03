#include "bandcampplayback.h"

#include "bandcamplogging.h"
#include "bandcamprefresh.h"
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
constexpr auto PlaybackSourceDeadlineLimit = std::chrono::milliseconds{MaxItemTimeoutMs};
constexpr auto ReopenDelay                 = std::chrono::milliseconds{100};

struct PlaybackWaiter
{
    std::mutex mutex;
    std::condition_variable changed;
    bool completed{false};
    bool cancelled{false};
    std::optional<BandcampPlaybackBridge::PreparedTransport> transport;
    QString failure;
    std::chrono::steady_clock::time_point deadline
        = std::chrono::steady_clock::now() + PlaybackSourceDeadlineLimit;
    QPointer<BandcampRefreshOperation> operation;
};

void completeWaiter(const std::shared_ptr<PlaybackWaiter>& waiter, QString failure)
{
    {
        const std::scoped_lock lock{waiter->mutex};
        if(waiter->completed || waiter->transport) {
            return;
        }
        waiter->failure = std::move(failure);
        waiter->completed = true;
    }
    waiter->changed.notify_all();
}

void playbackDiagnostic(qint64 trackId, const QString& event, const QString& reason)
{
    const CorrelationContext context{.trackId = trackId};
    qCWarning(BANDCAMP_TRACKS).noquote()
        << formatTraceEvent(context, event, {DiagnosticField::text(QStringLiteral("reason"), reason)});
}
} // namespace

struct BandcampPlaybackBridge::State
{
    QPointer<BandcampService> service;
    std::shared_ptr<Fooyin::AudioLoader> audioLoader;
    QPointer<Fooyin::RemoteIoService> remoteIo;
    std::mutex remoteIoMutex;
    QPointer<Fooyin::SettingsManager> settings;
    FooyinLibraryApi libraryApi;
    std::atomic_bool shuttingDown{false};
    std::stop_source shutdownSource;
    std::mutex waitersMutex;
    std::vector<std::weak_ptr<PlaybackWaiter>> waiters;
    std::mutex operationsMutex;
    std::vector<QPointer<BandcampRefreshOperation>> operations;

    void removeWaiter(const std::shared_ptr<PlaybackWaiter>& waiter)
    {
        const std::scoped_lock lock{waitersMutex};
        std::erase_if(waiters, [&waiter](const auto& candidate) {
            const auto current = candidate.lock();
            return !current || current == waiter;
        });
    }

    void removeOperation(BandcampRefreshOperation* operation)
    {
        const std::scoped_lock lock{operationsMutex};
        std::erase_if(operations, [operation](const auto& current) { return current.isNull() || current == operation; });
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
            waiter->failure = QStringLiteral("Playback preparation was cancelled.");
            waiter->changed.notify_all();
            return;
        }
    }

    if(!state->service || !state->settings || !state->audioLoader) {
        completeWaiter(waiter, QStringLiteral("Playback services are unavailable."));
        return;
    }

    try {
        FySettings settings;
        const auto loaded = loadBandcampSettings(settings);
        OperationOptions options;
        options.parallelism = loaded.parallelism;
        options.itemTimeoutMs = loaded.itemTimeoutMs;
        options.discographyOrder = loaded.discographyOrder;
        options.prePlaybackRefreshWindowMs
            = refreshWindowMilliseconds(loaded.prePlaybackRefreshWindowMinutes);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds{options.itemTimeoutMs};
        {
            const std::scoped_lock lock{waiter->mutex};
            if(state->shuttingDown.load(std::memory_order_acquire)) {
                waiter->completed = true;
                waiter->failure = QStringLiteral("Playback preparation was withdrawn.");
                waiter->changed.notify_all();
                return;
            }
            if(waiter->cancelled || waiter->completed) {
                waiter->completed = true;
                waiter->failure = QStringLiteral("Playback preparation was cancelled.");
                waiter->changed.notify_all();
                return;
            }
            waiter->deadline = deadline;
        }
        waiter->changed.notify_all();

        auto* operation = new BandcampRefreshOperation{state->service, {track}, options, state->libraryApi,
                                                       state->service};
        {
            const std::scoped_lock lock{state->operationsMutex, waiter->mutex};
            if(state->shuttingDown.load(std::memory_order_acquire) || waiter->cancelled || waiter->completed) {
                delete operation;
                if(!waiter->completed) {
                    waiter->completed = true;
                    waiter->failure = state->shuttingDown.load(std::memory_order_acquire)
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
        QObject::connect(operation, &BandcampRefreshOperation::transportPrepared, operation,
                         [waiter, deadline](qint64, const QString&, const QString& streamUrl, qint64, bool) {
            {
                const std::scoped_lock lock{waiter->mutex};
                if(waiter->cancelled || waiter->completed) {
                    return;
                }
                waiter->transport = BandcampPlaybackBridge::PreparedTransport{streamUrl, deadline};
            }
            waiter->changed.notify_all();
        });
        QObject::connect(operation, &BandcampRefreshOperation::consumerFinished, operation,
                         [waiter](qint64, const BandcampRefreshOutcome& outcome) {
            if(std::holds_alternative<BandcampRefreshFailure>(outcome)) {
                completeWaiter(waiter, QStringLiteral("Bandcamp could not prepare a playable stream."));
            }
            else if(std::holds_alternative<BandcampRefreshCancelled>(outcome)) {
                completeWaiter(waiter, QStringLiteral("Playback preparation was cancelled."));
            }
        });
        QObject::connect(operation, &BandcampRefreshOperation::finished, operation,
                         [weakState, waiter, operation](const BandcampRefreshSummary&) {
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
    catch(...) {
        completeWaiter(waiter, QStringLiteral("Starting playback preparation failed."));
    }
}

void cancelPreparation(const std::weak_ptr<BandcampPlaybackBridge::State>& weakState,
                       const std::shared_ptr<PlaybackWaiter>& waiter)
{
    const auto application = QCoreApplication::instance();
    if(!application) {
        return;
    }
    QMetaObject::invokeMethod(application, [weakState, waiter]() {
        if(const auto state = weakState.lock()) {
            QPointer<BandcampRefreshOperation> operation;
            {
                const std::scoped_lock lock{waiter->mutex};
                operation = waiter->operation;
            }
            if(operation) {
                try {
                    operation->cancel();
                }
                catch(...) {
                    completeWaiter(waiter, QStringLiteral("Playback operation withdrawal failed."));
                    playbackDiagnostic(0, QStringLiteral("playback.withdraw.failed"),
                                       QStringLiteral("Withdrawing playback preparation failed."));
                }
            }
        }
    }, Qt::QueuedConnection);
}
} // namespace

BandcampPlaybackBridge::BandcampPlaybackBridge(BandcampService* service,
                                               std::shared_ptr<Fooyin::AudioLoader> audioLoader,
                                               Fooyin::RemoteIoService* remoteIo,
                                               Fooyin::SettingsManager* settings,
                                               FooyinLibraryApi libraryApi)
    : m_state{std::make_shared<State>()}
{
    m_state->service = service;
    m_state->audioLoader = std::move(audioLoader);
    m_state->remoteIo = remoteIo;
    m_state->settings = settings;
    m_state->libraryApi = std::move(libraryApi);
}

BandcampPlaybackBridge::~BandcampPlaybackBridge() = default;

std::shared_ptr<Fooyin::AudioLoader> BandcampPlaybackBridge::audioLoader() const
{
    return m_state->audioLoader;
}

bool BandcampPlaybackBridge::createRemoteStreamSource(const QUrl& url,
                                                       Fooyin::RemoteStreamSource& source) const
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
    const auto state = m_state;
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
       || !QMetaObject::invokeMethod(application, [weakState, waiter, track]() {
              if(const auto locked = weakState.lock()) {
                  beginPreparation(locked, waiter, track);
              }
              else {
                  completeWaiter(waiter, QStringLiteral("Playback preparation was withdrawn."));
              }
          }, Qt::QueuedConnection)) {
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
        waiter->failure = reason;
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
    const QString reason = waiter->failure.isEmpty()
        ? QStringLiteral("Bandcamp could not prepare a playable stream.") : waiter->failure;
    lock.unlock();
    state->removeWaiter(waiter);
    return Failure{reason};
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
                        waiter->failure = QStringLiteral("Playback preparation was withdrawn.");
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
    auto cancelOperations = [state]() {
        std::vector<QPointer<BandcampRefreshOperation>> operations;
        {
            const std::scoped_lock lock{state->operationsMutex};
            operations = std::exchange(state->operations, {});
        }
        for(const QPointer<BandcampRefreshOperation>& operation : operations) {
            if(operation) {
                try {
                    operation->cancel();
                }
                catch(...) {
                    playbackDiagnostic(0, QStringLiteral("playback.shutdown.withdraw.failed"),
                                       QStringLiteral("Withdrawing an operation during shutdown failed."));
                }
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
            reportDelegateFailure(QStringLiteral("playback.seekability.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.decoder-policy.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.prebuffer.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.input-state.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.decoder-warning.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.repeat-policy.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.bitrate.failed"));
            return 0;
        }
    }

    std::optional<Fooyin::AudioFormat> init(const Fooyin::AudioSource& source, const Fooyin::Track& track,
                                            DecoderOptions options) override
    {
        stop();
        const auto id = parseBandcampTrackUri(track.filepath());
        if(!id || source.filepath != track.filepath()) {
            playbackDiagnostic(id.value_or(0), QStringLiteral("playback.identity.failed"),
                               QStringLiteral("The virtual track identity is invalid."));
            return {};
        }
        m_trackId.store(*id, std::memory_order_release);
        m_delegateFailureLogged.store(false, std::memory_order_release);
        m_delegateFault.store(false, std::memory_order_release);
        if(!m_bridge) {
            playbackDiagnostic(*id, QStringLiteral("playback.prepare.failed"),
                               QStringLiteral("The playback bridge is unavailable."));
            return {};
        }

        const auto shutdownToken = m_bridge->shutdownToken();
        std::stop_callback shutdownWake{shutdownToken, [this]() { requestAbort(); }};
        if(shutdownToken.stop_requested()) {
            playbackDiagnostic(*id, QStringLiteral("playback.prepare.failed"),
                               QStringLiteral("Playback is shutting down."));
            return {};
        }

        try {
            auto prepared = m_bridge->prepare(track, abortToken());
            if(const auto* failure = std::get_if<BandcampPlaybackBridge::Failure>(&prepared)) {
                playbackDiagnostic(*id, QStringLiteral("playback.prepare.failed"), failure->reason);
                return {};
            }
            const auto& transport = std::get<BandcampPlaybackBridge::PreparedTransport>(prepared);
            m_deadline = transport.deadline;
            if(attemptStopped()) {
                playbackDiagnostic(*id, QStringLiteral("playback.prepare.failed"),
                                   QStringLiteral("The playback deadline expired."));
                return {};
            }

            const QString safeUrl = QStringLiteral("https://bandcamp.invalid/track.mp3");
            Fooyin::Track decoderTrack{track};
            decoderTrack.setFilePath(safeUrl);
            decoderTrack.removeExtraProperty(StreamProviderProperty);
            decoderTrack.removeExtraProperty(BandcampSourceUrlProperty);
            decoderTrack.removeExtraProperty(BandcampStreamUrlProperty);
            decoderTrack.removeExtraProperty(BandcampExpiresAtProperty);
            decoderTrack.removeExtraProperty(BandcampTrackIdProperty);
            Fooyin::AudioSource decoderSource;
            if(!openRemoteSource(transport.streamUrl, m_deadline)) {
                playbackDiagnostic(*id, QStringLiteral("playback.stream.open.failed"),
                                   QStringLiteral("The Bandcamp stream could not be opened."));
                return {};
            }
            decoderSource.filepath = safeUrl;
            decoderSource.device = m_device.get();
            decoderSource.remoteStreamDevice = m_remoteDevice;

            auto candidates = m_bridge->audioLoader()->decodersForFile(safeUrl);
            if(candidates.empty()) {
                playbackDiagnostic(*id, QStringLiteral("playback.decoder.unavailable"),
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
                auto inner = std::shared_ptr<Fooyin::AudioDecoder>{std::move(candidate)};
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
                    ? std::min(m_hostOpenTimeout, remainingOpenTime) : remainingOpenTime;
                const auto format = inner->init(decoderSource, decoderTrack, options);
                if(format) {
                    if(m_remoteDevice) {
                        m_remoteDevice->setReconnectOnFinishedEnabled(false);
                    }
                    if(attemptStopped()) {
                        stop();
                        return {};
                    }
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
                    reportDelegateFailure(QStringLiteral("playback.stop.failed"));
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
            playbackDiagnostic(*id, QStringLiteral("playback.decoder.open.failed"),
                               QStringLiteral("No ordered Fooyin decoder could initialize the stream."));
            stop();
            return {};
        }
        catch(...) {
            playbackDiagnostic(*id, QStringLiteral("playback.init.failed"),
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
                reportDelegateFailure(QStringLiteral("playback.start.failed"));
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
                reportDelegateFailure(QStringLiteral("playback.stop.failed"));
            }
        }
        m_remoteDevice = nullptr;
        m_device.reset();
        m_hostOpenTimeout = {};
        m_trackId.store(0, std::memory_order_release);
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
            reportDelegateFailure(QStringLiteral("playback.seek.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.read.failed"));
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
            reportDelegateFailure(QStringLiteral("playback.read.failed"));
            return {};
        }
    }

    [[nodiscard]] bool trackHasChanged() const override
    {
        return false;
    }

    [[nodiscard]] Fooyin::Track changedTrack() const override
    {
        return {};
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
                reportDelegateFailure(QStringLiteral("playback.hints.failed"));
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
                reportDelegateFailure(QStringLiteral("playback.abort.failed"));
            }
        }
    }

private:
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
            remote.remoteDevice->setReconnectOnFinishedEnabled(false);
            remote.remoteDevice->setReadCancellationToken(abortToken());
            bool ready = false;
            if(remote.device->isOpen() || remote.device->open(QIODevice::ReadOnly)) {
                // Wait for response data before the first read. Qt caches isSequential() on the first
                // read or peek, so an early read leaves seeks failing on range-capable streams.
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if(remaining > std::chrono::milliseconds{0}
                   && remote.device->waitForReadyRead(static_cast<int>(
                       std::clamp<std::int64_t>(remaining.count(), 1, std::numeric_limits<int>::max())))) {
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
                m_remoteDevice = remote.remoteDevice;
                m_device = std::move(remote.device);
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
    creator.decoder = [bridge]() -> std::unique_ptr<Fooyin::AudioDecoder> {
        const auto locked = bridge.lock();
        return std::make_unique<BandcampAudioDecoder>(locked);
    };
    return creator;
}
} // namespace Fooyin::Bandcamp
