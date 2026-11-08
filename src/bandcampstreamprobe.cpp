#include "bandcampstreamprobe.h"

#include "bandcampdiagnostics.h"
#include "bandcamplogging.h"
#include "bandcampremotestream.h"

#include <core/engine/audioformat.h>
#include <core/engine/audioinput.h>
#include <core/network/remotestreamdevice.h>

#include <QIODevice>
#include <QMetaObject>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace Fooyin::Bandcamp {
namespace {
constexpr auto ProbeTimeout              = std::chrono::seconds{10};
constexpr auto ProbeBatchTimeout         = std::chrono::seconds{12};
constexpr qsizetype MaxDecoderCandidates = 16;
constexpr qint64 MaxProbeReadBytes       = 1024 * 1024;
const auto ProbePath                     = QStringLiteral("https://bandcamp.invalid/stream");
constexpr qsizetype MaxProbeRequests     = 2;

QString streamProbeFailureName(BandcampStreamProbeFailure failure)
{
    switch(failure) {
        case BandcampStreamProbeFailure::Cancelled:
            return QStringLiteral("cancelled");
        case BandcampStreamProbeFailure::RemoteUnavailable:
            return QStringLiteral("remote_unavailable");
        case BandcampStreamProbeFailure::OpenFailed:
            return QStringLiteral("open_failed");
        case BandcampStreamProbeFailure::TimedOut:
            return QStringLiteral("timed_out");
        case BandcampStreamProbeFailure::EmptyStream:
            return QStringLiteral("empty_stream");
        case BandcampStreamProbeFailure::DecoderUnavailable:
            return QStringLiteral("decoder_unavailable");
        case BandcampStreamProbeFailure::DecoderExhausted:
            return QStringLiteral("decoder_exhausted");
        case BandcampStreamProbeFailure::DecoderLimitReached:
            return QStringLiteral("decoder_limit_reached");
        case BandcampStreamProbeFailure::ReadLimitReached:
            return QStringLiteral("read_limit_reached");
        case BandcampStreamProbeFailure::DecoderException:
            return QStringLiteral("decoder_exception");
        case BandcampStreamProbeFailure::BatchDeadline:
            return QStringLiteral("batch_deadline");
        case BandcampStreamProbeFailure::CapacityRejected:
            return QStringLiteral("capacity_rejected");
    }
    return QStringLiteral("unknown");
}

void logStreamProbeFailure(qint64 trackId, BandcampStreamProbeFailure failure)
{
    if(failure == BandcampStreamProbeFailure::Cancelled) {
        return;
    }
    qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(
        CorrelationContext{.trackId = trackId}, QStringLiteral("stream_probe_failed"),
        {DiagnosticField::text(QStringLiteral("reason"), streamProbeFailureName(failure))});
}

void logRecoveredDecoderException(qint64 trackId)
{
    qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(
        CorrelationContext{.trackId = trackId}, QStringLiteral("stream_probe_decoder_exception_recovered"), {});
}

class ProbeReadLimit final : public QIODevice
{
public:
    explicit ProbeReadLimit(QIODevice* source)
        : m_source{source}
    {
        open(QIODevice::ReadOnly);
    }

    [[nodiscard]] bool isSequential() const override
    {
        return true;
    }

    [[nodiscard]] bool limitReached() const
    {
        return m_limitReached;
    }

    [[nodiscard]] qint64 bytesAvailable() const override
    {
        return QIODevice::bytesAvailable() + std::min(m_source->bytesAvailable(), m_remaining);
    }

    bool waitForReadyRead(int timeoutMs) override
    {
        return m_remaining > 0 && m_source->waitForReadyRead(timeoutMs);
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if(m_remaining <= 0) {
            m_limitReached = true;
            setErrorString(QStringLiteral("Bandcamp stream probe read limit reached."));
            return -1;
        }
        const qint64 count = m_source->read(data, std::min(maxSize, m_remaining));
        if(count > 0) {
            m_remaining -= count;
        }
        return count;
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1;
    }

private:
    QIODevice* m_source{nullptr};
    qint64 m_remaining{MaxProbeReadBytes};
    bool m_limitReached{false};
};

struct RemoteSettingsReset
{
    Fooyin::RemoteStreamDevice* device{nullptr};

    ~RemoteSettingsReset()
    {
        if(device) {
            device->setNonBlockingReadsEnabled(false);
            device->setReadCancellationToken({});
            device->setReconnectOnFinishedEnabled(true);
        }
    }
};

QThreadPool& probePool()
{
    // Fooyin calls shutdown before QPluginLoader::unload; Qt's default
    // PreventUnloadHint keeps the plugin image mapped while workers finish.
    static QThreadPool pool;
    static const bool configured = (pool.setMaxThreadCount(2), pool.setExpiryTimeout(30'000), true);
    Q_UNUSED(configured);
    return pool;
}

BandcampStreamProbeResult readProperties(const std::shared_ptr<Fooyin::AudioLoader>& loader,
                                         const QPointer<Fooyin::RemoteIoService>& remoteIo, std::mutex& stateMutex,
                                         const BandcampResolvedTrack& resolved, std::stop_token cancellation,
                                         std::chrono::steady_clock::time_point batchDeadline)
{
    BandcampStreamProbeResult result{.trackId = resolved.track.id};
    if(cancellation.stop_requested()) {
        result.failure = BandcampStreamProbeFailure::Cancelled;
        return result;
    }
    if(!loader) {
        result.failure = BandcampStreamProbeFailure::DecoderUnavailable;
        return result;
    }

    std::stop_source probeCancellation;
    std::stop_callback cancelProbe{cancellation, [&probeCancellation]() { probeCancellation.request_stop(); }};
    const auto probeDeadline  = std::min(batchDeadline, std::chrono::steady_clock::now() + ProbeTimeout);
    const auto probeToken     = probeCancellation.get_token();
    const auto stoppedFailure = [&]() {
        if(cancellation.stop_requested()) {
            return BandcampStreamProbeFailure::Cancelled;
        }
        return std::chrono::steady_clock::now() >= batchDeadline ? BandcampStreamProbeFailure::BatchDeadline
                                                                 : BandcampStreamProbeFailure::TimedOut;
    };
    std::jthread deadlineThread{[&probeCancellation, probeDeadline](std::stop_token timerStop) {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock{mutex};
        const bool interrupted = changed.wait_until(lock, timerStop, probeDeadline, [] { return false; });
        if(!interrupted && !timerStop.stop_requested()) {
            probeCancellation.request_stop();
        }
    }};

    const auto decoders = loader->decoders();
    qsizetype candidatesTried{0};
    bool decoderAvailable{false};
    bool decoderException{false};
    bool candidateLimitReached{false};
    for(const auto& entry : decoders) {
        if(!entry.enabled || !entry.creator || probeToken.stop_requested()) {
            continue;
        }
        auto candidate = entry.creator();
        if(!candidate || !candidate->supportsRemoteSources()) {
            continue;
        }
        decoderAvailable = true;
        if(candidatesTried >= MaxDecoderCandidates) {
            candidateLimitReached = true;
            break;
        }
        ++candidatesTried;
        auto decoder = std::shared_ptr<Fooyin::AudioDecoder>{std::move(candidate)};

        Fooyin::RemoteStreamSource remote;
        {
            const std::scoped_lock lock{stateMutex};
            if(!remoteIo) {
                result.failure = BandcampStreamProbeFailure::RemoteUnavailable;
                return result;
            }
            const QUrl streamUrl{resolved.track.streamUrl, QUrl::StrictMode};
            remote = remoteIo->createStreamSource(streamUrl);
        }
        if(!remote.device || !remote.remoteDevice) {
            result.failure = BandcampStreamProbeFailure::RemoteUnavailable;
            return result;
        }
        RemoteSettingsReset reset{remote.remoteDevice};
        if(!remote.device->isOpen() && !remote.device->open(QIODevice::ReadOnly)) {
            result.failure = probeToken.stop_requested() ? stoppedFailure() : BandcampStreamProbeFailure::OpenFailed;
            return result;
        }
        remote.remoteDevice->setReconnectOnFinishedEnabled(false);
        remote.remoteDevice->setReadCancellationToken(probeToken);
        const auto remaining
            = std::chrono::duration_cast<std::chrono::milliseconds>(probeDeadline - std::chrono::steady_clock::now());
        if(remaining.count() <= 0) {
            result.failure = std::chrono::steady_clock::now() >= batchDeadline
                               ? BandcampStreamProbeFailure::BatchDeadline
                               : BandcampStreamProbeFailure::TimedOut;
            return result;
        }
        const auto requestedTimeout = remote.openTimeout.count() > 0
                                        ? remote.openTimeout
                                        : std::chrono::duration_cast<std::chrono::milliseconds>(ProbeTimeout);
        const auto timeout          = std::min(
            {requestedTimeout, std::chrono::duration_cast<std::chrono::milliseconds>(ProbeTimeout), remaining});
        const auto openDeadline = std::min(probeDeadline, std::chrono::steady_clock::now() + timeout);
        if(!waitForRemoteStreamData(*remote.remoteDevice, probeToken, openDeadline)) {
            result.failure = probeToken.stop_requested() ? stoppedFailure()
                                                         : (std::chrono::steady_clock::now() >= batchDeadline
                                                                ? BandcampStreamProbeFailure::BatchDeadline
                                                                : BandcampStreamProbeFailure::TimedOut);
            return result;
        }
        if(probeToken.stop_requested()) {
            result.failure = stoppedFailure();
            return result;
        }
        remote.remoteDevice->setNonBlockingReadsEnabled(true);
        const bool hasData = !remote.device->peek(1).isEmpty();
        remote.remoteDevice->setNonBlockingReadsEnabled(false);
        if(!hasData) {
            result.failure = probeToken.stop_requested() ? stoppedFailure() : BandcampStreamProbeFailure::EmptyStream;
            return result;
        }

        Fooyin::AudioSource source;
        ProbeReadLimit limitedDevice{remote.device.get()};
        source.filepath           = ProbePath;
        source.device             = &limitedDevice;
        source.remoteStreamDevice = remote.remoteDevice;
        source.remoteOpenTimeout  = timeout;

        Fooyin::Track decoderTrack{ProbePath};
        decoderTrack.setMetadataWasRead(true);
        std::stop_callback abortOnCancel{probeToken, [decoder]() { decoder->requestAbort(); }};
        try {
            const auto format = decoder->init(source, decoderTrack, Fooyin::AudioDecoder::UpdateTracks);
            if(!format) {
                decoder->stop();
                if(probeToken.stop_requested()) {
                    result.failure = stoppedFailure();
                    return result;
                }
                if(limitedDevice.limitReached()) {
                    result.failure = BandcampStreamProbeFailure::ReadLimitReached;
                    return result;
                }
                continue;
            }
            if(probeToken.stop_requested()) {
                decoder->stop();
                result.failure = stoppedFailure();
                return result;
            }
            if(limitedDevice.limitReached()) {
                decoder->stop();
                result.failure = BandcampStreamProbeFailure::ReadLimitReached;
                return result;
            }
            BandcampStreamProperties properties = bandcampDecoderStreamProperties(
                decoder->trackHasChanged() ? decoder->changedTrack() : Fooyin::Track{}, *format);
            if(properties.bitrate <= 0) {
                properties.bitrate = decoder->bitrate();
            }
            decoder->stop();
            if(limitedDevice.limitReached()) {
                result.failure = BandcampStreamProbeFailure::ReadLimitReached;
                return result;
            }
            if(decoderException) {
                logRecoveredDecoderException(resolved.track.id);
            }
            result.properties = std::move(properties);
            return result;
        }
        catch(...) {
            decoderException = true;
            try {
                decoder->stop();
            }
            catch(...) {
            }
            if(probeToken.stop_requested()) {
                result.failure = stoppedFailure();
                return result;
            }
            if(limitedDevice.limitReached()) {
                result.failure = BandcampStreamProbeFailure::ReadLimitReached;
                return result;
            }
        }
        if(probeToken.stop_requested()) {
            result.failure = stoppedFailure();
            return result;
        }
        if(std::chrono::steady_clock::now() >= batchDeadline) {
            result.failure = BandcampStreamProbeFailure::BatchDeadline;
            return result;
        }
        if(std::chrono::steady_clock::now() >= probeDeadline) {
            result.failure = BandcampStreamProbeFailure::TimedOut;
            return result;
        }
    }
    if(probeToken.stop_requested()) {
        result.failure = stoppedFailure();
    }
    else if(std::chrono::steady_clock::now() >= batchDeadline) {
        result.failure = BandcampStreamProbeFailure::BatchDeadline;
    }
    else if(candidateLimitReached) {
        result.failure = BandcampStreamProbeFailure::DecoderLimitReached;
    }
    else if(!decoderAvailable) {
        result.failure = BandcampStreamProbeFailure::DecoderUnavailable;
    }
    else if(decoderException) {
        result.failure = BandcampStreamProbeFailure::DecoderException;
    }
    else {
        result.failure = BandcampStreamProbeFailure::DecoderExhausted;
    }
    return result;
}
} // namespace

struct BandcampStreamProbe::State
{
    std::shared_ptr<Fooyin::AudioLoader> audioLoader;
    QPointer<Fooyin::RemoteIoService> remoteIo;
    std::mutex mutex;
};

struct BandcampStreamProbe::Request
{
    QPointer<QObject> callbackContext;
    Completion completion;
    std::stop_source cancellation;
    std::mutex completionMutex;
    std::mutex resultsMutex;
    QList<BandcampStreamProbeResult> results;
    std::vector<bool> itemCompleted;
    qsizetype remaining{0};
    bool completionScheduled{false};
    std::atomic_bool disabled{false};
    std::function<void(std::optional<BandcampStreamProbeFailure>)> finish;
    QPointer<QTimer> deadlineTimer;
};

BandcampStreamProbe::BandcampStreamProbe(std::shared_ptr<Fooyin::AudioLoader> audioLoader,
                                         Fooyin::RemoteIoService* remoteIo, QObject* parent)
    : QObject{parent}
    , m_state{std::make_shared<State>()}
{
    m_state->audioLoader = std::move(audioLoader);
    m_state->remoteIo    = remoteIo;
}

BandcampStreamProbe::~BandcampStreamProbe()
{
    shutdown();
}

BandcampStreamProbeSubmission BandcampStreamProbe::probe(const QList<BandcampResolvedTrack>& tracks,
                                                         QObject* callbackContext, Completion completion)
{
    if(m_shuttingDown || !callbackContext || !completion) {
        return {.rejection = BandcampStreamProbeFailure::Cancelled};
    }
    if(m_requests.size() >= MaxProbeRequests) {
        for(const BandcampResolvedTrack& track : tracks) {
            logStreamProbeFailure(track.track.id, BandcampStreamProbeFailure::CapacityRejected);
        }
        return {.rejection = BandcampStreamProbeFailure::CapacityRejected};
    }
    const quint64 token = m_nextToken++;

    auto request             = std::make_shared<Request>();
    request->callbackContext = callbackContext;
    request->completion      = std::move(completion);
    request->remaining       = tracks.size();
    request->itemCompleted.resize(static_cast<size_t>(tracks.size()), false);
    request->results.reserve(tracks.size());
    for(const BandcampResolvedTrack& track : tracks) {
        request->results.append(BandcampStreamProbeResult{.trackId = track.track.id});
    }
    m_requests.insert(token, request);

    const auto state     = m_state;
    const auto stopToken = request->cancellation.get_token();
    const QPointer<BandcampStreamProbe> guard{this};
    const QPointer<QObject> callbackGuard{callbackContext};
    const std::weak_ptr<Request> weakRequest{request};

    auto dispatchCompletion
        = [guard, callbackGuard, token, weakRequest](QList<BandcampStreamProbeResult> results) mutable {
              if(!guard) {
                  return;
              }
              QMetaObject::invokeMethod(
                  guard,
                  [guard, callbackGuard, token, weakRequest, results = std::move(results)]() mutable {
                      if(!guard) {
                          return;
                      }
                      const auto request = weakRequest.lock();
                      if(!request || guard->m_requests.value(token) != request) {
                          return;
                      }
                      if(request->deadlineTimer) {
                          request->deadlineTimer->stop();
                          request->deadlineTimer->deleteLater();
                          request->deadlineTimer.clear();
                      }
                      if(guard->m_shuttingDown || !callbackGuard) {
                          guard->m_requests.remove(token);
                          return;
                      }
                      const bool queued = QMetaObject::invokeMethod(
                          callbackGuard,
                          [guard, callbackGuard, token, weakRequest, results = std::move(results)]() mutable {
                              if(!guard || !callbackGuard) {
                                  return;
                              }
                              const auto request = weakRequest.lock();
                              if(!request || request->disabled.load(std::memory_order_relaxed)) {
                                  return;
                              }
                              Completion callback;
                              {
                                  const std::scoped_lock lock{request->completionMutex};
                                  callback = std::move(request->completion);
                              }
                              if(callback && !request->disabled.load(std::memory_order_relaxed)) {
                                  callback(std::move(results));
                              }
                              if(guard) {
                                  QMetaObject::invokeMethod(
                                      guard,
                                      [guard, token]() {
                                          if(guard) {
                                              guard->m_requests.remove(token);
                                          }
                                      },
                                      Qt::QueuedConnection);
                              }
                          },
                          Qt::QueuedConnection);
                      if(!queued) {
                          guard->m_requests.remove(token);
                      }
                  },
                  Qt::QueuedConnection);
          };

    auto finishBatch
        = [weakRequest, dispatchCompletion](std::optional<BandcampStreamProbeFailure> unfinishedFailure) mutable {
              const auto request = weakRequest.lock();
              if(!request) {
                  return;
              }
              QList<BandcampStreamProbeResult> results;
              {
                  const std::scoped_lock lock{request->resultsMutex};
                  if(request->completionScheduled) {
                      return;
                  }
                  if(unfinishedFailure) {
                      for(size_t index = 0; index < request->itemCompleted.size(); ++index) {
                          if(!request->itemCompleted[index]) {
                              request->results[static_cast<qsizetype>(index)].failure = *unfinishedFailure;
                          }
                      }
                  }
                  request->completionScheduled = true;
                  results                      = std::move(request->results);
              }
              dispatchCompletion(std::move(results));
          };
    request->finish = finishBatch;

    auto completeItem = [weakRequest, finishBatch](qsizetype index, BandcampStreamProbeResult result) mutable {
        const auto request = weakRequest.lock();
        if(!request) {
            return;
        }
        bool lastItem = false;
        {
            const std::scoped_lock lock{request->resultsMutex};
            const size_t itemIndex = static_cast<size_t>(index);
            if(request->completionScheduled || request->itemCompleted[itemIndex]) {
                return;
            }
            request->results[index]           = std::move(result);
            request->itemCompleted[itemIndex] = true;
            lastItem                          = --request->remaining == 0;
        }
        if(lastItem) {
            finishBatch(std::nullopt);
        }
    };

    QObject::connect(callbackContext, &QObject::destroyed, this, [this, token, weakRequest]() {
        if(const auto request = weakRequest.lock()) {
            request->disabled.store(true, std::memory_order_relaxed);
            if(request->deadlineTimer) {
                request->deadlineTimer->stop();
                request->deadlineTimer->deleteLater();
                request->deadlineTimer.clear();
            }
            const std::scoped_lock lock{request->completionMutex};
            request->completion = {};
        }
        m_requests.remove(token);
    });

    if(tracks.isEmpty()) {
        finishBatch(std::nullopt);
        return {.token = token};
    }

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    request->deadlineTimer = timer;
    QObject::connect(timer, &QTimer::timeout, this, [request]() {
        request->cancellation.request_stop();
        if(request->finish) {
            request->finish(BandcampStreamProbeFailure::BatchDeadline);
        }
    });
    timer->start(static_cast<int>(ProbeBatchTimeout.count() * 1000));

    const auto batchDeadline = std::chrono::steady_clock::now() + ProbeBatchTimeout;
    for(qsizetype index = 0; index < tracks.size(); ++index) {
        const BandcampResolvedTrack track = tracks.at(index);
        try {
            probePool().start(
                QRunnable::create([state, stopToken, batchDeadline, track, index, completeItem]() mutable {
                    BandcampStreamProbeResult result;
                    if(stopToken.stop_requested() || std::chrono::steady_clock::now() >= batchDeadline) {
                        result = BandcampStreamProbeResult{
                            .trackId = track.track.id,
                            .failure = stopToken.stop_requested() ? BandcampStreamProbeFailure::Cancelled
                                                                  : BandcampStreamProbeFailure::BatchDeadline,
                        };
                    }
                    else {
                        try {
                            result = readProperties(state->audioLoader, state->remoteIo, state->mutex, track, stopToken,
                                                    batchDeadline);
                        }
                        catch(...) {
                            result = BandcampStreamProbeResult{.trackId = track.track.id,
                                                               .failure = BandcampStreamProbeFailure::DecoderException};
                        }
                    }
                    if(result.failure) {
                        logStreamProbeFailure(result.trackId, *result.failure);
                    }
                    completeItem(index, std::move(result));
                }));
        }
        catch(...) {
            BandcampStreamProbeResult result{.trackId = track.track.id,
                                             .failure = BandcampStreamProbeFailure::DecoderException};
            logStreamProbeFailure(result.trackId, *result.failure);
            completeItem(index, std::move(result));
        }
    }
    return {.token = token};
}

void BandcampStreamProbe::cancel(quint64 token)
{
    const auto request = m_requests.value(token);
    if(request) {
        request->cancellation.request_stop();
        if(request->finish) {
            request->finish(BandcampStreamProbeFailure::Cancelled);
        }
    }
}

void BandcampStreamProbe::shutdown()
{
    if(m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    for(const auto& request : std::as_const(m_requests)) {
        request->disabled.store(true, std::memory_order_relaxed);
        request->callbackContext.clear();
        {
            const std::scoped_lock lock{request->completionMutex};
            request->completion = {};
        }
        request->cancellation.request_stop();
        if(request->deadlineTimer) {
            request->deadlineTimer->stop();
            request->deadlineTimer->deleteLater();
            request->deadlineTimer.clear();
        }
    }
    m_requests.clear();
    {
        const std::scoped_lock lock{m_state->mutex};
        m_state->remoteIo.clear();
    }
}
} // namespace Fooyin::Bandcamp
