#pragma once

#include "bandcampclient.h"

#include <QObject>
#include <QTimer>

namespace Fooyin::Bandcamp {
struct BandcampAttemptPolicy
{
    int attemptTimeoutMs{0};
    int maxAttempts{1};
    int cooldownMs{0};
};

// Only transient transport and server-side failures are repeated. Deterministic failures such as an oversized page,
// an unsupported redirect, or any permanent client error (401, 403, 404, 410, ...) would fail the same way again.
inline bool isRetriableFetchFailure(const BandcampFetchFailure& failure)
{
    if(failure.error.code != ResolutionErrorCode::FetchFailed) {
        return false;
    }
    return failure.httpStatus == 0 || failure.httpStatus == 408 || failure.httpStatus == 429
        || failure.httpStatus >= 500;
}

// Supervises the attempt lifecycle of one fetch chain: a deadline per attempt, a cooldown before each retry,
// stale-attempt detection, and a count of issued requests not yet delivered. Deciding whether a failure is retried or
// reported stays with the owner.
class BandcampAttemptSupervisor : public QObject
{
    Q_OBJECT

public:
    explicit BandcampAttemptSupervisor(BandcampAttemptPolicy policy, QObject* parent = nullptr);

    void resetBudget();
    quint64 issueAttempt();
    bool deliver(quint64 id);
    void abandon();
    bool attemptActive() const;
    bool canRetry() const;
    int retriesLeft() const;
    // Attempts started in the current chain, counting the first one.
    int attemptsMade() const;
    int maxAttempts() const;
    int cooldownMs() const;
    void beginRetry();
    void stop();
    bool idle() const;

signals:
    void attemptTimedOut();
    void retryReady();

private:
    BandcampAttemptPolicy m_policy;
    QTimer m_deadline;
    QTimer m_cooldown;
    int m_retriesLeft{0};
    quint64 m_attempt{0};
    int m_outstanding{0};
    bool m_active{false};
    bool m_retryPending{false};
};
} // namespace Fooyin::Bandcamp
