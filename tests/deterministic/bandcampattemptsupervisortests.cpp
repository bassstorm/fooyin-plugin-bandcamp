#include "bandcampattemptsupervisor.h"

#include <QSignalSpy>
#include <QtTest>

using namespace Fooyin::Bandcamp;

namespace {
BandcampAttemptPolicy policy(int attemptTimeoutMs, int maxAttempts, int cooldownMs)
{
    return BandcampAttemptPolicy{attemptTimeoutMs, maxAttempts, cooldownMs};
}

class AttemptSupervisorTests : public QObject
{
    Q_OBJECT

private slots:
    void resetBudgetAllowsMaxAttemptsMinusOneRetries();
    void singleAttemptPolicyNeverRetries();
    void issueAttemptMarksActiveWithFreshIds();
    void deliverCurrentAttemptClearsActive();
    void deliverStaleAttemptIsIgnored();
    void abandonKeepsRequestOutstandingUntilDelivered();
    void idleRequiresNoPendingRetry();
    void deadlineEmitsTimedOutOnce();
    void beginRetryStopsDeadlineAndEmitsRetryReadyAfterCooldown();
    void stopCancelsDeadlineAndCooldownWithoutReleasingRequests();
};

void AttemptSupervisorTests::resetBudgetAllowsMaxAttemptsMinusOneRetries()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 3, 7)};
    QCOMPARE(supervisor.cooldownMs(), 7);

    supervisor.resetBudget();
    QCOMPARE(supervisor.retriesLeft(), 2);
    QVERIFY(supervisor.canRetry());

    supervisor.beginRetry();
    supervisor.stop();
    QCOMPARE(supervisor.retriesLeft(), 1);
    QVERIFY(supervisor.canRetry());

    supervisor.beginRetry();
    supervisor.stop();
    QCOMPARE(supervisor.retriesLeft(), 0);
    QVERIFY(!supervisor.canRetry());
}

void AttemptSupervisorTests::singleAttemptPolicyNeverRetries()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 1, 0)};
    supervisor.resetBudget();
    QCOMPARE(supervisor.retriesLeft(), 0);
    QVERIFY(!supervisor.canRetry());
}

void AttemptSupervisorTests::issueAttemptMarksActiveWithFreshIds()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 2, 0)};
    QVERIFY(!supervisor.attemptActive());

    const quint64 first = supervisor.issueAttempt();
    QVERIFY(supervisor.attemptActive());

    supervisor.abandon();
    const quint64 second = supervisor.issueAttempt();
    QVERIFY(second != first);
    QVERIFY(supervisor.attemptActive());

    QVERIFY(!supervisor.deliver(first));
    QVERIFY(supervisor.deliver(second));
    QVERIFY(supervisor.idle());
}

void AttemptSupervisorTests::deliverCurrentAttemptClearsActive()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 1, 0)};
    const quint64 id = supervisor.issueAttempt();
    QVERIFY(!supervisor.idle());

    QVERIFY(supervisor.deliver(id));
    QVERIFY(!supervisor.attemptActive());
    QVERIFY(supervisor.idle());
}

void AttemptSupervisorTests::deliverStaleAttemptIsIgnored()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 2, 0)};
    const quint64 stale = supervisor.issueAttempt();
    supervisor.abandon();
    const quint64 current = supervisor.issueAttempt();

    QVERIFY(!supervisor.deliver(stale));
    QVERIFY(supervisor.attemptActive());
    QVERIFY(!supervisor.idle());

    QVERIFY(supervisor.deliver(current));
    QVERIFY(!supervisor.attemptActive());
    QVERIFY(supervisor.idle());
}

void AttemptSupervisorTests::abandonKeepsRequestOutstandingUntilDelivered()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 2, 0)};
    const quint64 id = supervisor.issueAttempt();
    supervisor.abandon();

    QVERIFY(!supervisor.attemptActive());
    QVERIFY(!supervisor.idle());

    QVERIFY(!supervisor.deliver(id));
    QVERIFY(supervisor.idle());
}

void AttemptSupervisorTests::idleRequiresNoPendingRetry()
{
    BandcampAttemptSupervisor supervisor{policy(1000, 2, 1000)};
    supervisor.resetBudget();

    const quint64 id = supervisor.issueAttempt();
    QVERIFY(supervisor.deliver(id));
    QVERIFY(supervisor.idle());

    supervisor.beginRetry();
    QVERIFY(!supervisor.idle());

    supervisor.stop();
    QVERIFY(supervisor.idle());
}

void AttemptSupervisorTests::deadlineEmitsTimedOutOnce()
{
    BandcampAttemptSupervisor supervisor{policy(20, 1, 0)};
    QSignalSpy timedOut{&supervisor, &BandcampAttemptSupervisor::attemptTimedOut};

    supervisor.issueAttempt();
    QVERIFY(timedOut.wait(1000));
    QCOMPARE(timedOut.count(), 1);

    QTest::qWait(60);
    QCOMPARE(timedOut.count(), 1);
}

void AttemptSupervisorTests::beginRetryStopsDeadlineAndEmitsRetryReadyAfterCooldown()
{
    BandcampAttemptSupervisor supervisor{policy(20, 2, 30)};
    supervisor.resetBudget();
    QSignalSpy timedOut{&supervisor, &BandcampAttemptSupervisor::attemptTimedOut};
    QSignalSpy retryReady{&supervisor, &BandcampAttemptSupervisor::retryReady};

    const quint64 id = supervisor.issueAttempt();
    supervisor.beginRetry();
    QVERIFY(!supervisor.idle());
    QCOMPARE(retryReady.count(), 0);

    QVERIFY(retryReady.wait(1000));
    QCOMPARE(retryReady.count(), 1);

    QTest::qWait(60);
    QCOMPARE(timedOut.count(), 0);

    QVERIFY(supervisor.deliver(id));
    supervisor.stop();
}

void AttemptSupervisorTests::stopCancelsDeadlineAndCooldownWithoutReleasingRequests()
{
    BandcampAttemptSupervisor supervisor{policy(20, 2, 20)};
    supervisor.resetBudget();
    QSignalSpy timedOut{&supervisor, &BandcampAttemptSupervisor::attemptTimedOut};
    QSignalSpy retryReady{&supervisor, &BandcampAttemptSupervisor::retryReady};

    const quint64 id = supervisor.issueAttempt();
    supervisor.stop();
    QTest::qWait(60);
    QCOMPARE(timedOut.count(), 0);
    QVERIFY(supervisor.attemptActive());
    QVERIFY(!supervisor.idle());

    QVERIFY(supervisor.deliver(id));
    QVERIFY(supervisor.idle());

    const quint64 next = supervisor.issueAttempt();
    QVERIFY(supervisor.deliver(next));
    supervisor.beginRetry();
    supervisor.stop();
    QTest::qWait(60);
    QCOMPARE(retryReady.count(), 0);
    QVERIFY(supervisor.idle());
}
} // namespace

int runAttemptSupervisorTests(int argc, char** argv)
{
    AttemptSupervisorTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampattemptsupervisortests.moc"
