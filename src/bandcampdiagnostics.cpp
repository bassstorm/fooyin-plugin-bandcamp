#include "bandcampdiagnostics.h"

#include <QRegularExpression>
#include <QStringBuilder>

namespace Fooyin::Bandcamp {
namespace {
constexpr auto Ellipsis = u"...";

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

    const QChar::Category category = ch.category();
    if(category == QChar::Other_Control || category == QChar::Separator_Line
       || category == QChar::Separator_Paragraph) {
        return QStringLiteral("\\u%1").arg(static_cast<uint>(ch.unicode()), 4, 16, QLatin1Char('0'));
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

QString keyValue(const QString& name, const QString& value)
{
    return name % QStringLiteral("=\"") % escapeDiagnosticText(value) % QStringLiteral("\"");
}
} // namespace

CorrelationContext CorrelationContext::withRequest(qint64 id) const
{
    CorrelationContext copy = *this;
    copy.requestId          = id;
    return copy;
}

CorrelationContext CorrelationContext::withTrack(qint64 id) const
{
    CorrelationContext copy = *this;
    copy.trackId            = id;
    return copy;
}

DiagnosticField DiagnosticField::text(QString name, const QString& value)
{
    return DiagnosticField{std::move(name), redactDiagnosticText(value)};
}

DiagnosticField DiagnosticField::number(QString name, qint64 value)
{
    return DiagnosticField{std::move(name), QString::number(value)};
}

DiagnosticField DiagnosticField::url(QString name, const QUrl& url)
{
    return DiagnosticField{std::move(name), redactUrlForLog(url)};
}

QString redactUrlForLog(const QUrl& url)
{
    if(!url.isValid()) {
        return QStringLiteral("<invalid url>");
    }
    return url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toString(QUrl::FullyEncoded);
}

QString redactDiagnosticText(const QString& text)
{
    static const QRegularExpression urls(QStringLiteral(R"((?:[A-Za-z][A-Za-z0-9+.-]*:)?//[^\s"'<>\\]+)"));
    const auto matches = urls.globalMatch(text);
    QList<QRegularExpressionMatch> found;
    auto iter = matches;
    while(iter.hasNext()) {
        found.append(iter.next());
    }

    QString redacted = text;
    for(auto it = found.crbegin(); it != found.crend(); ++it) {
        redacted.replace(it->capturedStart(), it->capturedLength(), redactUrlForLog(QUrl(it->captured())));
    }
    return redacted;
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

QString formatTraceEvent(const CorrelationContext& context, const QString& event, const QList<DiagnosticField>& fields)
{
    QString record;
    record.reserve(MaxDiagnosticRecordChars);

    // A track URI leads once the track ID is trustworthy. Before then, job and release context identify the event.
    if(context.trackId > 0) {
        record += QStringLiteral("bandcamp://track/%1 ").arg(context.trackId);
    }
    if(context.jobId > 0) {
        record += keyValue(QStringLiteral("job"), QString::number(context.jobId)) + QLatin1Char(' ');
    }
    if(context.requestId > 0) {
        record += keyValue(QStringLiteral("request"), QString::number(context.requestId)) + QLatin1Char(' ');
    }
    if(context.resourceUrl.isValid()) {
        record += keyValue(QStringLiteral("release"), redactUrlForLog(context.resourceUrl)) + QLatin1Char(' ');
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
} // namespace Fooyin::Bandcamp
