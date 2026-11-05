#include "bandcamplistingresolver.h"

#include "bandcampattemptsupervisor.h"
#include "bandcampclient.h"
#include "bandcampdiagnostics.h"
#include "bandcamplogging.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QtConcurrentRun>

#include <utility>

namespace Fooyin::Bandcamp {
struct BandcampListingResolver::Impl
{
    BandcampListingResolver* owner{nullptr};
    BandcampClient* client{nullptr};
    BandcampOperationId operationId{0};
    QUrl pageUrl;
    BandcampListingParser parser;
    std::shared_ptr<BandcampWorkflowSlots> workflowSlots;
    OutcomeCallback callback;
    BandcampAttemptSupervisor attempts;
    std::shared_ptr<std::atomic_bool> stop{std::make_shared<std::atomic_bool>(false)};
    CorrelationContext context;
    BandcampClient::RequestId requestId{0};
    int lastHttpStatus{0};
    QElapsedTimer elapsed;
    bool started{false};
    bool slotHeld{false};
    bool workerActive{false};
    bool reported{false};
    bool settled{false};

    Impl(BandcampListingResolver* resolver, QNetworkAccessManager* network, BandcampOperationId id, QUrl url,
         BandcampAttemptPolicy policy, BandcampListingParser pageParser,
         std::shared_ptr<BandcampWorkflowSlots> capacity, OutcomeCallback outcomeCallback)
        : owner{resolver}
        , client{new BandcampClient(network, resolver)}
        , operationId{id}
        , pageUrl{std::move(url)}
        , parser{std::move(pageParser)}
        , workflowSlots{std::move(capacity)}
        , callback{std::move(outcomeCallback)}
        , attempts{policy}
    {
        context.resourceUrl = pageUrl;
        QObject::connect(&attempts, &BandcampAttemptSupervisor::attemptTimedOut, owner,
                         [this]() { onAttemptTimedOut(); });
        QObject::connect(&attempts, &BandcampAttemptSupervisor::retryReady, owner, [this]() { issueFetch(); });
        QObject::connect(workflowSlots.get(), &BandcampWorkflowSlots::capacityAvailable, owner,
                         [this]() { tryStart(); });
    }

    void start()
    {
        if(started || reported) {
            return;
        }
        started = true;
        elapsed.start();
        tryStart();
    }

    void tryStart()
    {
        if(!started || reported || slotHeld) {
            return;
        }
        if(!workflowSlots || !workflowSlots->tryAcquire()) {
            return;
        }
        slotHeld = true;
        attempts.resetBudget();
        issueFetch();
    }

    // Each attempt gets its own deadline, so a hung request is abandoned and retried like any other failure.
    void issueFetch()
    {
        const quint64 id = attempts.issueAttempt();
        requestId
            = client->fetch(pageUrl, [this, id](BandcampFetchOutcome outcome) { onFetch(id, std::move(outcome)); },
                            context);
    }

    void scheduleRetry(const ResolutionError& error, int httpStatus)
    {
        attempts.beginRetry();
        logResolutionRetry(context, error, httpStatus, attempts);
    }

    // Detaches the in-flight attempt: its late callback is stale, and the client cancels the underlying reply.
    void abandonFetch()
    {
        const BandcampClient::RequestId hung = requestId;
        requestId                            = 0;
        attempts.abandon();
        if(hung != 0) {
            client->cancel(hung);
        }
    }

    void cancel()
    {
        if(reported || settled) {
            return;
        }
        stop->store(true, std::memory_order_relaxed);
        const QPointer<BandcampListingResolver> guard{owner};
        report(ResolutionCancelled{});
        if(!guard) {
            return;
        }
        if(attempts.attemptActive() && requestId != 0) {
            client->cancel(requestId);
        }
        settleIfIdle();
    }

    void onAttemptTimedOut()
    {
        if(reported || settled) {
            return;
        }
        if(attempts.attemptActive() && attempts.canRetry()) {
            abandonFetch();
            scheduleRetry(makeResolutionError(ResolutionErrorCode::Timeout, ResolutionStage::Fetch, pageUrl, 0,
                                              QString{}, QStringLiteral("attempt timed out")),
                          0);
            return;
        }
        stop->store(true, std::memory_order_relaxed);
        const QPointer<BandcampListingResolver> guard{owner};
        report(ResolutionFailure{makeResolutionError(
            ResolutionErrorCode::Timeout, workerActive ? ResolutionStage::Parse : ResolutionStage::Fetch, pageUrl, 0,
            QStringLiteral("listing"), QStringLiteral("discography page timed out"))});
        if(!guard) {
            return;
        }
        abandonFetch();
        settleIfIdle();
    }

    void onFetch(quint64 id, BandcampFetchOutcome outcome)
    {
        if(!attempts.deliver(id)) {
            settleIfIdle();
            return;
        }
        requestId = 0;
        if(reported) {
            settleIfIdle();
            return;
        }
        if(std::holds_alternative<ResolutionCancelled>(outcome)) {
            const QPointer<BandcampListingResolver> guard{owner};
            report(ResolutionCancelled{});
            if(guard) {
                settleIfIdle();
            }
            return;
        }
        if(const auto* failure = std::get_if<BandcampFetchFailure>(&outcome)) {
            lastHttpStatus = failure->httpStatus;
            if(attempts.canRetry() && isRetriableFetchFailure(*failure)) {
                scheduleRetry(failure->error, failure->httpStatus);
                return;
            }
            const QPointer<BandcampListingResolver> guard{owner};
            report(ResolutionFailure{failure->error});
            if(guard) {
                settleIfIdle();
            }
            return;
        }
        auto response       = std::get<ResolutionSuccess<BandcampFetchResponse>>(std::move(outcome)).value;
        context.resourceUrl = response.url;
        parse(std::move(response.body), response.url);
    }

    void parse(QByteArray body, QUrl effectiveUrl)
    {
        workerActive  = true;
        auto* watcher = new QFutureWatcher<BandcampListingParseOutcome>(owner);
        const QPointer<BandcampListingResolver> guard{owner};
        QObject::connect(watcher, &QFutureWatcherBase::finished, owner, [guard, watcher]() {
            if(!guard) {
                watcher->deleteLater();
                return;
            }
            BandcampListingParseOutcome outcome = ResolutionCancelled{};
            try {
                outcome = watcher->result();
            }
            catch(...) {
                outcome = ResolutionFailure{makeResolutionError(
                    ResolutionErrorCode::MalformedPage, ResolutionStage::Parse, guard->m_impl->pageUrl, 0,
                    QStringLiteral("listing"), QStringLiteral("listing parser dependency failed"))};
            }
            watcher->deleteLater();
            watcher->setParent(nullptr);
            guard->m_impl->workerActive = false;
            if(!guard->m_impl->reported) {
                guard->m_impl->report(std::move(outcome));
                if(!guard) {
                    return;
                }
            }
            guard->m_impl->settleIfIdle();
        });

        const CorrelationContext parseContext = context;
        const auto stopToken                  = stop;
        const auto pageParser                 = parser;
        try {
            watcher->setFuture(
                QtConcurrent::run([body = std::move(body), effectiveUrl = std::move(effectiveUrl), parseContext,
                                   stopToken, pageParser]() mutable -> BandcampListingParseOutcome {
                    try {
                        const auto parsed = pageParser(body, effectiveUrl, parseContext, stopToken.get());
                        return stopToken->load(std::memory_order_relaxed)
                                 ? BandcampListingParseOutcome{ResolutionCancelled{}}
                                 : parsed;
                    }
                    catch(...) {
                        return ResolutionFailure{makeResolutionError(
                            ResolutionErrorCode::MalformedPage, ResolutionStage::Parse, effectiveUrl, 0,
                            QStringLiteral("listing"), QStringLiteral("listing parser dependency failed"))};
                    }
                }));
        }
        catch(...) {
            watcher->deleteLater();
            watcher->setParent(nullptr);
            workerActive = false;
            const QPointer<BandcampListingResolver> guard{owner};
            report(ResolutionFailure{makeResolutionError(
                ResolutionErrorCode::MalformedPage, ResolutionStage::Parse, pageUrl, 0, QStringLiteral("listing"),
                QStringLiteral("listing parser worker could not be started"))});
            if(guard) {
                settleIfIdle();
            }
        }
    }

    void report(BandcampListingParseOutcome outcome)
    {
        if(reported) {
            return;
        }
        reported = true;
        if(const auto* failure = std::get_if<ResolutionFailure>(&outcome)) {
            if(started) {
                logResolutionFailure(context, failure->error, lastHttpStatus, attempts, elapsed.elapsed());
            } else {
                logResolutionFailure(context, failure->error, lastHttpStatus);
            }
        }
        attempts.stop();
        const QPointer<BandcampListingResolver> guard{owner};
        callback(std::move(outcome));
        if(guard) {
            settleIfIdle();
        }
    }

    void settleIfIdle()
    {
        if(settled || !attempts.idle() || workerActive) {
            return;
        }
        if(!reported) {
            report(ResolutionFailure{makeResolutionError(
                ResolutionErrorCode::FetchFailed, ResolutionStage::Fetch, pageUrl, 0, QStringLiteral("listing"),
                QStringLiteral("listing workflow ended without an outcome"))});
            return;
        }
        settled = true;
        const QPointer<BandcampListingResolver> guard{owner};
        if(slotHeld) {
            slotHeld = false;
            workflowSlots->release();
            if(!guard) {
                return;
            }
        }
        emit owner->physicallyQuiescent();
    }
};

BandcampListingResolver::BandcampListingResolver(QNetworkAccessManager* network, BandcampOperationId operationId,
                                                 QUrl pageUrl, int attemptTimeoutMs, int maxAttempts,
                                                 int attemptCooldownMs, BandcampListingParser parser,
                                                 std::shared_ptr<BandcampWorkflowSlots> workflowSlots,
                                                 OutcomeCallback callback, QObject* parent)
    : QObject{parent}
    , m_impl{std::make_unique<Impl>(this, network, operationId, std::move(pageUrl),
                                    BandcampAttemptPolicy{attemptTimeoutMs, maxAttempts, attemptCooldownMs},
                                    std::move(parser), std::move(workflowSlots), std::move(callback))}
{ }

BandcampListingResolver::~BandcampListingResolver()
{
    m_impl->stop->store(true, std::memory_order_relaxed);
    m_impl->attempts.stop();
    if(m_impl->attempts.attemptActive() && m_impl->client && m_impl->requestId != 0) {
        m_impl->client->cancel(m_impl->requestId);
    }
}

void BandcampListingResolver::start()
{
    m_impl->start();
}

void BandcampListingResolver::cancel()
{
    m_impl->cancel();
}

} // namespace Fooyin::Bandcamp
