#include "bandcampdiagnostics.h"
#include "bandcampresolution.h"
#include "bandcampsettings.h"

#include <QtTest>

#include <type_traits>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
class ContractTests : public QObject
{
    Q_OBJECT

private slots:
    void candidateFieldStates();
    void outcomeReportsSuccessOnly();
    void errorNamesAreStable();
    void clampsOperationOptions();
    void correlationContextIsImmutable();
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

} // namespace

int runContractTests(int argc, char** argv)
{
    ContractTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampcontracttests.moc"
