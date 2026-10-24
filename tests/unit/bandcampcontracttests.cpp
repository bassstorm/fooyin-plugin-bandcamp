#include "bandcampclock.h"
#include "bandcampdiagnostics.h"
#include "bandcampresolution.h"
#include "bandcampsettings.h"

#include <QtTest>

#include <type_traits>
#include <variant>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace std::chrono_literals;

namespace {
class ManualClock final : public BandcampClock
{
public:
    [[nodiscard]] std::chrono::milliseconds now() const override
    {
        return m_now;
    }

    void advance(std::chrono::milliseconds by)
    {
        m_now += by;
    }

private:
    std::chrono::milliseconds m_now{1000ms};
};

class ContractTests : public QObject
{
    Q_OBJECT

private slots:
    void candidateFieldStates();
    void resolveScopeMapsEveryParentState();
    void unknownTrackIsNotResolved();
    void outcomeReportsSuccessOnly();
    void errorNamesAreStable();
    void clampsOperationOptions();
    void correlationContextIsImmutable();
    void deadlineFollowsManualClock();
    void negativeBudgetIsExpiredAtOnce();
    void deadlineSaturatesHugeBudget();
    void deadlineRemainingSurvivesNegativeClock();
};

void ContractTests::candidateFieldStates()
{
    const auto usable = CandidateField<int>::usable(7, FieldProvenance::Fallback);
    QVERIFY(usable.isUsable());
    QVERIFY(usable.provenance == FieldProvenance::Fallback);
    QCOMPARE(usable.value, 7);

    const auto invalid = CandidateField<int>::invalid();
    QVERIFY(invalid.state == FieldState::Invalid);
    QVERIFY(!invalid.isUsable());

    const auto missing = CandidateField<int>::missing();
    QVERIFY(missing.state == FieldState::Missing);
    QVERIFY(!missing.isUsable());
}

void ContractTests::resolveScopeMapsEveryParentState()
{
    const AlbumParent album{42, QUrl(u"https://artist.bandcamp.com/album/x"_s)};

    // An album selection is the whole album whatever the parent state.
    const auto wholeUnknown = resolveScope(AlbumSelection{}, UnknownParent{});
    QVERIFY(wholeUnknown.has_value());
    QVERIFY(std::holds_alternative<WholeAlbumScope>(*wholeUnknown));

    const auto wholeStandalone = resolveScope(AlbumSelection{}, StandaloneParent{});
    QVERIFY(wholeStandalone.has_value());
    QVERIFY(std::holds_alternative<WholeAlbumScope>(*wholeStandalone));

    const auto wholeAlbum = resolveScope(AlbumSelection{}, album);
    QVERIFY(wholeAlbum.has_value());
    QVERIFY(std::holds_alternative<WholeAlbumScope>(*wholeAlbum));

    const auto standalone = resolveScope(TrackSelection{9}, StandaloneParent{});
    QVERIFY(standalone.has_value());
    QVERIFY(std::holds_alternative<StandaloneTrackScope>(*standalone));
    QCOMPARE(std::get<StandaloneTrackScope>(*standalone).trackId, 9);

    const auto inAlbum = resolveScope(TrackSelection{9}, album);
    QVERIFY(inAlbum.has_value());
    QVERIFY(std::holds_alternative<AlbumTrackScope>(*inAlbum));
    QCOMPARE(std::get<AlbumTrackScope>(*inAlbum).trackId, 9);
}

void ContractTests::unknownTrackIsNotResolved()
{
    // Membership is unknown, so no scope may be chosen yet.
    QVERIFY(!resolveScope(TrackSelection{9}, UnknownParent{}).has_value());
}

void ContractTests::outcomeReportsSuccessOnly()
{
    // Regression check: a default-constructed outcome must not compile, so it cannot read as success.
    static_assert(!std::is_default_constructible_v<ResolutionOutcome<int>>);

    const ResolutionOutcome<int> success = ResolutionSuccess<int>{5};
    QVERIFY(isSuccess(success));

    ResolutionError error;
    error.code   = ResolutionErrorCode::Timeout;
    error.stage  = ResolutionStage::Fetch;
    error.reason = u"deadline"_s;
    const ResolutionOutcome<int> failure = ResolutionFailure{error};
    QVERIFY(!isSuccess(failure));
    QVERIFY(std::holds_alternative<ResolutionFailure>(failure));

    // Cancellation is neither success nor failure.
    const ResolutionOutcome<int> cancelled = ResolutionCancelled{};
    QVERIFY(!isSuccess(cancelled));
    QVERIFY(std::holds_alternative<ResolutionCancelled>(cancelled));
}

void ContractTests::errorNamesAreStable()
{
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::FetchFailed), u"fetch_failed"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::Timeout), u"timeout"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::PageTooLarge), u"page_too_large"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::UnsupportedRequest), u"unsupported_request"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::MalformedPage), u"malformed_page"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::RequiredFieldMissing), u"required_field_missing"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::RelationshipUnresolved), u"relationship_unresolved"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::ParentUnavailable), u"parent_unavailable"_s);
    QCOMPARE(resolutionErrorCodeName(ResolutionErrorCode::NoResolvableTracks), u"no_resolvable_tracks"_s);

    QCOMPARE(resolutionStageName(ResolutionStage::Fetch), u"fetch"_s);
    QCOMPARE(resolutionStageName(ResolutionStage::Parse), u"parse"_s);
    QCOMPARE(resolutionStageName(ResolutionStage::Acquire), u"acquire"_s);
    QCOMPARE(resolutionStageName(ResolutionStage::Assemble), u"assemble"_s);
    QCOMPARE(resolutionStageName(ResolutionStage::Admit), u"admit"_s);
    QCOMPARE(resolutionStageName(ResolutionStage::Refine), u"refine"_s);
    QCOMPARE(resolutionStageName(ResolutionStage::Publish), u"publish"_s);
}

void ContractTests::clampsOperationOptions()
{
    const auto low = clampOperationOptions(OperationOptions{.parallelism = 0, .itemTimeoutMs = 1});
    QCOMPARE(low.parallelism, MinParallelism);
    QCOMPARE(low.itemTimeoutMs, MinItemTimeoutMs);

    const auto high = clampOperationOptions(OperationOptions{.parallelism = 999, .itemTimeoutMs = 999999});
    QCOMPARE(high.parallelism, MaxParallelism);
    QCOMPARE(high.itemTimeoutMs, MaxItemTimeoutMs);

    const auto inRange = clampOperationOptions(OperationOptions{});
    QCOMPARE(inRange.parallelism, DefaultParallelism);
    QCOMPARE(inRange.itemTimeoutMs, DefaultItemTimeoutMs);
}

void ContractTests::correlationContextIsImmutable()
{
    const CorrelationContext base{.jobId = 1};
    const CorrelationContext withRequest = base.withRequest(5);
    const CorrelationContext withTrack   = withRequest.withTrack(9);

    QCOMPARE(base.requestId, 0);
    QCOMPARE(base.trackId, 0);
    QCOMPARE(withRequest.jobId, 1);
    QCOMPARE(withRequest.requestId, 5);
    QCOMPARE(withRequest.trackId, 0);
    QCOMPARE(withTrack.requestId, 5);
    QCOMPARE(withTrack.trackId, 9);
}

void ContractTests::deadlineFollowsManualClock()
{
    ManualClock clock;
    const Deadline deadline = Deadline::after(clock, 500ms);

    QVERIFY(!deadline.expired(clock));
    QCOMPARE(deadline.remaining(clock), 500ms);

    clock.advance(499ms);
    QVERIFY(!deadline.expired(clock));
    QCOMPARE(deadline.remaining(clock), 1ms);

    clock.advance(1ms);
    QVERIFY(deadline.expired(clock));
    QCOMPARE(deadline.remaining(clock), 0ms);
}

void ContractTests::negativeBudgetIsExpiredAtOnce()
{
    ManualClock clock;
    const Deadline deadline = Deadline::after(clock, -5ms);

    QVERIFY(deadline.expired(clock));
    QCOMPARE(deadline.remaining(clock), 0ms);
}

void ContractTests::deadlineSaturatesHugeBudget()
{
    ManualClock clock;
    const Deadline deadline = Deadline::after(clock, std::chrono::milliseconds::max());

    // The sum with the clock would overflow, so the deadline saturates and never expires on its own.
    QVERIFY(!deadline.expired(clock));
    clock.advance(std::chrono::hours{24 * 365});
    QVERIFY(!deadline.expired(clock));
    QCOMPARE(deadline.remaining(clock), std::chrono::milliseconds::max() - clock.now());
}

void ContractTests::deadlineRemainingSurvivesNegativeClock()
{
    ManualClock clock;
    const Deadline finite    = Deadline::after(clock, 500ms);
    const Deadline unbounded = Deadline::after(clock, std::chrono::milliseconds::max());

    // Moving the clock below zero is where a naive remaining-time difference overflows.
    clock.advance(-5000ms);
    QCOMPARE(clock.now(), -4000ms);
    QVERIFY(!unbounded.expired(clock));
    QCOMPARE(finite.remaining(clock), 5500ms);
    QCOMPARE(unbounded.remaining(clock), std::chrono::milliseconds::max());
}
} // namespace

int runContractTests(int argc, char** argv)
{
    ContractTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampcontracttests.moc"
