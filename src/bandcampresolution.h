#pragma once

#include "bandcampsettings.h"

#include <QString>
#include <QUrl>

#include <optional>
#include <variant>

namespace Fooyin::Bandcamp {
// Where a candidate value came from. Explicit values are read from the resource's own data.
enum class FieldProvenance : quint8 { Explicit, Fallback, Inherited, Derived };

// Invalid is kept apart from Missing so a rejected source value is not mistaken for an absent one.
enum class FieldState : quint8 { Missing, Invalid, Usable };

template <typename T>
struct CandidateField
{
    FieldState state{FieldState::Missing};
    // Semantic provenance only. Concrete JSONPaths stay in parser diagnostics.
    FieldProvenance provenance{FieldProvenance::Explicit};
    T value{};

    [[nodiscard]] bool isUsable() const { return state == FieldState::Usable; }

    static CandidateField usable(T value, FieldProvenance provenance)
    {
        return CandidateField{FieldState::Usable, provenance, std::move(value)};
    }

    static CandidateField invalid()
    {
        return CandidateField{FieldState::Invalid, FieldProvenance::Explicit, T{}};
    }

    static CandidateField missing()
    {
        return CandidateField{};
    }
};

// Membership is unknown until the parent has been checked. Standalone is an established fact, never a default.
struct UnknownParent
{ };

struct StandaloneParent
{ };

struct AlbumParent
{
    qint64 albumId{0};
    QUrl albumUrl;
};

using ParentState = std::variant<UnknownParent, StandaloneParent, AlbumParent>;

// What the caller asked for.
struct AlbumSelection
{ };

struct TrackSelection
{
    qint64 trackId{0};
};

using RequestedScope = std::variant<AlbumSelection, TrackSelection>;

// What the request resolves to once the parent state is known.
struct WholeAlbumScope
{ };

struct AlbumTrackScope
{
    qint64 trackId{0};
};

struct StandaloneTrackScope
{
    qint64 trackId{0};
};

using ResolvedScope = std::variant<WholeAlbumScope, AlbumTrackScope, StandaloneTrackScope>;

// Returns nullopt while a track's membership is still unknown, so no scope can be chosen yet.
std::optional<ResolvedScope> resolveScope(const RequestedScope& requested, const ParentState& parent);

enum class ResolutionStage : quint8 { Fetch, Parse, Acquire, Assemble, Admit, Refine, Publish };

// Codes are stable identifiers. Their text names appear in logs and must not change.
enum class ResolutionErrorCode : quint8 {
    FetchFailed,
    Timeout,
    PageTooLarge,
    UnsupportedRequest,
    MalformedPage,
    RequiredFieldMissing,
    RelationshipUnresolved,
    ParentUnavailable,
    NoResolvableTracks,
};

struct ResolutionError
{
    ResolutionErrorCode code{ResolutionErrorCode::FetchFailed};
    ResolutionStage stage{ResolutionStage::Fetch};
    // Redacted before it is formatted for logs.
    QUrl resourceUrl;
    // Zero when the track identity is unknown.
    qint64 trackId{0};
    // Empty when the failure is not tied to one field.
    QString field;
    // Concise and safe. It must not contain page content.
    QString reason;
};

QString resolutionErrorCodeName(ResolutionErrorCode code);
QString resolutionStageName(ResolutionStage stage);

template <typename T>
struct ResolutionSuccess
{
    // Deliberately not default constructible: a default outcome would read as success with placeholder data.
    ResolutionSuccess() = delete;
    explicit ResolutionSuccess(T result)
        : value{std::move(result)}
    { }

    T value;
};

struct ResolutionFailure
{
    ResolutionError error;
};

struct ResolutionCancelled
{ };

// Expected failures are values, not exceptions. Cancellation is not a failure.
template <typename T>
using ResolutionOutcome = std::variant<ResolutionSuccess<T>, ResolutionFailure, ResolutionCancelled>;

template <typename T>
[[nodiscard]] bool isSuccess(const ResolutionOutcome<T>& outcome)
{
    return std::holds_alternative<ResolutionSuccess<T>>(outcome);
}

struct OperationOptions
{
    int parallelism{DefaultParallelism};
    int itemTimeoutMs{DefaultItemTimeoutMs};
};

// Clamps each value to the user-facing range in bandcampsettings.h.
OperationOptions clampOperationOptions(OperationOptions options);
} // namespace Fooyin::Bandcamp
