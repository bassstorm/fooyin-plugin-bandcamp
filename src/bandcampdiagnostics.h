#pragma once

#include <QList>
#include <QString>
#include <QUrl>

class QLoggingCategory;

namespace Fooyin::Bandcamp {
class BandcampAttemptSupervisor;
struct ResolutionError;

// Bounds keep one TRACE record small however large the input is.
constexpr int MaxDiagnosticFieldChars  = 256;
constexpr int MaxDiagnosticFields      = 16;
constexpr int MaxDiagnosticRecordChars = 2048;

// Entity correlation for one log record. Zero or empty means not known yet.
struct CorrelationContext
{
    qint64 albumId{0};
    qint64 trackId{0};
    // Full release or listing URL. Only logged as `url=` while neither ID is known.
    QUrl resourceUrl;

    [[nodiscard]] CorrelationContext withAlbum(qint64 id) const;
    [[nodiscard]] CorrelationContext withTrack(qint64 id) const;
    [[nodiscard]] CorrelationContext withResourceUrl(const QUrl& url) const;
};

struct DiagnosticField
{
    QString name;
    QString value;

    static DiagnosticField text(QString name, const QString& value);
    static DiagnosticField number(QString name, qint64 value);
    // Cleaned before it is stored, so signed query tokens never reach a log.
    static DiagnosticField url(QString name, const QUrl& url);
};

// Keeps the scheme, host, and path (custom Bandcamp domains included) and drops query, fragment, and userinfo.
// Invalid URLs become `<invalid url>`.
QString cleanUrlForLog(const QUrl& url);

// Escapes control characters, separators, backslashes, and quotes so the text stays on one line.
// Output is capped at MaxDiagnosticFieldChars.
QString escapeDiagnosticText(const QString& text);

// Escapes only control characters and line or paragraph separators, with no length cap. For verbatim JSON snapshots.
QString escapeControlChars(const QString& text);

// Builds one single-line record: `<correlation> <event> <key=value...>`.
// Callers should emit it only when the category is enabled:
// qCDebug(BANDCAMP_TRACE_FETCH).noquote() << formatTraceEvent(...) does not run the formatting when disabled.
QString formatTraceEvent(const CorrelationContext& context, const QString& event, const QList<DiagnosticField>& fields);

// Debug `conflict` record for two usable values that disagree. `chosen` wins, `ignored` is dropped.
// Values are raw; they are escaped once here.
void logFieldConflict(const QLoggingCategory& category, const CorrelationContext& context, const QString& field,
                      const QString& chosen, const QString& ignored);

// Debug `fallback` record for a value taken from a secondary source.
void logFieldFallback(const QLoggingCategory& category, const CorrelationContext& context, const QString& field,
                      const QString& chosen);

// Single Warning for one retried failure. Logged by the layer that decides to retry.
// The attempt count and the cooldown delay come from the supervisor, which must already have begun the retry.
void logResolutionRetry(const CorrelationContext& context, const ResolutionError& error, int httpStatus,
                        const BandcampAttemptSupervisor& attempts);

// Single Warning for a terminal failure after at least one attempt started. Logged once, by the layer that decides
// the outcome. Zero or negative httpStatus is omitted.
void logResolutionFailure(const CorrelationContext& context, const ResolutionError& error, int httpStatus,
                          const BandcampAttemptSupervisor& attempts, qint64 elapsedMs);

// Single Warning for a terminal failure before any attempt started: omits the attempt and elapsed-time fields.
void logResolutionFailure(const CorrelationContext& context, const ResolutionError& error, int httpStatus);
} // namespace Fooyin::Bandcamp
