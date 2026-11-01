#include "bandcampservice.h"

#include "bandcamplogging.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <limits>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr qsizetype MaxPageUrlBytes = 8192;

ResolutionError submissionError(ResolutionErrorCode code, const QUrl& url, QString field, QString reason)
{
    return {code, ResolutionStage::Acquire, url, 0, std::move(field), std::move(reason)};
}

QString listingPageKey(QUrl url)
{
    url.setFragment(QString());
    url = url.adjusted(QUrl::NormalizePathSegments);
    return url.toString(QUrl::FullyEncoded);
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
                      BandcampPageParser releaseParser, BandcampListingParser listingParser,
                      ResultHandler resultHandler, ProgressHandler progressHandler, FinishedHandler finishedHandler,
                      QuietHandler quietHandler, QObject* parent)
        : QObject{parent}
        , m_network{network}
        , m_workflowSlots{std::move(workflowSlots)}
        , m_id{id}
        , m_inputPages{pages}
        , m_options{clampOperationOptions(options)}
        , m_releaseParser{std::move(releaseParser)}
        , m_listingParser{std::move(listingParser)}
        , m_orchestrator{pages, m_options.discographyOrder}
        , m_resultHandler{std::move(resultHandler)}
        , m_progressHandler{std::move(progressHandler)}
        , m_finishedHandler{std::move(finishedHandler)}
        , m_quietHandler{std::move(quietHandler)}
    {
        if(m_orchestrator.isDiscoveryComplete() && !m_orchestrator.requests().isEmpty()) {
            createResolver();
        }
    }

    void scheduleStart()
    {
        const QPointer<BandcampOperation> guard{this};
        QTimer::singleShot(0, this, [guard]() {
            if(guard) {
                guard->publishProgress();
                if(guard) {
                    guard->startDiscovery();
                }
            }
        });
    }

    void cancel()
    {
        if(m_finished || m_operationCancelled) {
            return;
        }
        m_operationCancelled = true;
        const QPointer<BandcampOperation> guard{this};
        if(m_resolverStarted && m_resolver) {
            m_resolver->cancel();
        }
        else {
            deliverReady(m_orchestrator.cancelPending());
            if(!guard) {
                return;
            }
            const auto listingResolvers = m_listingResolvers.values();
            for(BandcampListingResolver* resolver : listingResolvers) {
                if(resolver) {
                    resolver->cancel();
                    if(!guard) {
                        return;
                    }
                }
            }
            finishIfReady();
        }
    }

private:
    void startDiscovery()
    {
        if(m_operationCancelled || m_finished) {
            return;
        }
        while(m_nextInput < m_inputPages.size() && m_inputPages.at(m_nextInput).kind != BandcampPageKind::Discography) {
            ++m_nextInput;
        }
        if(m_nextInput >= m_inputPages.size()) {
            startResolver();
            return;
        }
        m_activeDiscoveryInput = m_nextInput++;
        m_seenListingPages.clear();
        m_discoveredReferences = 0;
        m_activeListingReferences.clear();
        startListing(QUrl(m_inputPages.at(m_activeDiscoveryInput).url));
    }

    void startListing(const QUrl& url)
    {
        if(m_operationCancelled || m_finished) {
            return;
        }
        const QString key = listingPageKey(url);
        if(key.isEmpty() || m_seenListingPages.contains(key)) {
            const QPointer<BandcampOperation> guard{this};
            failActiveDiscovery(ResolutionFailure{ResolutionError{
                ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Acquire, url, 0,
                QStringLiteral("listing.continuation"), QStringLiteral("discography continuation repeated a page")}});
            if(guard) {
                startDiscovery();
            }
            return;
        }
        m_seenListingPages.insert(key);
        const auto id  = static_cast<BandcampOperationId>(m_id);
        auto* resolver = new BandcampListingResolver{
            m_network,
            id,
            url,
            m_options.itemTimeoutMs,
            m_listingParser,
            m_workflowSlots,
            [this, input = m_activeDiscoveryInput, requestedUrl = url](BandcampListingParseOutcome outcome) {
                onListingOutcome(input, requestedUrl, std::move(outcome));
            },
            this};
        m_listingResolvers.insert(resolver);
        m_currentListingResolver = resolver;
        const QPointer<BandcampOperation> guard{this};
        QObject::connect(resolver, &BandcampListingResolver::physicallyQuiescent, this, [guard, resolver]() {
            if(!guard) {
                return;
            }
            guard->m_listingResolvers.remove(resolver);
            if(guard->m_currentListingResolver == resolver) {
                guard->m_currentListingResolver = nullptr;
            }
            resolver->deleteLater();
            guard->notifyQuiescent();
            if(guard) {
                guard->publishProgress();
            }
        });
        resolver->start();
        if(guard) {
            publishProgress();
        }
    }

    void onListingOutcome(qsizetype inputOrdinal, const QUrl& requestedUrl, BandcampListingParseOutcome outcome)
    {
        if(m_operationCancelled || m_finished || inputOrdinal != m_activeDiscoveryInput) {
            return;
        }
        const QPointer<BandcampOperation> guard{this};
        if(const auto* failure = std::get_if<ResolutionFailure>(&outcome)) {
            failActiveDiscovery(*failure);
            if(guard) {
                startDiscovery();
            }
            return;
        }
        if(std::holds_alternative<ResolutionCancelled>(outcome)) {
            failActiveDiscovery(ResolutionCancelled{});
            if(guard) {
                startDiscovery();
            }
            return;
        }

        BandcampListingBatch batch = std::get<ResolutionSuccess<BandcampListingBatch>>(std::move(outcome)).value;
        const QUrl effectiveUrl    = batch.effectiveUrl.isValid() ? batch.effectiveUrl : requestedUrl;
        const QString effectiveKey = listingPageKey(effectiveUrl);
        const QString requestedKey = listingPageKey(requestedUrl);
        if(effectiveKey != requestedKey) {
            if(m_seenListingPages.contains(effectiveKey)) {
                failActiveDiscovery(ResolutionFailure{
                    ResolutionError{ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Acquire, effectiveUrl,
                                    0, QStringLiteral("listing.continuation"),
                                    QStringLiteral("discography redirect repeated a previously visited page")}});
                if(guard) {
                    startDiscovery();
                }
                return;
            }
            m_seenListingPages.insert(effectiveKey);
        }
        if(batch.references.size() > std::numeric_limits<qsizetype>::max() - m_discoveredReferences) {
            failActiveDiscovery(ResolutionFailure{ResolutionError{
                ResolutionErrorCode::ResourceLimitExceeded, ResolutionStage::Parse, effectiveUrl, 0,
                QStringLiteral("listing.ordinal"), QStringLiteral("discography listing ordinal overflow")}});
            if(guard) {
                startDiscovery();
            }
            return;
        }
        for(BandcampListingReference reference : std::as_const(batch.references)) {
            reference.ordinal = m_discoveredReferences++;
            m_activeListingReferences.append(std::move(reference));
        }
        if(batch.continuation) {
            startListing(batch.continuation->locator);
            return;
        }
        BandcampListingBatch completeBatch;
        completeBatch.references = std::move(m_activeListingReferences);
        m_orchestrator.appendListingBatch(inputOrdinal, completeBatch);
        deliverReady(m_orchestrator.completeDiscography(inputOrdinal));
        if(guard) {
            m_activeDiscoveryInput = -1;
            publishProgress();
            if(guard) {
                startDiscovery();
            }
        }
    }

    void failActiveDiscovery(const BandcampResolutionOutcome& outcome)
    {
        if(m_activeDiscoveryInput < 0) {
            return;
        }
        const qsizetype failedInput = m_activeDiscoveryInput;
        m_activeDiscoveryInput      = -1;
        m_seenListingPages.clear();
        m_discoveredReferences = 0;
        m_activeListingReferences.clear();
        deliverReady(m_orchestrator.failDiscography(failedInput, outcome));
    }

    void startResolver()
    {
        if(m_resolverStarted || m_finished || m_operationCancelled || !m_orchestrator.isDiscoveryComplete()) {
            return;
        }
        if(m_orchestrator.requests().isEmpty()) {
            finishIfReady();
            return;
        }

        m_resolverStarted   = true;
        m_resolverQuiescent = false;
        createResolver();
        const QPointer<BandcampOperation> guard{this};
        const std::optional<ResolutionError> rejection = m_resolver->start();
        if(!guard) {
            return;
        }
        if(rejection) {
            m_resolverQuiescent = true;
            rejectStart(*rejection);
        }
    }

    void createResolver()
    {
        if(m_resolver) {
            return;
        }
        m_resolver = new BandcampResolver{m_network,       m_id, m_orchestrator.requests(), m_options, m_releaseParser,
                                          m_workflowSlots, this};
        QObject::connect(
            m_resolver, &BandcampResolver::keyedOutcomeReady, this,
            [this](const QString& key, const BandcampResolutionOutcome& outcome) {
                const QString keyCopy = key;
                const BandcampResolutionOutcome outcomeCopy = outcome;
                const QPointer<BandcampOperation> guard{this};
                publishProgress();
                if(guard) {
                    guard->onReleaseOutcome(keyCopy, outcomeCopy);
                }
            });
        QObject::connect(m_resolver, &BandcampResolver::activeCountChanged, this, [this](qsizetype active) {
            m_active = active;
            publishProgress();
        });
        QObject::connect(
            m_resolver, &BandcampResolver::completed, this,
            [this](const QHash<QString, BandcampResolutionOutcome>& outcomes) { onResolverCompleted(outcomes); });
        QObject::connect(m_resolver, &BandcampResolver::physicallyQuiescent, this, [this]() {
            m_resolverQuiescent = true;
            notifyQuiescent();
        });
    }

    void rejectStart(const ResolutionError& error)
    {
        if(m_finished) {
            return;
        }
        qCWarning(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
            CorrelationContext{
                .jobId = static_cast<qint64>(m_id), .resourceUrl = error.resourceUrl, .trackId = error.trackId},
            QStringLiteral("resolver_start_rejected"),
            {DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error.code)),
             DiagnosticField::text(QStringLiteral("reason"), error.reason)});
        const QPointer<BandcampOperation> guard{this};
        for(const BandcampReleaseRequest& request : m_orchestrator.requests()) {
            ResolutionError scopedError = error;
            scopedError.resourceUrl     = QUrl(request.page.url);
            onReleaseOutcome(request.key, ResolutionFailure{std::move(scopedError)});
            if(!guard) {
                return;
            }
        }
        m_resolverCompleted = true;
        finishIfReady();
    }

    void onReleaseOutcome(const QString& key, const BandcampResolutionOutcome& outcome)
    {
        deliverReady(m_orchestrator.associate(key, outcome));
    }

    void onResolverCompleted(const QHash<QString, BandcampResolutionOutcome>& outcomes)
    {
        if(m_resolverCompleted || m_finished) {
            return;
        }
        const QPointer<BandcampOperation> guard{this};
        for(const BandcampReleaseRequest& request : m_orchestrator.requests()) {
            const auto it = outcomes.constFind(request.key);
            if(it != outcomes.cend()) {
                onReleaseOutcome(request.key, it.value());
            }
            else {
                onReleaseOutcome(request.key, ResolutionFailure{ResolutionError{
                                                  ResolutionErrorCode::FetchFailed, ResolutionStage::Publish,
                                                  QUrl(request.page.url), 0, QStringLiteral("resolver.outcome"),
                                                  QStringLiteral("resolver omitted a requested outcome")}});
            }
            if(!guard) {
                return;
            }
        }
        m_resolverCompleted = true;
        finishIfReady();
    }

    void deliverReady(const QVector<BandcampAssociatedOutcome>& ready)
    {
        if(ready.isEmpty()) {
            return;
        }
        const QPointer<BandcampOperation> guard{this};
        ++m_outcomeDeliveryDepth;
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
                const CorrelationContext context{
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
        --m_outcomeDeliveryDepth;
        publishProgress();
        if(guard) {
            finishIfReady();
        }
    }

    void finishIfReady()
    {
        if(m_finished || m_outcomeDeliveryDepth != 0 || !m_orchestrator.isComplete()) {
            return;
        }
        if(m_resolverStarted && !m_resolverCompleted) {
            return;
        }
        m_finished = true;
        const BandcampOperationSummary summary{m_id, m_orchestrator.occurrenceCount(), m_succeeded, m_failed,
                                               m_cancelled};
        qCInfo(BANDCAMP_RESOLVE) << "Operation" << m_id << "finished:" << summary.succeeded << "succeeded,"
                                 << summary.failed << "failed," << summary.cancelled << "cancelled";
        const QPointer<BandcampOperation> guard{this};
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
        if(guard) {
            notifyQuiescent();
        }
    }

    void publishProgress()
    {
        try {
            const qsizetype active = m_active + (m_currentListingResolver ? 1 : 0);
            const BandcampOperationPhase phase
                = m_finished ? BandcampOperationPhase::Finished
                             : (m_orchestrator.isDiscoveryComplete() ? BandcampOperationPhase::Resolving
                                                                    : BandcampOperationPhase::Discovering);
            const qsizetype totalWork = m_orchestrator.isDiscoveryComplete() ? m_orchestrator.requests().size() : 0;
            const qsizetype completedWork = m_resolver ? m_resolver->completedOutcomeCount() : 0;
            m_progressHandler({m_id, m_orchestrator.occurrenceCount(), m_orchestrator.deliveredCount(), active,
                               phase, completedWork, totalWork});
        }
        catch(...) {
            qCWarning(BANDCAMP_RESOLVE) << "An operation progress handler threw";
        }
    }

    void notifyQuiescent()
    {
        if(!m_finished || m_quietSent || !m_listingResolvers.isEmpty() || !m_resolverQuiescent) {
            return;
        }
        m_quietSent = true;
        try {
            m_quietHandler();
        }
        catch(...) {
            qCWarning(BANDCAMP_RESOLVE) << "An operation cleanup handler threw";
        }
    }

    QPointer<QNetworkAccessManager> m_network;
    std::shared_ptr<BandcampWorkflowSlots> m_workflowSlots;
    BandcampOperationId m_id{0};
    QList<BandcampPageUrl> m_inputPages;
    OperationOptions m_options;
    BandcampPageParser m_releaseParser;
    BandcampListingParser m_listingParser;
    BandcampOrchestrator m_orchestrator;
    BandcampResolver* m_resolver{nullptr};
    QSet<BandcampListingResolver*> m_listingResolvers;
    BandcampListingResolver* m_currentListingResolver{nullptr};
    QSet<QString> m_seenListingPages;
    QList<BandcampListingReference> m_activeListingReferences;
    ResultHandler m_resultHandler;
    ProgressHandler m_progressHandler;
    FinishedHandler m_finishedHandler;
    QuietHandler m_quietHandler;
    qsizetype m_nextInput{0};
    qsizetype m_activeDiscoveryInput{-1};
    qsizetype m_discoveredReferences{0};
    qsizetype m_outcomeDeliveryDepth{0};
    qsizetype m_succeeded{0};
    qsizetype m_failed{0};
    qsizetype m_cancelled{0};
    qsizetype m_active{0};
    bool m_resolverStarted{false};
    bool m_resolverCompleted{false};
    bool m_resolverQuiescent{true};
    bool m_operationCancelled{false};
    bool m_finished{false};
    bool m_quietSent{false};
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
        if(!dependencies.parseDiscographyPage) {
            dependencies.parseDiscographyPage = [](const QByteArray& body, const QUrl& url, CorrelationContext context,
                                                   const std::atomic_bool* stop) {
                return parseDiscographyPage(body, url, std::move(context), stop);
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
                                                dependencies.parseDiscographyPage,
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
