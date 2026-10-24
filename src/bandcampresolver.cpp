#include "bandcampresolver.h"

#include "bandcampassembly.h"
#include "bandcampclient.h"
#include "bandcamplogging.h"
#include "bandcamppostprocessor.h"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr int MaxPhysicalWorkflows = MaxParallelism;

ResolutionError makeError(ResolutionErrorCode code, ResolutionStage stage, const QUrl& url, qint64 trackId,
                          QString field, QString reason)
{
    return {code, stage, url, trackId, std::move(field), std::move(reason)};
}

QString trackUri(qint64 id)
{
    return id > 0 ? QStringLiteral("bandcamp://track/%1").arg(id) : QString{};
}

QString cacheIdentity(const BandcampPageUrl& page)
{
    return QString::number(static_cast<unsigned int>(page.kind)) + QChar(u':') + page.url;
}

ResolutionError parentFailure(const ResolutionError& cause, const QUrl& parentUrl, qint64 trackId,
                              ResolutionStage stage)
{
    return makeError(ResolutionErrorCode::ParentUnavailable, stage, parentUrl, trackId,
                     cause.field.isEmpty() ? QStringLiteral("parent") : cause.field,
                     QStringLiteral("required album parent failed: %1").arg(cause.reason));
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
            return ResolutionFailure{makeError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
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
        return ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.album)}};
    }

    const auto* track = std::get_if<TrackPageCandidate>(&primary);
    if(!track) {
        return ResolutionFailure{makeError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                           context.resourceUrl, 0, QStringLiteral("identity"),
                                           QStringLiteral("parsed resource is not a track"))};
    }
    if(track->track.id.isUsable()) {
        context.trackId     = track->track.id.value;
        context.resourceUrl = QUrl(trackUri(track->track.id.value));
    }
    const ParentRequirement requirement = classifyParentEvidence(*track, context);
    if(const auto* failure = std::get_if<ResolutionFailure>(&requirement)) {
        return *failure;
    }

    ResolutionOutcome<TrackAdmission> admission = ResolutionCancelled{};
    if(std::holds_alternative<StandaloneParentEvidence>(requirement)) {
        admission = admitStandaloneTrack(*track, context, stop);
    }
    else {
        const auto& required = std::get<RequiredAlbumParent>(requirement);
        if(!parent) {
            return ResolutionFailure{makeError(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire,
                                               required.url, track->track.id.value, QStringLiteral("parent.albumUrl"),
                                               QStringLiteral("required album parent is unavailable"))};
        }
        const auto* album = std::get_if<AlbumCandidate>(parent);
        if(!album) {
            return ResolutionFailure{makeError(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Parse,
                                               required.url, track->track.id.value, QStringLiteral("parent.resource"),
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
    return ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(refined.result)}};
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

QString workflowPhaseName(WorkflowPhase phase)
{
    switch(phase) {
        case WorkflowPhase::Created:
            return QStringLiteral("created");
        case WorkflowPhase::FetchingPrimary:
            return QStringLiteral("fetch_primary");
        case WorkflowPhase::ParsingPrimary:
            return QStringLiteral("parse_primary");
        case WorkflowPhase::FetchingParent:
            return QStringLiteral("fetch_parent");
        case WorkflowPhase::ParsingParent:
            return QStringLiteral("parse_parent");
        case WorkflowPhase::Resolving:
            return QStringLiteral("resolve");
    }
    return QStringLiteral("unknown");
}

class BandcampReleaseWorkflow : public QObject
{
public:
    using OutcomeCallback = std::function<void(const QString&, BandcampResolutionOutcome)>;
    using SettledCallback = std::function<void(const QString&)>;

    BandcampReleaseWorkflow(BandcampClient* client, BandcampOperationId operationId, BandcampReleaseRequest request,
                            int timeoutMs, BandcampPageParser parser, OutcomeCallback outcomeCallback,
                            SettledCallback settledCallback, QObject* parent = nullptr)
        : QObject{parent}
        , m_client{client}
        , m_operationId{operationId}
        , m_request{std::move(request)}
        , m_timeoutMs{timeoutMs}
        , m_parser{std::move(parser)}
        , m_outcomeCallback{std::move(outcomeCallback)}
        , m_settledCallback{std::move(settledCallback)}
    {
        m_context.jobId       = static_cast<qint64>(m_operationId);
        m_context.resourceUrl = QUrl(m_request.page.url);
        m_deadline.setSingleShot(true);
        QObject::connect(&m_deadline, &QTimer::timeout, this, [this]() { onDeadline(); });
    }

    ~BandcampReleaseWorkflow() override
    {
        m_stop->store(true, std::memory_order_relaxed);
        if(m_fetchActive && m_client && m_requestId != 0) {
            m_client->cancel(m_requestId);
        }
    }

    void start()
    {
        m_deadline.start(m_timeoutMs);
        if(m_request.page.kind == BandcampPageKind::Discography) {
            const QPointer<BandcampReleaseWorkflow> guard{this};
            if(!report(ResolutionFailure{makeError(ResolutionErrorCode::UnsupportedRequest, ResolutionStage::Acquire,
                                                   QUrl(m_request.page.url), 0, QStringLiteral("resource"),
                                                   QStringLiteral("discography expansion is not implemented"))})
               || !guard) {
                return;
            }
            settleIfIdle();
            return;
        }
        fetchPage(m_request.page, false);
    }

    void cancel()
    {
        if(m_reported) {
            return;
        }
        m_stop->store(true, std::memory_order_relaxed);
        trace(QStringLiteral("workflow_cancelled"));
        const QPointer<BandcampReleaseWorkflow> guard{this};
        if(m_fetchActive && m_client && m_requestId != 0) {
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
        const QUrl url{page.url};
        m_currentUrl          = url;
        m_context.resourceUrl = url;
        setPhase(parent ? WorkflowPhase::FetchingParent : WorkflowPhase::FetchingPrimary);
        m_fetchActive = true;
        const QPointer<BandcampReleaseWorkflow> guard{this};
        m_requestId = m_client->fetch(
            url,
            [guard, parent](BandcampFetchOutcome outcome) mutable {
                if(guard) {
                    guard->onFetch(parent, std::move(outcome));
                }
            },
            m_context);
        if(!parent) {
            m_primaryRequestId = m_requestId;
        }
    }

    void onFetch(bool parent, BandcampFetchOutcome outcome)
    {
        m_fetchActive                                    = false;
        const BandcampClient::RequestId completedRequest = m_requestId;
        m_requestId                                      = 0;
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
        if(parent) {
            m_context.requestId = static_cast<qint64>(completedRequest);
            setPhase(WorkflowPhase::ParsingParent);
        }
        else {
            m_context.requestId = static_cast<qint64>(m_primaryRequestId);
            setPhase(WorkflowPhase::ParsingPrimary);
        }
        parsePage(std::move(response.body), response.url, parent);
    }

    void parsePage(QByteArray body, QUrl effectiveUrl, bool parent)
    {
        m_workerActive = true;
        auto* watcher  = new QFutureWatcher<ReleaseParseOutcome>(this);
        const QPointer<BandcampReleaseWorkflow> guard{this};
        QObject::connect(watcher, &QFutureWatcherBase::finished, this, [guard, watcher, parent, effectiveUrl]() {
            if(!guard) {
                watcher->deleteLater();
                return;
            }
            ReleaseParseOutcome outcome = ResolutionCancelled{};
            try {
                outcome = watcher->result();
            }
            catch(...) {
                outcome = ResolutionFailure{makeError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                      effectiveUrl, 0, QStringLiteral("parser"),
                                                      QStringLiteral("parser dependency failed"))};
            }
            watcher->deleteLater();
            watcher->setParent(nullptr);
            guard->m_workerActive = false;
            guard->onParsed(parent, effectiveUrl, std::move(outcome));
            if(!guard) {
                return;
            }
            guard->settleIfIdle();
        });

        const CorrelationContext context = m_context;
        const auto stop                  = m_stop;
        const auto parser                = m_parser;
        try {
            watcher->setFuture(QtConcurrent::run(
                [body = std::move(body), effectiveUrl, context, stop, parser]() mutable -> ReleaseParseOutcome {
                    try {
                        ReleaseParseOutcome parsed = parser(body, effectiveUrl, context, stop.get());
                        if(stop->load(std::memory_order_relaxed)) {
                            return ResolutionCancelled{};
                        }
                        return parsed;
                    }
                    catch(...) {
                        return ResolutionFailure{makeError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
                                                           effectiveUrl, 0, QStringLiteral("parser"),
                                                           QStringLiteral("parser dependency failed"))};
                    }
                }));
        }
        catch(...) {
            watcher->deleteLater();
            watcher->setParent(nullptr);
            m_workerActive = false;
            const QPointer<BandcampReleaseWorkflow> guard{this};
            onParsed(parent, effectiveUrl,
                     ResolutionFailure{makeError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse,
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
                report(ResolutionFailure{makeError(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Parse,
                                                   effectiveUrl, primaryTrackId(), QStringLiteral("parent.resource"),
                                                   QStringLiteral("parent response did not identify an album"))});
                return;
            }
            m_parentCandidate = std::move(candidate);
            resolve();
            return;
        }

        if(m_request.page.kind == BandcampPageKind::Album) {
            m_primaryCandidate = std::move(candidate);
            resolve();
            return;
        }
        const auto* track = std::get_if<TrackPageCandidate>(&candidate);
        if(!track) {
            report(ResolutionFailure{makeError(ResolutionErrorCode::MalformedPage, ResolutionStage::Parse, effectiveUrl,
                                               0, QStringLiteral("identity"),
                                               QStringLiteral("parsed resource is not a track"))});
            return;
        }
        if(track->track.id.isUsable()) {
            m_context.trackId     = track->track.id.value;
            m_context.resourceUrl = QUrl(trackUri(track->track.id.value));
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

        const auto& required             = std::get<RequiredAlbumParent>(requirement);
        const BandcampPageUrl parentPage = parseBandcampPageUrl(required.url.toString(QUrl::FullyEncoded));
        if(!parentPage.isValid() || parentPage.kind != BandcampPageKind::Album) {
            report(ResolutionFailure{makeError(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire,
                                               required.url, primaryTrackId(), QStringLiteral("parent.albumUrl"),
                                               QStringLiteral("required parent location is not an album URL"))});
            return;
        }
        fetchPage(parentPage, true);
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
            context.resourceUrl = QUrl(trackUri(primaryTrackId()));
            context.trackId     = primaryTrackId();
        }
        m_currentUrl = context.resourceUrl;

        m_workerActive = true;
        auto* watcher  = new QFutureWatcher<BandcampResolutionOutcome>(this);
        const QPointer<BandcampReleaseWorkflow> guard{this};
        QObject::connect(watcher, &QFutureWatcherBase::finished, this, [guard, watcher]() {
            if(!guard) {
                watcher->deleteLater();
                return;
            }
            BandcampResolutionOutcome outcome = ResolutionCancelled{};
            try {
                outcome = watcher->result();
            }
            catch(...) {
                outcome = ResolutionFailure{makeError(ResolutionErrorCode::InvalidField, ResolutionStage::Refine,
                                                      guard->m_context.resourceUrl, guard->primaryTrackId(),
                                                      QStringLiteral("processor"),
                                                      QStringLiteral("resolution dependency failed"))};
            }
            watcher->deleteLater();
            watcher->setParent(nullptr);
            guard->m_workerActive = false;
            if(!guard->report(std::move(outcome)) || !guard) {
                return;
            }
            guard->settleIfIdle();
        });

        const BandcampPageUrl page = m_request.page;
        const auto stop            = m_stop;
        const bool needsParent     = parent.has_value();
        const QUrl resolutionUrl   = context.resourceUrl;
        const qint64 trackId       = context.trackId;
        try {
            watcher->setFuture(QtConcurrent::run([page, primary = std::move(primary), parent = std::move(parent),
                                                  context, stop, needsParent, resolutionUrl, trackId]() mutable {
                try {
                    if(stop->load(std::memory_order_relaxed)) {
                        return BandcampResolutionOutcome{ResolutionCancelled{}};
                    }
                    BandcampResolutionOutcome outcome
                        = resolveScope(page, primary, needsParent ? &*parent : nullptr, context, stop.get());
                    if(stop->load(std::memory_order_relaxed)) {
                        return BandcampResolutionOutcome{ResolutionCancelled{}};
                    }
                    return outcome;
                }
                catch(...) {
                    return BandcampResolutionOutcome{ResolutionFailure{
                        makeError(ResolutionErrorCode::InvalidField, ResolutionStage::Refine, resolutionUrl, trackId,
                                  QStringLiteral("processor"), QStringLiteral("resolution dependency failed"))}};
                }
            }));
        }
        catch(...) {
            watcher->deleteLater();
            watcher->setParent(nullptr);
            m_workerActive = false;
            if(report(ResolutionFailure{makeError(ResolutionErrorCode::InvalidField, ResolutionStage::Refine,
                                                  context.resourceUrl, context.trackId, QStringLiteral("processor"),
                                                  QStringLiteral("resolution worker could not be started"))})) {
                settleIfIdle();
            }
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

    void onDeadline()
    {
        if(m_reported) {
            return;
        }
        m_stop->store(true, std::memory_order_relaxed);
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
        const qint64 trackId = primaryTrackId();
        trace(QStringLiteral("workflow_timeout"),
              {DiagnosticField::text(QStringLiteral("stage"), resolutionStageName(stage))});
        const QPointer<BandcampReleaseWorkflow> guard{this};
        if(m_fetchActive && m_client && m_requestId != 0) {
            m_client->cancel(m_requestId);
        }
        if(!guard) {
            return;
        }
        if(report(ResolutionFailure{makeError(ResolutionErrorCode::Timeout, stage, m_currentUrl, trackId,
                                              QStringLiteral("resource"), QStringLiteral("item deadline expired"))})) {
            settleIfIdle();
        }
    }

    bool report(BandcampResolutionOutcome outcome)
    {
        if(m_reported) {
            return true;
        }
        m_reported = true;
        m_deadline.stop();
        CorrelationContext context = m_context;
        QList<DiagnosticField> fields;
        if(std::holds_alternative<ResolutionCancelled>(outcome)) {
            fields.append(DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("cancelled")));
        }
        else if(const auto* failure = std::get_if<ResolutionFailure>(&outcome)) {
            context.trackId = failure->error.trackId;
            fields.append(DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("failure")));
            fields.append(DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(failure->error.code)));
            fields.append(DiagnosticField::text(QStringLiteral("stage"), resolutionStageName(failure->error.stage)));
        }
        else {
            fields.append(DiagnosticField::text(QStringLiteral("outcome"), QStringLiteral("success")));
        }
        qCDebug(BANDCAMP_TRACE_RESOLVE).noquote()
            << formatTraceEvent(context, QStringLiteral("workflow_outcome"), fields);
        const QPointer<BandcampReleaseWorkflow> guard{this};
        m_outcomeCallback(m_request.key, std::move(outcome));
        return guard;
    }

    void settleIfIdle()
    {
        if(m_settled || m_fetchActive || m_workerActive) {
            return;
        }
        if(!m_reported) {
            if(!report(ResolutionFailure{makeError(ResolutionErrorCode::FetchFailed, ResolutionStage::Fetch,
                                                   m_currentUrl, primaryTrackId(), QStringLiteral("workflow"),
                                                   QStringLiteral("release workflow ended without an outcome"))})) {
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
        if(m_phase == phase) {
            return;
        }
        m_phase = phase;
        trace(QStringLiteral("workflow_stage"),
              {DiagnosticField::text(QStringLiteral("stage"), workflowPhaseName(phase))});
    }

    void trace(const QString& event, const QList<DiagnosticField>& fields = {}) const
    {
        qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(m_context, event, fields);
    }

    QPointer<BandcampClient> m_client;
    BandcampOperationId m_operationId{0};
    BandcampReleaseRequest m_request;
    int m_timeoutMs{0};
    BandcampPageParser m_parser;
    OutcomeCallback m_outcomeCallback;
    SettledCallback m_settledCallback;
    QTimer m_deadline;
    std::shared_ptr<std::atomic_bool> m_stop{std::make_shared<std::atomic_bool>(false)};
    CorrelationContext m_context;
    std::optional<ReleaseCandidate> m_primaryCandidate;
    std::optional<ReleaseCandidate> m_parentCandidate;
    QUrl m_currentUrl;
    BandcampClient::RequestId m_requestId{0};
    BandcampClient::RequestId m_primaryRequestId{0};
    WorkflowPhase m_phase{WorkflowPhase::Created};
    bool m_fetchActive{false};
    bool m_workerActive{false};
    bool m_reported{false};
    bool m_settled{false};
};
} // namespace

BandcampWorkflowSlots::BandcampWorkflowSlots(QObject* parent)
    : QObject{parent}
{ }

bool BandcampWorkflowSlots::tryAcquire()
{
    if(m_active >= MaxPhysicalWorkflows) {
        return false;
    }
    ++m_active;
    return true;
}

void BandcampWorkflowSlots::release()
{
    if(m_active <= 0) {
        return;
    }
    --m_active;
    emit capacityAvailable();
}

struct BandcampResolver::Impl
{
    BandcampResolver* owner{nullptr};
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

    Impl(BandcampResolver* resolver, QNetworkAccessManager* network, BandcampOperationId id,
         QVector<BandcampReleaseRequest> requests, OperationOptions operationOptions, BandcampPageParser pageParser,
         std::shared_ptr<BandcampWorkflowSlots> capacity)
        : owner{resolver}
        , client{new BandcampClient(network, resolver)}
        , operationId{id}
        , pending{std::move(requests)}
        , options{clampOperationOptions(operationOptions)}
        , parser{std::move(pageParser)}
        , workflowSlots{std::move(capacity)}
    {
        QObject::connect(workflowSlots.get(), &BandcampWorkflowSlots::capacityAvailable, owner,
                         [this]() { schedulePump(); });
    }

    std::optional<ResolutionError> start()
    {
        if(started) {
            return std::nullopt;
        }
        QSet<QString> requestKeys;
        for(const BandcampReleaseRequest& request : pending) {
            if(request.key.isEmpty() || requestKeys.contains(request.key)) {
                return makeError(ResolutionErrorCode::UnsupportedRequest, ResolutionStage::Acquire,
                                 QUrl(request.page.url), 0, QStringLiteral("requestKey"),
                                 QStringLiteral("resolver request keys must be non-empty and unique"));
            }
            requestKeys.insert(request.key);
        }
        started = true;
        const QPointer<BandcampResolver> guard{owner};
        pump();
        if(!guard) {
            return std::nullopt;
        }
        maybeComplete();
        return std::nullopt;
    }

    void cancel()
    {
        if(cancelled || completionSent) {
            return;
        }
        cancelled = true;
        const QPointer<BandcampResolver> guard{owner};
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
            const QString identity               = cacheIdentity(request.page);
            if(cache.contains(identity)) {
                qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
                    CorrelationContext{.jobId       = static_cast<qint64>(operationId),
                                       .resourceUrl = QUrl(request.page.url)},
                    QStringLiteral("domain_cache_hit"),
                    {DiagnosticField::text(QStringLiteral("scope"), pageKindName(request.page.kind))});
                ++nextRequest;
                const QPointer<BandcampResolver> guard{owner};
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
                client, operationId, request, options.itemTimeoutMs, parser,
                [this, identity](const QString& key, BandcampResolutionOutcome outcome) {
                    publish(key, std::move(outcome), identity);
                },
                [this](const QString& key) { workflowSettled(key); }, owner);
            active.insert(request.key, workflow);
            const QPointer<BandcampResolver> guard{owner};
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
            const QPointer<BandcampResolver> guard{owner};
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
        const QPointer<BandcampResolver> guard{owner};
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
            const QPointer<BandcampResolver> guard{owner};
            emit owner->completed(outcomes);
            if(!guard) {
                return;
            }
        }
        if(completionSent && active.isEmpty() && !quietSent) {
            quietSent = true;
            const QPointer<BandcampResolver> guard{owner};
            emit owner->physicallyQuiescent();
        }
    }
};

BandcampResolver::BandcampResolver(QNetworkAccessManager* network, BandcampOperationId operationId,
                                   QVector<BandcampReleaseRequest> requests, OperationOptions options,
                                   BandcampPageParser parser, std::shared_ptr<BandcampWorkflowSlots> workflowSlots,
                                   QObject* parent)
    : QObject{parent}
    , m_impl{std::make_unique<Impl>(this, network, operationId, std::move(requests), options, std::move(parser),
                                    std::move(workflowSlots))}
{ }

BandcampResolver::~BandcampResolver() = default;

std::optional<ResolutionError> BandcampResolver::start()
{
    return m_impl->start();
}

void BandcampResolver::cancel()
{
    m_impl->cancel();
}

bool BandcampResolver::isComplete() const
{
    return m_impl->completionSent;
}
} // namespace Fooyin::Bandcamp
