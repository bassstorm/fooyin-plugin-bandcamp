#include "bandcampdiagnostics.h"

#include "bandcampattemptsupervisor.h"
#include "bandcamplogging.h"
#include "bandcampoutcome.h"

#include <QLoggingCategory>

#include <algorithm>

namespace Fooyin::Bandcamp {
namespace {
constexpr auto Ellipsis = u"...";

bool isControlOrSeparator(QChar ch)
{
    const QChar::Category category = ch.category();
    return category == QChar::Other_Control || category == QChar::Separator_Line
        || category == QChar::Separator_Paragraph;
}

QString unicodeEscape(QChar ch)
{
    return QStringLiteral("\\u%1").arg(static_cast<uint>(ch.unicode()), 4, 16, QLatin1Char('0'));
}

QString escapedChar(QChar ch)
{
    switch(ch.unicode()) {
        case u'\\':
            return QStringLiteral("\\\\");
        case u'"':
            return QStringLiteral("\\\"");
        case u'\n':
            return QStringLiteral("\\n");
        case u'\r':
            return QStringLiteral("\\r");
        case u'\t':
            return QStringLiteral("\\t");
        default:
            break;
    }

    if(isControlOrSeparator(ch)) {
        return unicodeEscape(ch);
    }
    return QString(ch);
}

// Stops before a surrogate pair would be split, so the output is always valid UTF-16.
void dropLoneHighSurrogate(QString& text)
{
    if(!text.isEmpty() && text.back().isHighSurrogate()) {
        text.chop(1);
    }
}

// Simple tokens stay bare (`album=1`, `code=timeout`, `url=https://host/path`); anything else is quoted.
QString formatValue(const QString& value)
{
    const bool bare = !value.isEmpty() && std::none_of(value.cbegin(), value.cend(), [](QChar ch) {
        return ch.isSpace() || ch == u'"' || ch == u'\\' || ch.category() == QChar::Other_Control
            || ch.category() == QChar::Separator_Line || ch.category() == QChar::Separator_Paragraph;
    });
    if(bare) {
        return value;
    }
    return QStringLiteral("\"") + escapeDiagnosticText(value) + QStringLiteral("\"");
}

QString keyValue(const QString& name, const QString& value)
{
    return name + QStringLiteral("=") + formatValue(value);
}
} // namespace

CorrelationContext CorrelationContext::withAlbum(qint64 id) const
{
    CorrelationContext copy = *this;
    copy.albumId            = id;
    return copy;
}

CorrelationContext CorrelationContext::withTrack(qint64 id) const
{
    CorrelationContext copy = *this;
    copy.trackId            = id;
    return copy;
}

CorrelationContext CorrelationContext::withResourceUrl(const QUrl& url) const
{
    CorrelationContext copy = *this;
    copy.resourceUrl        = url;
    return copy;
}

DiagnosticField DiagnosticField::text(QString name, const QString& value)
{
    return DiagnosticField{std::move(name), value};
}

DiagnosticField DiagnosticField::number(QString name, qint64 value)
{
    return DiagnosticField{std::move(name), QString::number(value)};
}

DiagnosticField DiagnosticField::url(QString name, const QUrl& url)
{
    return DiagnosticField{std::move(name), cleanUrlForLog(url)};
}

QString cleanUrlForLog(const QUrl& url)
{
    if(!url.isValid()) {
        return QStringLiteral("<invalid url>");
    }
    return url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toString(QUrl::FullyEncoded);
}

QString escapeDiagnosticText(const QString& text)
{
    constexpr int limit = MaxDiagnosticFieldChars - 3;

    QString out;
    out.reserve(qMin(text.size(), MaxDiagnosticFieldChars));
    for(const QChar ch : text) {
        const QString piece = escapedChar(ch);
        if(out.size() + piece.size() > limit) {
            dropLoneHighSurrogate(out);
            out += Ellipsis;
            return out;
        }
        out += piece;
    }
    return out;
}

QString escapeControlChars(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for(const QChar ch : text) {
        out += isControlOrSeparator(ch) ? unicodeEscape(ch) : QString(ch);
    }
    return out;
}

QString formatTraceEvent(const CorrelationContext& context, const QString& event, const QList<DiagnosticField>& fields)
{
    QString record;
    record.reserve(MaxDiagnosticRecordChars);

    // The URL identifies the work only until an entity ID is known, so it is never repeated alongside one.
    if(context.albumId > 0) {
        record += keyValue(QStringLiteral("album"), QString::number(context.albumId)) + QLatin1Char(' ');
    }
    if(context.trackId > 0) {
        record += keyValue(QStringLiteral("track"), QString::number(context.trackId)) + QLatin1Char(' ');
    }
    if(context.albumId <= 0 && context.trackId <= 0 && context.resourceUrl.isValid()) {
        record += keyValue(QStringLiteral("url"), cleanUrlForLog(context.resourceUrl)) + QLatin1Char(' ');
    }

    record += escapeDiagnosticText(event);

    const int shown = qMin(fields.size(), static_cast<int>(MaxDiagnosticFields));
    for(int i = 0; i < shown; ++i) {
        record += QLatin1Char(' ') + keyValue(escapeDiagnosticText(fields.at(i).name), fields.at(i).value);
    }
    if(fields.size() > shown) {
        record += QLatin1Char(' ') + keyValue(QStringLiteral("fields_omitted"), QString::number(fields.size() - shown));
    }

    if(record.size() > MaxDiagnosticRecordChars) {
        record.truncate(MaxDiagnosticRecordChars - 3);
        dropLoneHighSurrogate(record);
        record += Ellipsis;
    }
    return record;
}

namespace {
CorrelationContext effectiveContext(const CorrelationContext& context, const ResolutionError& error)
{
    CorrelationContext merged = context;
    if(merged.albumId <= 0) {
        merged.albumId = error.albumId;
    }
    if(merged.trackId <= 0) {
        merged.trackId = error.trackId;
    }
    if(merged.albumId <= 0 && merged.trackId <= 0 && !merged.resourceUrl.isValid()) {
        merged.resourceUrl = error.resourceUrl;
    }
    return merged;
}

QString attemptsValue(int attempt, int maxAttempts)
{
    return QString::number(attempt) + QLatin1Char('/') + QString::number(maxAttempts);
}

// attempt <= 0 or elapsedMs < 0 means no attempt was started, so those fields are omitted.
void logFailureRecord(const CorrelationContext& context, const ResolutionError& error, int httpStatus, int attempt,
                      int maxAttempts, qint64 elapsedMs)
{
    if(!BANDCAMP_RESOLVE().isWarningEnabled()) {
        return;
    }
    QList<DiagnosticField> fields{
        DiagnosticField::text(QStringLiteral("stage"), resolutionStageName(error.stage)),
        DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error.code)),
    };
    if(attempt > 0) {
        fields.append(DiagnosticField::text(QStringLiteral("attempts"), attemptsValue(attempt, maxAttempts)));
    }
    if(httpStatus > 0) {
        fields.append(DiagnosticField::number(QStringLiteral("httpStatus"), httpStatus));
    }
    if(!error.field.isEmpty()) {
        fields.append(DiagnosticField::text(QStringLiteral("field"), error.field));
    }
    if(!error.value.isEmpty()) {
        fields.append(DiagnosticField::text(QStringLiteral("value"), error.value));
    }
    if(elapsedMs >= 0) {
        fields.append(DiagnosticField::number(QStringLiteral("elapsedMs"), elapsedMs));
    }
    if(!error.reason.isEmpty()) {
        fields.append(DiagnosticField::text(QStringLiteral("reason"), error.reason));
    }
    const CorrelationContext effective = effectiveContext(context, error);
    if((effective.albumId > 0 || effective.trackId > 0) && effective.resourceUrl.isValid()) {
        fields.append(DiagnosticField::url(QStringLiteral("url"), effective.resourceUrl));
    }
    qCWarning(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(effective, QStringLiteral("failed"), fields);
}
} // namespace

void logFieldConflict(const QLoggingCategory& category, const CorrelationContext& context, const QString& field,
                      const QString& chosen, const QString& ignored)
{
    if(!category.isDebugEnabled()) {
        return;
    }
    qCDebug(category).noquote() << formatTraceEvent(context, QStringLiteral("conflict"),
                                                    {DiagnosticField::text(QStringLiteral("field"), field),
                                                     DiagnosticField::text(QStringLiteral("chosen"), chosen),
                                                     DiagnosticField::text(QStringLiteral("ignored"), ignored)});
}

void logFieldFallback(const QLoggingCategory& category, const CorrelationContext& context, const QString& field,
                      const QString& chosen)
{
    if(!category.isDebugEnabled()) {
        return;
    }
    qCDebug(category).noquote() << formatTraceEvent(
        context, QStringLiteral("fallback"),
        {DiagnosticField::text(QStringLiteral("field"), field), DiagnosticField::text(QStringLiteral("new"), chosen)});
}

void logResolutionRetry(const CorrelationContext& context, const ResolutionError& error, int httpStatus,
                        const BandcampAttemptSupervisor& attempts)
{
    if(!BANDCAMP_RESOLVE().isWarningEnabled()) {
        return;
    }
    QList<DiagnosticField> fields{
        DiagnosticField::text(QStringLiteral("attempt"),
                              attemptsValue(attempts.attemptsMade(), attempts.maxAttempts())),
        DiagnosticField::number(QStringLiteral("delayMs"), attempts.cooldownMs()),
        DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(error.code)),
    };
    if(httpStatus > 0) {
        fields.append(DiagnosticField::number(QStringLiteral("httpStatus"), httpStatus));
    }
    if(!error.reason.isEmpty()) {
        fields.append(DiagnosticField::text(QStringLiteral("reason"), error.reason));
    }
    qCWarning(BANDCAMP_RESOLVE).noquote()
        << formatTraceEvent(effectiveContext(context, error), QStringLiteral("retry"), fields);
}

void logResolutionFailure(const CorrelationContext& context, const ResolutionError& error, int httpStatus)
{
    logFailureRecord(context, error, httpStatus, 0, 0, -1);
}

void logResolutionFailure(const CorrelationContext& context, const ResolutionError& error, int httpStatus,
                          const BandcampAttemptSupervisor& attempts, qint64 elapsedMs)
{
    logFailureRecord(context, error, httpStatus, attempts.attemptsMade(), attempts.maxAttempts(), elapsedMs);
}
} // namespace Fooyin::Bandcamp
