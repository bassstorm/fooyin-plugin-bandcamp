#include "bandcampreleaseresolver.h"

#include "bandcampassembly.h"
#include "bandcampattemptsupervisor.h"
#include "bandcampclient.h"
#include "bandcampdiagnostics.h"
#include "bandcamplogging.h"
#include "bandcamppostprocessor.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QRunnable>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <type_traits>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
ResolutionError parentFailure(const ResolutionError& cause, const QUrl& parentUrl, qint64 trackId,
                              ResolutionStage stage)
{
    return makeResolutionError(ResolutionErrorCode::ParentUnavailable, stage, parentUrl, trackId,
                               cause.field.isEmpty() ? QStringLiteral("parent") : cause.field,
                               QStringLiteral("required album parent failed: %1").arg(cause.reason));
}

// The artist is shown only when it differs from the album artist, or when the track has no album.
void logAdmittedTrack(const BandcampTrack& track, const QString& albumArtist, bool standalone,
                      const CorrelationContext& context)
{
    if(!BANDCAMP_TRACKS().isDebugEnabled()) {
        return;
    }
    QList<DiagnosticField> fields;
    if(track.trackNumber > 0) {
        fields.append(DiagnosticField::number(QStringLiteral("num"), track.trackNumber));
    }
    fields.append(DiagnosticField::text(QStringLiteral("title"), track.title));
    if(standalone || track.artist != albumArtist) {
        fields.append(DiagnosticField::text(QStringLiteral("artist"), track.artist));
    }
    fields.append(DiagnosticField::number(QStringLiteral("durationMs"), track.durationMs));
    qCDebug(BANDCAMP_TRACKS).noquote() << formatTraceEvent(context, QStringLiteral("admitted"), fields);
}

void logResolvedAlbum(const BandcampAlbumMetadata& album, qsizetype trackCount, qint64 elapsedMs,
                      const CorrelationContext& context)
{
    qCInfo(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
        context, QStringLiteral("release_resolve_completed"),
        {DiagnosticField::text(QStringLiteral("title"), album.title),
         DiagnosticField::text(QStringLiteral("artist"), album.artist),
         DiagnosticField::number(QStringLiteral("tracks"), static_cast<qint64>(trackCount)),
         DiagnosticField::text(QStringLiteral("releaseDate"), album.releaseDate.toString(Qt::ISODate)),
         DiagnosticField::url(QStringLiteral("url"), QUrl(album.sourceUrl)),
         DiagnosticField::number(QStringLiteral("elapsedMs"), elapsedMs)});
}

void logResolvedTrack(const BandcampTrack& track, qint64 elapsedMs, const CorrelationContext& context)
{
    qCInfo(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
        context, QStringLiteral("release_resolve_completed"),
        {DiagnosticField::text(QStringLiteral("title"), track.title),
         DiagnosticField::text(QStringLiteral("artist"), track.artist),
         DiagnosticField::text(QStringLiteral("releaseDate"), track.releaseDate.toString(Qt::ISODate)),
         DiagnosticField::url(QStringLiteral("url"), QUrl(track.sourceUrl)),
         DiagnosticField::number(QStringLiteral("elapsedMs"), elapsedMs)});
}

BandcampResolutionOutcome resolveScope(const BandcampPageUrl& page, const ReleaseCandidate& primary,
                                       const ReleaseCandidate* parent, CorrelationContext context,
                                       const std::atomic_bool* stop)
{
    if(stop && stop->load(std::memory_order_relaxed)) {
        return ResolutionCancelled{};
    }
    if(page.kind == BandcampPageKind::Album) {
        const auto* candidate = std::get_if<AlbumCandidate>(&primary);
        if(!candidate) {
            return ResolutionFailure{makeResolutionError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                         context.resourceUrl, 0, QStringLiteral("identity"),
                                                         QStringLiteral("parsed resource is not an album"))};
        }
        auto admission = admitAlbum(*candidate, context, stop);
        if(const auto* failure = std::get_if<ResolutionFailure>(&admission)) {
            return *failure;
        }
        if(std::holds_alternative<ResolutionCancelled>(admission)) {
            return ResolutionCancelled{};
        }
        AlbumAdmission refined = std::get<ResolutionSuccess<AlbumAdmission>>(std::move(admission)).value;
        postProcessAlbum(refined, stop);
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const CorrelationContext albumContext = context.withAlbum(refined.album.metadata.id);
        for(const BandcampTrack& track : std::as_const(refined.album.tracks)) {
            logAdmittedTrack(track, refined.album.metadata.artist, false, albumContext.withTrack(track.id));
        }
        return ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.album)}};
    }

    const auto* track = std::get_if<TrackPageCandidate>(&primary);
    if(!track) {
        return ResolutionFailure{makeResolutionError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                     context.resourceUrl, 0, QStringLiteral("identity"),
                                                     QStringLiteral("parsed resource is not a track"))};
    }
    if(track->track.id.isUsable()) {
        context.trackId = track->track.id.value;
    }

    // The workflow classified parent evidence and fetched the parent when required, so the parent's presence is the
    // decision.
    ResolutionOutcome<TrackAdmission> admission = ResolutionCancelled{};
    if(!parent) {
        admission = admitStandaloneTrack(*track, context, stop);
    }
    else {
        const auto* album = std::get_if<AlbumCandidate>(parent);
        if(!album) {
            return ResolutionFailure{makeResolutionError(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Parse,
                                                         context.resourceUrl, track->track.id.value,
                                                         QStringLiteral("parent.resource"),
                                                         QStringLiteral("parent response did not identify an album"))};
        }
        admission = admitAlbumTrack(*track, *album, context, stop);
    }
    if(const auto* failure = std::get_if<ResolutionFailure>(&admission)) {
        return *failure;
    }
    if(std::holds_alternative<ResolutionCancelled>(admission)) {
        return ResolutionCancelled{};
    }
    TrackAdmission refined = std::get<ResolutionSuccess<TrackAdmission>>(std::move(admission)).value;
    postProcessTrack(refined, stop);
    if(stop && stop->load(std::memory_order_relaxed)) {
        return ResolutionCancelled{};
    }
    CorrelationContext resolvedContext = context;
    if(refined.result.album) {
        resolvedContext = resolvedContext.withAlbum(refined.result.album->id);
        logAdmittedTrack(refined.result.track, refined.result.album->artist, false, resolvedContext);
    }
    else {
        logAdmittedTrack(refined.result.track, {}, true, resolvedContext);
    }
    return ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.result)}};
}

CorrelationContext releaseRequestContext(const BandcampReleaseRequest& request)
{
    const CorrelationContext context = CorrelationContext{}.withResourceUrl(QUrl(request.page.url));
    if(request.id <= 0) {
        return context;
    }
    return request.page.kind == BandcampPageKind::Track ? context.withTrack(request.id) : context.withAlbum(request.id);
}

enum class WorkflowPhase : quint8
{
    Created,
    FetchingPrimary,
    ParsingPrimary,
    FetchingParent,
    ParsingParent,
    Resolving
};

class BandcampReleaseWorkflow : public QObject
{
public:
    using OutcomeCallback = std::function<void(const QString&, BandcampResolutionOutcome)>;
    using SettledCallback = std::function<void(const QString&)>;

    BandcampReleaseWorkflow(BandcampClient* client, BandcampOperationId operationId, BandcampReleaseRequest request,
                            BandcampAttemptPolicy policy, BandcampPageParser parser, OutcomeCallback outcomeCallback,
                            SettledCallback settledCallback, QObject* parent = nullptr)
        : QObject{parent}
        , m_client{client}
        , m_operationId{operationId}
        , m_request{std::move(request)}
        , m_attempts{policy, this}
        , m_parser{std::move(parser)}
        , m_outcomeCallback{std::move(outcomeCallback)}
        , m_settledCallback{std::move(settledCallback)}
    {
        m_context = releaseRequestContext(m_request);
        QObject::connect(&m_attempts, &BandcampAttemptSupervisor::attemptTimedOut, this,
                         [this]() { onAttemptTimedOut(); });
        QObject::connect(&m_attempts, &BandcampAttemptSupervisor::retryReady, this,
                         [this]() { issueFetch(m_currentUrl, m_currentParent); });
    }

    ~BandcampReleaseWorkflow() override
    {
        m_stop->store(true, std::memory_order_relaxed);
        m_attempts.stop();
        if(m_attempts.attemptActive() && m_client && m_requestId != 0) {
            m_client->cancel(m_requestId);
        }
    }

    void start()
    {
        m_started = true;
        m_elapsed.start();
        fetchPage(m_request.page, false);
    }

    void cancel()
    {
        if(m_reported) {
            return;
        }
        m_stop->store(true, std::memory_order_relaxed);
        const QPointer<BandcampReleaseWorkflow> guard{this};
        if(m_attempts.attemptActive() && m_client && m_requestId != 0) {
            m_client->cancel(m_requestId);
        }
        if(!guard) {
            return;
        }
        if(!report(ResolutionCancelled{}) || !guard) {
            return;
        }
        settleIfIdle();
    }

private:
    void fetchPage(const BandcampPageUrl& page, bool parent)
    {
        if(m_reported) {
            return;
        }
        m_attempts.resetBudget();
        issueFetch(QUrl{page.url}, parent);
    }

    // Each attempt gets its own deadline, so a hung request is abandoned and retried like any other failure.
    void issueFetch(const QUrl& url, bool parent)
    {
        m_currentUrl          = url;
        m_currentParent       = parent;
        m_context.resourceUrl = url;
        setPhase(parent ? WorkflowPhase::FetchingParent : WorkflowPhase::FetchingPrimary);
        const quint64 attempt = m_attempts.issueAttempt();
        const QPointer<BandcampReleaseWorkflow> guard{this};
        m_requestId = m_client->fetch(
            url,
            [guard, attempt](BandcampFetchOutcome outcome) mutable {
                if(guard) {
                    guard->onFetch(attempt, std::move(outcome));
                }
            },
            m_context);
    }

    // Bumps the attempt id so the callback of the abandoned request is ignored whenever it is delivered.
    // The request stays outstanding in m_attempts until delivery, so the workflow cannot settle first.
    void abandonFetch()
    {
        const BandcampClient::RequestId hung = m_requestId;
        m_requestId                          = 0;
        m_attempts.abandon();
        if(m_client && hung != 0) {
            m_client->cancel(hung);
        }
    }

    void scheduleRetry(const ResolutionError& error, int httpStatus)
    {
        m_attempts.beginRetry();
        logResolutionRetry(m_context, error, httpStatus, m_attempts);
    }

    void onFetch(quint64 attempt, BandcampFetchOutcome outcome)
    {
        if(!m_attempts.deliver(attempt)) {
            settleIfIdle();
            return;
        }
        const bool parent = m_currentParent;
        m_requestId       = 0;
        if(m_reported) {
            settleIfIdle();
            return;
        }
        if(std::holds_alternative<ResolutionCancelled>(outcome)) {
            if(report(ResolutionCancelled{})) {
                settleIfIdle();
            }
            return;
        }
        if(const auto* failure = std::get_if<BandcampFetchFailure>(&outcome)) {
            m_lastHttpStatus = failure->httpStatus;
            if(m_attempts.canRetry() && isRetriableFetchFailure(*failure)) {
                scheduleRetry(failure->error, failure->httpStatus);
                return;
            }
            if(parent) {
                const qint64 trackId = primaryTrackId();
                if(report(ResolutionFailure{
                       parentFailure(failure->error, m_currentUrl, trackId, ResolutionStage::Acquire)})) {
                    settleIfIdle();
                }
            }
            else {
                if(report(ResolutionFailure{failure->error})) {
                    settleIfIdle();
                }
            }
            return;
        }

        BandcampFetchResponse response = std::get<ResolutionSuccess<BandcampFetchResponse>>(std::move(outcome)).value;
        m_currentUrl                   = response.url;
        m_context.resourceUrl          = response.url;
        setPhase(parent ? WorkflowPhase::ParsingParent : WorkflowPhase::ParsingPrimary);
        parsePage(std::move(response.body), response.url, parent);
    }

    void parsePage(QByteArray body, QUrl effectiveUrl, bool parent)
    {
        m_workerActive = true;
        const QPointer<BandcampReleaseWorkflow> guard{this};
        const CorrelationContext context = m_context;
        const auto stop                  = m_stop;
        const auto parser                = m_parser;
        try {
            QThreadPool::globalInstance()->start(QRunnable::create([guard, body = std::move(body), effectiveUrl, parent,
                                                                    context, stop, parser]() mutable {
                ReleaseParseOutcome outcome = ResolutionCancelled{};
                try {
                    if(!stop->load(std::memory_order_relaxed)) {
                        outcome = parser(body, effectiveUrl, context, stop.get());
                        if(stop->load(std::memory_order_relaxed)) {
                            outcome = ResolutionCancelled{};
                        }
                    }
                }
                catch(...) {
                    outcome = ResolutionFailure{
                        makeResolutionError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse, effectiveUrl, 0,
                                            QStringLiteral("parser"), QStringLiteral("parser dependency failed"))};
                }

                if(!guard) {
                    return;
                }
                QMetaObject::invokeMethod(
                    guard,
                    [guard, parent, effectiveUrl, outcome = std::move(outcome)]() mutable {
                        if(!guard) {
                            return;
                        }
                        guard->m_workerActive = false;
                        guard->onParsed(parent, effectiveUrl, std::move(outcome));
                        if(!guard) {
                            return;
                        }
                        guard->settleIfIdle();
                    },
                    Qt::QueuedConnection);
            }));
        }
        catch(...) {
            m_workerActive = false;
            const QPointer<BandcampReleaseWorkflow> guard{this};
            onParsed(parent, effectiveUrl,
                     ResolutionFailure{makeResolutionError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                           effectiveUrl, primaryTrackId(), QStringLiteral("parser"),
                                                           QStringLiteral("parser worker could not be started"))});
            if(!guard) {
                return;
            }
            guard->settleIfIdle();
        }
    }

    void onParsed(bool parent, const QUrl& effectiveUrl, ReleaseParseOutcome outcome)
    {
        if(m_reported) {
            return;
        }
        if(std::holds_alternative<ResolutionCancelled>(outcome)) {
            report(ResolutionCancelled{});
            return;
        }
        if(const auto* failure = std::get_if<ResolutionFailure>(&outcome)) {
            if(parent) {
                report(ResolutionFailure{
                    parentFailure(failure->error, effectiveUrl, primaryTrackId(), ResolutionStage::Parse)});
            }
            else {
                report(ResolutionFailure{failure->error});
            }
            return;
        }

        ReleaseCandidate candidate = std::get<ResolutionSuccess<ReleaseCandidate>>(std::move(outcome)).value;
        if(parent) {
            if(!std::holds_alternative<AlbumCandidate>(candidate)) {
                report(ResolutionFailure{makeResolutionError(
                    ResolutionErrorCode::ParentUnavailable, ResolutionStage::Parse, effectiveUrl, primaryTrackId(),
                    QStringLiteral("parent.resource"), QStringLiteral("parent response did not identify an album"))});
                return;
            }
            adoptAlbumId(candidate);
            m_parentCandidate = std::move(candidate);
            resolve();
            return;
        }

        if(m_request.page.kind == BandcampPageKind::Album) {
            adoptAlbumId(candidate);
            m_primaryCandidate = std::move(candidate);
            resolve();
            return;
        }
        const auto* track = std::get_if<TrackPageCandidate>(&candidate);
        if(!track) {
            report(ResolutionFailure{makeResolutionError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                         effectiveUrl, 0, QStringLiteral("identity"),
                                                         QStringLiteral("parsed resource is not a track"))});
            return;
        }
        if(track->track.id.isUsable()) {
            m_context.trackId = track->track.id.value;
        }
        const ParentRequirement requirement = classifyParentEvidence(*track, m_context);
        if(const auto* failure = std::get_if<ResolutionFailure>(&requirement)) {
            report(*failure);
            return;
        }
        m_primaryCandidate = std::move(candidate);
        if(std::holds_alternative<StandaloneParentEvidence>(requirement)) {
            resolve();
            return;
        }

        const auto& required = std::get<RequiredAlbumParent>(requirement);
        if(required.albumId) {
            m_context = m_context.withAlbum(*required.albumId);
        }
        fetchPage(BandcampPageUrl{BandcampPageKind::Album, required.url.toString(QUrl::FullyEncoded), {}}, true);
    }

    void resolve()
    {
        if(m_reported || !m_primaryCandidate) {
            return;
        }
        setPhase(WorkflowPhase::Resolving);
        ReleaseCandidate primary = std::move(*m_primaryCandidate);
        m_primaryCandidate.reset();
        std::optional<ReleaseCandidate> parent = std::move(m_parentCandidate);
        m_parentCandidate.reset();
        CorrelationContext context = m_context;
        if(m_request.page.kind == BandcampPageKind::Album) {
            context.resourceUrl = QUrl(m_request.page.url);
            context.trackId     = 0;
        }
        else if(primaryTrackId() > 0) {
            context.trackId = primaryTrackId();
        }
        m_currentUrl = context.resourceUrl;

        m_workerActive = true;
        const QPointer<BandcampReleaseWorkflow> guard{this};
        const BandcampPageUrl page = m_request.page;
        const auto stop            = m_stop;
        const bool needsParent     = parent.has_value();
        const QUrl resolutionUrl   = context.resourceUrl;
        const qint64 trackId       = context.trackId;
        try {
            QThreadPool::globalInstance()->start(QRunnable::create([guard, page, primary = std::move(primary),
                                                                    parent = std::move(parent), context, stop,
                                                                    needsParent, resolutionUrl, trackId]() mutable {
                BandcampResolutionOutcome outcome = ResolutionCancelled{};
                try {
                    if(!stop->load(std::memory_order_relaxed)) {
                        outcome = resolveScope(page, primary, needsParent ? &*parent : nullptr, context, stop.get());
                        if(stop->load(std::memory_order_relaxed)) {
                            outcome = ResolutionCancelled{};
                        }
                    }
                }
                catch(...) {
                    outcome = BandcampResolutionOutcome{ResolutionFailure{makeResolutionError(
                        ResolutionErrorCode::InvalidField, ResolutionStage::Refine, resolutionUrl, trackId,
                        QStringLiteral("processor"), QStringLiteral("resolution dependency failed"))}};
                }

                if(!guard) {
                    return;
                }
                QMetaObject::invokeMethod(
                    guard,
                    [guard, outcome = std::move(outcome)]() mutable {
                        if(!guard) {
                            return;
                        }
                        guard->m_workerActive = false;
                        if(!guard->report(std::move(outcome)) || !guard) {
                            return;
                        }
                        guard->settleIfIdle();
                    },
                    Qt::QueuedConnection);
            }));
        }
        catch(...) {
            m_workerActive = false;
            if(report(ResolutionFailure{makeResolutionError(
                   ResolutionErrorCode::InvalidField, ResolutionStage::Refine, context.resourceUrl, context.trackId,
                   QStringLiteral("processor"), QStringLiteral("resolution worker could not be started"))})) {
                settleIfIdle();
            }
        }
    }

    void adoptAlbumId(const ReleaseCandidate& candidate)
    {
        if(const auto* album = std::get_if<AlbumCandidate>(&candidate); album && album->id.isUsable()) {
            m_context = m_context.withAlbum(album->id.value);
        }
    }

    qint64 primaryTrackId() const
    {
        if(m_primaryCandidate) {
            if(const auto* track = std::get_if<TrackPageCandidate>(&*m_primaryCandidate);
               track && track->track.id.isUsable()) {
                return track->track.id.value;
            }
        }
        return m_context.trackId;
    }

    void onAttemptTimedOut()
    {
        if(m_reported) {
            return;
        }
        ResolutionStage stage = ResolutionStage::Fetch;
        switch(m_phase) {
            case WorkflowPhase::FetchingPrimary:
            case WorkflowPhase::Created:
                stage = ResolutionStage::Fetch;
                break;
            case WorkflowPhase::ParsingPrimary:
            case WorkflowPhase::ParsingParent:
                stage = ResolutionStage::Parse;
                break;
            case WorkflowPhase::FetchingParent:
                stage = ResolutionStage::Acquire;
                break;
            case WorkflowPhase::Resolving:
                stage = ResolutionStage::Refine;
                break;
        }
        const ResolutionError error
            = makeResolutionError(ResolutionErrorCode::Timeout, stage, m_currentUrl, primaryTrackId(),
                                  QStringLiteral("resource"), QStringLiteral("page attempt timed out"));
        if(m_attempts.attemptActive() && m_attempts.canRetry()) {
            abandonFetch();
            scheduleRetry(error, 0);
            return;
        }
        m_stop->store(true, std::memory_order_relaxed);
        const QPointer<BandcampReleaseWorkflow> guard{this};
        abandonFetch();
        if(!guard) {
            return;
        }
        if(report(ResolutionFailure{error})) {
            settleIfIdle();
        }
    }

    bool report(BandcampResolutionOutcome outcome)
    {
        if(m_reported) {
            return true;
        }
        m_reported = true;
        m_attempts.stop();
        if(m_started) {
            if(const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&outcome)) {
                const qint64 elapsedMs = m_elapsed.elapsed();
                std::visit(
                    [this, elapsedMs](const auto& data) {
                        using Data = std::decay_t<decltype(data)>;
                        if constexpr(std::is_same_v<Data, BandcampAlbum>) {
                            logResolvedAlbum(data.metadata, data.tracks.size(), elapsedMs,
                                             m_context.withAlbum(data.metadata.id));
                        }
                        else {
                            CorrelationContext context = m_context.withTrack(data.track.id);
                            if(data.album) {
                                context = context.withAlbum(data.album->id);
                            }
                            logResolvedTrack(data.track, elapsedMs, context);
                        }
                    },
                    success->value);
            }
        }
        if(const auto* failure = std::get_if<ResolutionFailure>(&outcome)) {
            if(m_started) {
                logResolutionFailure(m_context, failure->error, m_lastHttpStatus, m_attempts, m_elapsed.elapsed());
            }
            else {
                logResolutionFailure(m_context, failure->error, m_lastHttpStatus);
            }
        }
        const QPointer<BandcampReleaseWorkflow> guard{this};
        m_outcomeCallback(m_request.key, std::move(outcome));
        return guard;
    }

    void settleIfIdle()
    {
        if(m_settled || !m_attempts.idle() || m_workerActive) {
            return;
        }
        if(!m_reported) {
            if(!report(ResolutionFailure{makeResolutionError(
                   ResolutionErrorCode::FetchFailed, ResolutionStage::Fetch, m_currentUrl, primaryTrackId(),
                   QStringLiteral("workflow"), QStringLiteral("release workflow ended without an outcome"))})) {
                return;
            }
        }
        m_settled = true;
        const QPointer<BandcampReleaseWorkflow> guard{this};
        m_settledCallback(m_request.key);
        if(!guard) {
            return;
        }
    }

    void setPhase(WorkflowPhase phase)
    {
        m_phase = phase;
    }

    QPointer<BandcampClient> m_client;
    BandcampOperationId m_operationId{0};
    BandcampReleaseRequest m_request;
    BandcampAttemptSupervisor m_attempts;
    BandcampPageParser m_parser;
    OutcomeCallback m_outcomeCallback;
    SettledCallback m_settledCallback;
    std::shared_ptr<std::atomic_bool> m_stop{std::make_shared<std::atomic_bool>(false)};
    CorrelationContext m_context;
    std::optional<ReleaseCandidate> m_primaryCandidate;
    std::optional<ReleaseCandidate> m_parentCandidate;
    QUrl m_currentUrl;
    bool m_currentParent{false};
    BandcampClient::RequestId m_requestId{0};
    WorkflowPhase m_phase{WorkflowPhase::Created};
    int m_lastHttpStatus{0};
    QElapsedTimer m_elapsed;
    bool m_started{false};
    bool m_workerActive{false};
    bool m_reported{false};
    bool m_settled{false};
};
} // namespace

struct BandcampReleaseResolver::Impl
{
    BandcampReleaseResolver* owner{nullptr};
    BandcampClient* client{nullptr};
    BandcampOperationId operationId{0};
    QVector<BandcampReleaseRequest> pending;
    qsizetype nextRequest{0};
    OperationOptions options;
    BandcampPageParser parser;
    std::shared_ptr<BandcampWorkflowSlots> workflowSlots;
    QHash<QString, BandcampReleaseWorkflow*> active;
    QHash<QString, BandcampResolvedData> cache;
    QHash<QString, BandcampResolutionOutcome> outcomes;
    qsizetype outcomeDeliveryDepth{0};
    bool started{false};
    bool cancelled{false};
    bool completionSent{false};
    bool quietSent{false};
    bool pumpQueued{false};

    Impl(BandcampReleaseResolver* resolver, QNetworkAccessManager* network, BandcampOperationId id,
         QVector<BandcampReleaseRequest> requests, OperationOptions operationOptions, BandcampPageParser pageParser,
         std::shared_ptr<BandcampWorkflowSlots> capacity)
        : owner{resolver}
        , client{new BandcampClient(network, resolver)}
        , operationId{id}
        , pending{std::move(requests)}
        , options{operationOptions}
        , parser{std::move(pageParser)}
        , workflowSlots{std::move(capacity)}
    {
        QObject::connect(workflowSlots.get(), &BandcampWorkflowSlots::capacityAvailable, owner,
                         [this]() { schedulePump(); });
    }

    void start()
    {
        if(started) {
            return;
        }
        started = true;
        const QPointer<BandcampReleaseResolver> guard{owner};
        pump();
        if(!guard) {
            return;
        }
        maybeComplete();
    }

    void cancel()
    {
        if(cancelled || completionSent) {
            return;
        }
        cancelled = true;
        const QPointer<BandcampReleaseResolver> guard{owner};
        while(nextRequest < pending.size()) {
            const BandcampReleaseRequest request = pending.at(nextRequest++);
            publish(request.key, ResolutionCancelled{});
            if(!guard) {
                return;
            }
        }
        const auto running = active.values();
        for(BandcampReleaseWorkflow* workflow : running) {
            if(workflow) {
                workflow->cancel();
                if(!guard) {
                    return;
                }
            }
        }
        maybeComplete();
    }

    void pump()
    {
        if(!started || cancelled) {
            maybeComplete();
            return;
        }
        while(nextRequest < pending.size() && active.size() < options.parallelism) {
            const BandcampReleaseRequest request = pending.at(nextRequest);
            const QString identity               = bandcampPageKey(request.page);
            if(cache.contains(identity)) {
                qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
                    releaseRequestContext(request), QStringLiteral("domain_cache_hit"),
                    {DiagnosticField::text(QStringLiteral("scope"), pageKindName(request.page.kind))});
                ++nextRequest;
                const QPointer<BandcampReleaseResolver> guard{owner};
                publish(request.key, ResolutionSuccess<BandcampResolvedData>{cache.value(identity)}, identity);
                if(!guard) {
                    return;
                }
                if(cancelled || completionSent) {
                    return;
                }
                continue;
            }
            if(!workflowSlots->tryAcquire()) {
                break;
            }
            ++nextRequest;
            auto* workflow = new BandcampReleaseWorkflow(
                client, operationId, request,
                BandcampAttemptPolicy{options.attemptTimeoutMs, options.maxAttempts, options.attemptCooldownMs}, parser,
                [this, identity](const QString& key, BandcampResolutionOutcome outcome) {
                    publish(key, std::move(outcome), identity);
                },
                [this](const QString& key) { workflowSettled(key); }, owner);
            active.insert(request.key, workflow);
            const QPointer<BandcampReleaseResolver> guard{owner};
            emit owner->activeCountChanged(active.size());
            if(!guard) {
                return;
            }
            workflow->start();
            if(!guard) {
                return;
            }
            if(cancelled || completionSent) {
                return;
            }
        }
        maybeComplete();
    }

    void publish(const QString& key, BandcampResolutionOutcome outcome, const QString& identity = {})
    {
        if(outcomes.contains(key)) {
            return;
        }
        if(!identity.isEmpty()) {
            if(const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&outcome)) {
                cache.insert(identity, success->value);
            }
        }
        outcomes.insert(key, std::move(outcome));
        const auto it = outcomes.constFind(key);
        if(it != outcomes.cend()) {
            const QPointer<BandcampReleaseResolver> guard{owner};
            ++outcomeDeliveryDepth;
            emit owner->keyedOutcomeReady(key, it.value());
            if(!guard) {
                return;
            }
            --outcomeDeliveryDepth;
        }
        maybeComplete();
    }

    void workflowSettled(const QString& key)
    {
        auto it = active.find(key);
        if(it == active.end()) {
            return;
        }
        BandcampReleaseWorkflow* workflow = it.value();
        active.erase(it);
        if(workflow) {
            workflow->deleteLater();
        }
        const QPointer<BandcampReleaseResolver> guard{owner};
        workflowSlots->release();
        if(!guard) {
            return;
        }
        emit owner->activeCountChanged(active.size());
        if(!guard) {
            return;
        }
        maybeComplete();
        if(!guard) {
            return;
        }
        schedulePump();
    }

    void schedulePump()
    {
        if(pumpQueued || !started || cancelled) {
            return;
        }
        pumpQueued = true;
        QTimer::singleShot(0, owner, [this]() {
            pumpQueued = false;
            pump();
        });
    }

    void maybeComplete()
    {
        if(outcomeDeliveryDepth != 0) {
            return;
        }
        if(started && !completionSent && outcomes.size() == pending.size()) {
            completionSent = true;
            const QPointer<BandcampReleaseResolver> guard{owner};
            emit owner->completed();
            if(!guard) {
                return;
            }
        }
        if(completionSent && active.isEmpty() && !quietSent) {
            quietSent = true;
            const QPointer<BandcampReleaseResolver> guard{owner};
            emit owner->physicallyQuiescent();
        }
    }
};

BandcampReleaseResolver::BandcampReleaseResolver(QNetworkAccessManager* network, BandcampOperationId operationId,
                                                 QVector<BandcampReleaseRequest> requests, OperationOptions options,
                                                 BandcampPageParser parser,
                                                 std::shared_ptr<BandcampWorkflowSlots> workflowSlots, QObject* parent)
    : QObject{parent}
    , m_impl{std::make_unique<Impl>(this, network, operationId, std::move(requests), options, std::move(parser),
                                    std::move(workflowSlots))}
{ }

BandcampReleaseResolver::~BandcampReleaseResolver() = default;

void BandcampReleaseResolver::start()
{
    m_impl->start();
}

void BandcampReleaseResolver::cancel()
{
    m_impl->cancel();
}

bool BandcampReleaseResolver::isComplete() const
{
    return m_impl->completionSent;
}

qsizetype BandcampReleaseResolver::completedOutcomeCount() const
{
    return static_cast<qsizetype>(m_impl->outcomes.size());
}
} // namespace Fooyin::Bandcamp
