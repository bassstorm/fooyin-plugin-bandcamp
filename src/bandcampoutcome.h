#pragma once

#include <QString>
#include <QUrl>

#include <utility>
#include <variant>

namespace Fooyin::Bandcamp {
enum class ResolutionStage : quint8
{
    Fetch,
    Parse,
    Acquire,
    Assemble,
    Admit,
    Refine,
    Publish
};

// Codes are stable identifiers. Their text names appear in logs and must not
// change.
enum class ResolutionErrorCode : quint8
{
    FetchFailed,
    Timeout,
    PageTooLarge,
    ResourceLimitExceeded,
    UnsupportedRequest,
    MalformedPage,
    RequiredFieldMissing,
    RelationshipUnresolved,
    ParentUnavailable,
    NoResolvableTracks,
    InvalidField,
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
    // Zero when the album identity is unknown.
    qint64 albumId{0};
    // Short offending value (for example `null` or a rejected URL). Empty when not applicable.
    QString value;
};

ResolutionError makeResolutionError(ResolutionErrorCode code, ResolutionStage stage, const QUrl& url, qint64 trackId,
                                    QString field, QString reason);
QString resolutionErrorCodeName(ResolutionErrorCode code);
QString resolutionStageName(ResolutionStage stage);

template <typename T>
struct ResolutionSuccess
{
    // Deliberately not default constructible: a default outcome would read as
    // success with placeholder data.
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

} // namespace Fooyin::Bandcamp
