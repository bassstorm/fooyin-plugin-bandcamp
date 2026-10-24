#include "bandcampservice.h"

#include "bandcamplogging.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr qsizetype MaxPageUrlBytes = 8192;

ResolutionError submissionError(ResolutionErrorCode code, const QUrl& url, QString field, QString reason)
{
    return {code, ResolutionStage::Acquire, url, 0, std::move(field), std::move(reason)};
}

class BandcampOperation final : public QObject
{
public:
    using ResultHandler   = std::function<void(const BandcampScopeResult&)>;
    using ProgressHandler = std::function<void(const BandcampOperationProgress&)>;
    using FinishedHandler = std::function<void(const BandcampOperationSummary&)>;
    using QuietHandler    = std::function<void()>;

    BandcampOperation(QNetworkAccessManager* network, std::shared_ptr<BandcampWorkflowSlots> workflowSlots,
                      BandcampOperationId id, const QList<BandcampPageUrl>& pages, OperationOptions options,
                      BandcampPageParser parser, ResultHandler resultHandler, ProgressHandler progressHandler,
                      FinishedHandler finishedHandler, QuietHandler quietHandler, QObject* parent)
        : QObject{parent}
        , m_id{id}
        , m_orchestrator{pages}
        , m_resultHandler{std::move(resultHandler)}
        , m_progressHandler{std::move(progressHandler)}
        , m_finishedHandler{std::move(finishedHandler)}
        , m_quietHandler{std::move(quietHandler)}
    {
        m_resolver = new BandcampResolver{
            network, id, m_orchestrator.requests(), options, std::move(parser), std::move(workflowSlots), this};
        QObject::connect(
            m_resolver, &BandcampResolver::keyedOutcomeReady, this,
            [this](const QString& key, const BandcampResolutionOutcome& outcome) { onOutcome(key, outcome); });
        QObject::connect(m_resolver, &BandcampResolver::activeCountChanged, this, [this](qsizetype active) {
            m_active = active;
            publishProgress();
        });
        QObject::connect(m_resolver, &BandcampResolver::completed, this,
                         [this](const QHash<QString, BandcampResolutionOutcome>& outcomes) { onCompleted(outcomes); });
        QObject::connect(m_resolver, &BandcampResolver::physicallyQuiescent, this, [this]() { m_quietHandler(); });
    }

    void scheduleStart()
    {
        const QPointer<BandcampOperation> guard{this};
        QTimer::singleShot(0, this, [guard]() {
            if(guard) {
                guard->publishProgress();
                if(guard) {
                    const std::optional<ResolutionError> rejection = guard->m_resolver->start();
                    if(!guard) {
                        return;
                    }
                    if(rejection) {
                        guard->rejectStart(*rejection);
                    }
                }
            }
        });
    }

    void cancel()
    {
        if(!m_finished) {
            m_resolver->cancel();
        }
    }

private:
    void rejectStart(const ResolutionError& error)
    {
        if(m_finished) {
            return;
        }
        const QPointer<BandcampOperation> guard{this};
        qCWarning(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
            CorrelationContext{
                .jobId = static_cast<qint64>(m_id), .resourceUrl = error.resourceUrl, .trackId = error.trackId},
            QStringLiteral("resolver_start_rejected"),
            {DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error.code)),
             DiagnosticField::text(QStringLiteral("reason"), error.reason)});
        for(const BandcampReleaseRequest& request : m_orchestrator.requests()) {
            ResolutionError scopedError = error;
            scopedError.resourceUrl     = QUrl(request.page.url);
            onOutcome(request.key, ResolutionFailure{std::move(scopedError)});
            if(!guard) {
                return;
            }
        }

        m_finished = true;
        const BandcampOperationSummary summary{m_id, m_orchestrator.occurrenceCount(), m_succeeded, m_failed,
                                               m_cancelled};
        qCInfo(BANDCAMP_RESOLVE) << "Operation" << m_id << "finished:" << summary.succeeded << "succeeded,"
                                 << summary.failed << "failed," << summary.cancelled << "cancelled";
        publishProgress();
        if(!guard) {
            return;
        }
        try {
            m_finishedHandler(summary);
        }
        catch(...) {
            qCWarning(BANDCAMP_RESOLVE) << "An operation completion handler threw";
        }
        if(!guard) {
            return;
        }
        m_quietHandler();
    }

    void onOutcome(const QString& key, const BandcampResolutionOutcome& outcome)
    {
        const QPointer<BandcampOperation> guard{this};
        const QVector<BandcampAssociatedOutcome> ready = m_orchestrator.associate(key, outcome);
        for(const BandcampAssociatedOutcome& associated : ready) {
            BandcampScopeResult result{m_id, associated.ordinal, associated.page, associated.outcome};
            if(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(result.outcome)) {
                ++m_succeeded;
            }
            else if(std::holds_alternative<ResolutionCancelled>(result.outcome)) {
                ++m_cancelled;
            }
            else {
                ++m_failed;
                const auto& error      = std::get<ResolutionFailure>(result.outcome).error;
                const QUrl resourceUrl = error.resourceUrl.isEmpty() ? QUrl(associated.page.url) : error.resourceUrl;
                CorrelationContext context{
                    .jobId = static_cast<qint64>(m_id), .resourceUrl = resourceUrl, .trackId = error.trackId};
                qCWarning(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
                    context, QStringLiteral("scope_failed"),
                    {DiagnosticField::number(QStringLiteral("ordinal"), associated.ordinal),
                     DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error.code)),
                     DiagnosticField::text(QStringLiteral("stage"), resolutionStageName(error.stage)),
                     DiagnosticField::text(QStringLiteral("field"), error.field),
                     DiagnosticField::text(QStringLiteral("reason"), error.reason)});
            }
            try {
                m_resultHandler(result);
            }
            catch(...) {
                qCWarning(BANDCAMP_RESOLVE) << "A scope result handler threw";
            }
            if(!guard) {
                return;
            }
        }
        if(guard) {
            publishProgress();
        }
    }

    void onCompleted(const QHash<QString, BandcampResolutionOutcome>& outcomes)
    {
        const QPointer<BandcampOperation> guard{this};
        if(m_finished) {
            return;
        }
        if(!m_orchestrator.isComplete()) {
            for(const BandcampReleaseRequest& request : m_orchestrator.requests()) {
                const auto it = outcomes.constFind(request.key);
                if(it != outcomes.cend()) {
                    onOutcome(request.key, it.value());
                    if(!guard) {
                        return;
                    }
                }
            }
        }
        m_finished = true;
        const BandcampOperationSummary summary{m_id, m_orchestrator.occurrenceCount(), m_succeeded, m_failed,
                                               m_cancelled};
        qCInfo(BANDCAMP_RESOLVE) << "Operation" << m_id << "finished:" << summary.succeeded << "succeeded,"
                                 << summary.failed << "failed," << summary.cancelled << "cancelled";
        publishProgress();
        if(!guard) {
            return;
        }
        try {
            m_finishedHandler(summary);
        }
        catch(...) {
            qCWarning(BANDCAMP_RESOLVE) << "An operation completion handler threw";
        }
    }

    void publishProgress()
    {
        try {
            m_progressHandler({m_id, m_orchestrator.occurrenceCount(), m_orchestrator.deliveredCount(), m_active});
        }
        catch(...) {
            qCWarning(BANDCAMP_RESOLVE) << "An operation progress handler threw";
        }
    }

    BandcampOperationId m_id{0};
    BandcampOrchestrator m_orchestrator;
    BandcampResolver* m_resolver{nullptr};
    ResultHandler m_resultHandler;
    ProgressHandler m_progressHandler;
    FinishedHandler m_finishedHandler;
    QuietHandler m_quietHandler;
    qsizetype m_succeeded{0};
    qsizetype m_failed{0};
    qsizetype m_cancelled{0};
    qsizetype m_active{0};
    bool m_finished{false};
};
} // namespace

struct BandcampService::Impl
{
    BandcampService* owner{nullptr};
    QNetworkAccessManager* network{nullptr};
    BandcampServiceDependencies dependencies;
    std::shared_ptr<BandcampWorkflowSlots> workflowSlots{std::make_shared<BandcampWorkflowSlots>()};
    QHash<BandcampOperationId, BandcampOperation*> operations;
    BandcampOperationId nextOperationId{1};

    Impl(BandcampService* service, QNetworkAccessManager* networkManager,
         BandcampServiceDependencies serviceDependencies)
        : owner{service}
        , network{networkManager}
        , dependencies{std::move(serviceDependencies)}
    {
        if(!dependencies.parseReleasePage) {
            dependencies.parseReleasePage = [](const QByteArray& body, const QUrl& url, CorrelationContext context,
                                               const std::atomic_bool* stop) {
                return parseReleasePage(body, url, std::move(context), stop);
            };
        }
    }

    BandcampSubmission submit(const QList<BandcampPageUrl>& pages, OperationOptions options)
    {
        if(pages.isEmpty()) {
            return BandcampSubmissionRejected{submissionError(ResolutionErrorCode::UnsupportedRequest, {},
                                                              QStringLiteral("operation"),
                                                              QStringLiteral("operation has no requested scopes"))};
        }
        for(const BandcampPageUrl& page : pages) {
            if(!page.isValid() || page.url.isEmpty() || page.url.size() > MaxPageUrlBytes
               || page.url.toUtf8().size() > MaxPageUrlBytes) {
                return BandcampSubmissionRejected{submissionError(ResolutionErrorCode::UnsupportedRequest, {},
                                                                  QStringLiteral("resourceUrl"),
                                                                  QStringLiteral("invalid or oversized scope URL"))};
            }
            const BandcampPageUrl canonical = parseBandcampPageUrl(page.url);
            if(!canonical.isValid() || canonical.url != page.url || canonical.kind != page.kind) {
                return BandcampSubmissionRejected{submissionError(ResolutionErrorCode::UnsupportedRequest,
                                                                  QUrl(page.url), QStringLiteral("resourceUrl"),
                                                                  QStringLiteral("invalid or oversized scope URL"))};
            }
        }

        while(operations.contains(nextOperationId) || nextOperationId == 0) {
            ++nextOperationId;
        }
        const BandcampOperationId id = nextOperationId++;
        const QPointer<BandcampService> guard{owner};
        auto* operation = new BandcampOperation{network,
                                                workflowSlots,
                                                id,
                                                pages,
                                                options,
                                                dependencies.parseReleasePage,
                                                [guard](const BandcampScopeResult& result) {
                                                    if(guard) {
                                                        emit guard->scopeResolved(result);
                                                    }
                                                },
                                                [guard](const BandcampOperationProgress& progress) {
                                                    if(guard) {
                                                        emit guard->progressChanged(progress);
                                                    }
                                                },
                                                [guard](const BandcampOperationSummary& summary) {
                                                    if(guard) {
                                                        emit guard->operationFinished(summary);
                                                    }
                                                },
                                                [guard, id]() {
                                                    if(guard && guard->m_impl) {
                                                        guard->m_impl->removeOperation(id);
                                                    }
                                                },
                                                owner};
        operations.insert(id, operation);
        operation->scheduleStart();
        return BandcampOperationAccepted{id};
    }

    void cancel(BandcampOperationId id)
    {
        const auto it = operations.constFind(id);
        if(it != operations.cend() && it.value()) {
            it.value()->cancel();
        }
    }

    void removeOperation(BandcampOperationId id)
    {
        auto it = operations.find(id);
        if(it == operations.end()) {
            return;
        }
        BandcampOperation* operation = it.value();
        operations.erase(it);
        if(operation) {
            operation->deleteLater();
        }
    }
};

BandcampService::BandcampService(QNetworkAccessManager* network, QObject* parent,
                                 BandcampServiceDependencies dependencies)
    : QObject{parent}
    , m_impl{std::make_unique<Impl>(this, network, std::move(dependencies))}
{ }

BandcampService::~BandcampService() = default;

BandcampSubmission BandcampService::submit(const QList<BandcampPageUrl>& pages, OperationOptions options)
{
    return m_impl->submit(pages, options);
}

void BandcampService::cancel(BandcampOperationId id)
{
    m_impl->cancel(id);
}
} // namespace Fooyin::Bandcamp
