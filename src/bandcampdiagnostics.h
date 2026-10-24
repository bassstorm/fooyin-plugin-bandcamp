#pragma once

#include <QList>
#include <QString>
#include <QUrl>

namespace Fooyin::Bandcamp {
// Bounds keep one TRACE record small however large the input is.
constexpr int MaxDiagnosticFieldChars  = 256;
constexpr int MaxDiagnosticFields      = 16;
constexpr int MaxDiagnosticRecordChars = 2048;

// Immutable identity of the work an event belongs to. Zero or empty means not known yet.
struct CorrelationContext
{
    qint64 jobId{0};
    qint64 requestId{0};
    // Canonical release URL. It is redacted before formatting.
    QUrl resourceUrl;
    qint64 trackId{0};

    [[nodiscard]] CorrelationContext withRequest(qint64 id) const;
    [[nodiscard]] CorrelationContext withTrack(qint64 id) const;
};

struct DiagnosticField
{
    QString name;
    QString value;

    static DiagnosticField text(QString name, const QString& value);
    static DiagnosticField number(QString name, qint64 value);
    // Redacted before it is stored, so the raw URL never reaches a log.
    static DiagnosticField url(QString name, const QUrl& url);
};

// Drops the query, fragment, and userinfo. Queries carry signed stream tokens.
QString redactUrlForLog(const QUrl& url);

// Escapes control characters, separators, backslashes, and quotes so the text stays on one line.
// Output is capped at MaxDiagnosticFieldChars.
QString escapeDiagnosticText(const QString& text);

// Builds one escaped, single-line TRACE record. Callers should emit it only when the category is enabled:
// qCDebug(BANDCAMP_TRACE_FETCH).noquote() << formatTraceEvent(...) does not run the formatting when disabled.
QString formatTraceEvent(const CorrelationContext& context, const QString& event, const QList<DiagnosticField>& fields);
} // namespace Fooyin::Bandcamp
