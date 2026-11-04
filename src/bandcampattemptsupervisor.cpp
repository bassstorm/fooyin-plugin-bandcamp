#include "bandcampattemptsupervisor.h"

namespace Fooyin::Bandcamp {
BandcampAttemptSupervisor::BandcampAttemptSupervisor(BandcampAttemptPolicy policy, QObject* parent)
    : QObject{parent}
    , m_policy{policy}
{
    m_deadline.setSingleShot(true);
    QObject::connect(&m_deadline, &QTimer::timeout, this, &BandcampAttemptSupervisor::attemptTimedOut);
    m_cooldown.setSingleShot(true);
    QObject::connect(&m_cooldown, &QTimer::timeout, this, [this]() {
        m_retryPending = false;
        emit retryReady();
    });
}

void BandcampAttemptSupervisor::resetBudget()
{
    m_retriesLeft = m_policy.maxAttempts - 1;
}

quint64 BandcampAttemptSupervisor::issueAttempt()
{
    m_active = true;
    m_deadline.start(m_policy.attemptTimeoutMs);
    ++m_outstanding;
    return ++m_attempt;
}

bool BandcampAttemptSupervisor::deliver(quint64 id)
{
    --m_outstanding;
    if(id != m_attempt) {
        return false;
    }
    m_active = false;
    return true;
}

void BandcampAttemptSupervisor::abandon()
{
    m_active = false;
    ++m_attempt;
}

bool BandcampAttemptSupervisor::attemptActive() const
{
    return m_active;
}

bool BandcampAttemptSupervisor::canRetry() const
{
    return m_retriesLeft > 0;
}

int BandcampAttemptSupervisor::retriesLeft() const
{
    return m_retriesLeft;
}

int BandcampAttemptSupervisor::cooldownMs() const
{
    return m_policy.cooldownMs;
}

void BandcampAttemptSupervisor::beginRetry()
{
    --m_retriesLeft;
    m_deadline.stop();
    m_retryPending = true;
    m_cooldown.start(m_policy.cooldownMs);
}

void BandcampAttemptSupervisor::stop()
{
    m_deadline.stop();
    m_cooldown.stop();
    m_retryPending = false;
}

bool BandcampAttemptSupervisor::idle() const
{
    return !m_active && !m_retryPending && m_outstanding == 0;
}
} // namespace Fooyin::Bandcamp
