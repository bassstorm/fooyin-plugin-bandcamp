#include "bandcampparser.h"

#include "bandcamplogging.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xpath.h>

#include <cmath>
#include <limits>
#include <memory>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
enum class SourceKind : quint8
{
    Album,
    Track
};

QString cleanText(QString text)
{
    for(QChar& ch : text) {
        if(ch.unicode() < 0x20 || ch.unicode() == 0x7F) {
            ch = QLatin1Char(' ');
        }
    }
    return text.simplified();
}

bool decodeEntity(QStringView name, QString& out)
{
    if(name == u"amp") {
        out = QStringLiteral("&");
    }
    else if(name == u"quot") {
        out = QStringLiteral("\"");
    }
    else if(name == u"apos") {
        out = QStringLiteral("'");
    }
    else if(name == u"lt") {
        out = QStringLiteral("<");
    }
    else if(name == u"gt") {
        out = QStringLiteral(">");
    }
    else {
        if(name.isEmpty() || name.at(0) != u'#') {
            return false;
        }
        const bool hex    = name.size() > 1 && (name.at(1) == u'x' || name.at(1) == u'X');
        const auto digits = name.mid(hex ? 2 : 1);
        if(digits.isEmpty() || digits.size() > 8) {
            return false;
        }
        bool ok         = false;
        const uint code = digits.toUInt(&ok, hex ? 16 : 10);
        if(!ok || code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        const char32_t ch = static_cast<char32_t>(code);
        out               = QString::fromUcs4(&ch, 1);
    }
    return true;
}

// Single-pass, bounded look-ahead: an escaped entity is never decoded twice.
bool stopRequested(const std::atomic_bool* stop)
{
    return stop && stop->load(std::memory_order_relaxed);
}

QString decodeHtmlEntities(const QString& text, const std::atomic_bool* stop)
{
    QString out;
    out.reserve(text.size());
    for(qsizetype i = 0; i < text.size();) {
        if((i & 0x3ff) == 0 && stopRequested(stop)) {
            return {};
        }
        if(text.at(i) == u'&') {
            const qsizetype end = qMin(text.size(), i + 12);
            qsizetype semi      = i + 1;
            while(semi < end && text.at(semi) != u';') {
                ++semi;
            }
            QString decoded;
            if(semi < end && decodeEntity(QStringView(text).mid(i + 1, semi - i - 1), decoded)) {
                out += decoded;
                i = semi + 1;
                continue;
            }
        }
        out += text.at(i++);
    }
    return out;
}

ResolutionFailure failure(ResolutionErrorCode code, const QUrl& url, const QString& field, const QString& reason)
{
    return {{code, ResolutionStage::Parse, url, 0, field, reason}};
}

// Check before Qt allocates a decoded tree. Brackets inside strings do not count.
bool withinDepth(const QString& json, const std::atomic_bool* stop)
{
    int depth    = 0;
    bool quoted  = false;
    bool escaped = false;
    for(qsizetype i = 0; i < json.size(); ++i) {
        if((i & 0x3ff) == 0 && stopRequested(stop)) {
            return false;
        }
        const QChar ch = json.at(i);
        if(quoted) {
            if(escaped) {
                escaped = false;
            }
            else if(ch == u'\\') {
                escaped = true;
            }
            else if(ch == u'"') {
                quoted = false;
            }
        }
        else if(ch == u'"') {
            quoted = true;
        }
        else if(ch == u'{' || ch == u'[') {
            if(++depth > MaxJsonDepth) {
                return false;
            }
        }
        else if(ch == u'}' || ch == u']') {
            --depth;
        }
    }
    return true;
}

struct DecodedSource
{
    QJsonValue value;
    QString json;
};

using Decoded = std::variant<DecodedSource, ResolutionFailure>;

Decoded extractTralbum(const QByteArray& body, const QUrl& url, const std::atomic_bool* stop)
{
    if(stopRequested(stop)) {
        return failure(ResolutionErrorCode::Timeout, url, QStringLiteral("parser"), QStringLiteral("parse cancelled"));
    }
    if(body.size() > MaxPageBytes) {
        return failure(ResolutionErrorCode::PageTooLarge, url, {}, QStringLiteral("response byte limit exceeded"));
    }
    static const QByteArray marker("data-tralbum=");
    const qsizetype start = body.indexOf(marker);
    if(start < 0 || start + marker.size() >= body.size()) {
        return failure(ResolutionErrorCode::MalformedPage, url, {}, QStringLiteral("missing release data"));
    }
    const qsizetype quotePos = start + marker.size();
    const char quote         = body.at(quotePos);
    const qsizetype end      = body.indexOf(quote, quotePos + 1);
    if((quote != '"' && quote != '\'') || end < 0 || end == quotePos + 1) {
        return failure(ResolutionErrorCode::MalformedPage, url, {}, QStringLiteral("invalid release attribute"));
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString escaped = decoder(body.mid(quotePos + 1, end - quotePos - 1));
    if(stopRequested(stop)) {
        return failure(ResolutionErrorCode::Timeout, url, QStringLiteral("parser"), QStringLiteral("parse cancelled"));
    }
    if(decoder.hasError()) {
        return failure(ResolutionErrorCode::MalformedPage, url, {}, QStringLiteral("invalid release UTF-8"));
    }
    const QString json = decodeHtmlEntities(escaped, stop);
    if(stopRequested(stop)) {
        return failure(ResolutionErrorCode::Timeout, url, QStringLiteral("parser"), QStringLiteral("parse cancelled"));
    }
    if(!withinDepth(json, stop)) {
        if(stopRequested(stop)) {
            return failure(ResolutionErrorCode::Timeout, url, QStringLiteral("parser"),
                           QStringLiteral("parse cancelled"));
        }
        return failure(ResolutionErrorCode::ResourceLimitExceeded, url, {},
                       QStringLiteral("JSON depth limit exceeded"));
    }
    QJsonParseError error;
    // A wrapper permits snapshots of valid scalar JSON too, before rejecting its release shape.
    const auto doc = QJsonDocument::fromJson('[' + json.toUtf8() + ']', &error);
    if(stopRequested(stop)) {
        return failure(ResolutionErrorCode::Timeout, url, QStringLiteral("parser"), QStringLiteral("parse cancelled"));
    }
    if(error.error != QJsonParseError::NoError || doc.array().size() != 1) {
        return failure(ResolutionErrorCode::MalformedPage, url, {}, QStringLiteral("invalid release JSON"));
    }
    return DecodedSource{doc.array().first(), json};
}

// Qt preserves integer tokens, but can round a fractional token to an integer.
// The private companion retains lexical number tokens and stays inside the parser.
// It lets integral fields validate the original decimal without passing through double.
QJsonDocument numericLexemes(const QString& json, const std::atomic_bool* stop)
{
    QString encoded;
    encoded.reserve(json.size());
    bool quoted  = false;
    bool escaped = false;
    for(qsizetype i = 0; i < json.size(); ++i) {
        if((i & 0x3ff) == 0 && stopRequested(stop)) {
            return {};
        }
        const QChar ch = json.at(i);
        if(!quoted && (ch == u'-' || (ch >= u'0' && ch <= u'9'))) {
            encoded += '"';
            do {
                if((i & 0x3ff) == 0 && stopRequested(stop)) {
                    return {};
                }
                encoded += json.at(i);
                ++i;
            } while(i < json.size()
                    && (json.at(i).isDigit() || json.at(i) == u'.' || json.at(i) == u'e' || json.at(i) == u'E'
                        || json.at(i) == u'-' || json.at(i) == u'+'));
            encoded += '"';
            --i;
            continue;
        }
        encoded += ch;
        if(quoted) {
            if(escaped) {
                escaped = false;
            }
            else if(ch == u'\\') {
                escaped = true;
            }
            else if(ch == u'"') {
                quoted = false;
            }
        }
        else if(ch == u'"') {
            quoted = true;
        }
    }
    return QJsonDocument::fromJson(encoded.toUtf8());
}

bool exactPositiveInteger(QString token, qint64& out)
{
    if(token.startsWith(u'-')) {
        return false;
    }
    qsizetype exponentAt = token.indexOf(u'e');
    if(exponentAt < 0) {
        exponentAt = token.indexOf(u'E');
    }
    int exponent = 0;
    if(exponentAt >= 0) {
        bool ok  = false;
        exponent = token.mid(exponentAt + 1).toInt(&ok);
        if(!ok || exponent < -MaxSourceFieldChars || exponent > MaxSourceFieldChars) {
            return false;
        }
        token.truncate(exponentAt);
    }
    const qsizetype point    = token.indexOf(u'.');
    const qsizetype fraction = point >= 0 ? token.size() - point - 1 : 0;
    if(point >= 0) {
        token.remove(point, 1);
    }
    qsizetype first = 0;
    while(first < token.size() && token.at(first) == u'0') {
        ++first;
    }
    token.remove(0, first);
    if(token.isEmpty()) {
        return false;
    }
    const qsizetype scale = static_cast<qsizetype>(exponent) - fraction;
    if(scale < 0) {
        if(-scale >= token.size()) {
            return false;
        }
        for(qsizetype i = token.size() + scale; i < token.size(); ++i) {
            if(token.at(i) != u'0') {
                return false;
            }
        }
        token.chop(-scale);
    }
    else {
        if(token.size() + scale > 19) {
            return false;
        }
        token += QString(scale, u'0');
    }
    if(token.size() > 19) {
        return false;
    }
    bool ok = false;
    out     = token.toLongLong(&ok);
    return ok && out > 0;
}

struct SourceObject
{
    QJsonObject values;
    QJsonObject numbers;

    QJsonValue value(QStringView key) const
    {
        return values.value(key);
    }

    CandidateField<qint64> integer(QStringView key, qint64 maximum = std::numeric_limits<qint64>::max()) const
    {
        const auto input = value(key);
        if(input.isUndefined()) {
            return CandidateField<qint64>::missing();
        }
        if(!input.isString() && !input.isDouble()) {
            return CandidateField<qint64>::invalid();
        }
        const auto token = numbers.value(key).toString();
        if(input.isString()) {
            if(token.isEmpty()) {
                return CandidateField<qint64>::invalid();
            }
            for(QChar ch : token) {
                if(ch < u'0' || ch > u'9') {
                    return CandidateField<qint64>::invalid();
                }
            }
        }
        qint64 result = 0;
        if(!exactPositiveInteger(token, result) || result > maximum) {
            return CandidateField<qint64>::invalid();
        }
        return CandidateField<qint64>::usable(result, FieldProvenance::Explicit);
    }

    bool explicitNoParent(QStringView key) const
    {
        const auto input = value(key);
        if(input.isNull()) {
            return true;
        }
        if(input.isString()) {
            return input.toString() == u"0";
        }
        if(!input.isDouble()) {
            return false;
        }
        auto token           = numbers.value(key).toString();
        qsizetype exponentAt = token.indexOf(u'e');
        if(exponentAt < 0) {
            exponentAt = token.indexOf(u'E');
        }
        if(exponentAt >= 0) {
            token.truncate(exponentAt);
        }
        for(QChar ch : token) {
            if(ch != u'0' && ch != u'.' && ch != u'-') {
                return false;
            }
        }
        return !token.isEmpty();
    }
};

QString safeText(QString text)
{
    static const QRegularExpression urls(QStringLiteral(R"((?:[A-Za-z][A-Za-z0-9+.-]*:)?//[^\s"'<>\\]+)"));
    const auto matches = urls.globalMatch(text);
    QList<QRegularExpressionMatch> found;
    auto iter = matches;
    while(iter.hasNext()) {
        found.append(iter.next());
    }
    for(auto it = found.crbegin(); it != found.crend(); ++it) {
        text.replace(it->capturedStart(), it->capturedLength(), redactUrlForLog(QUrl(it->captured())));
    }
    return text;
}

QJsonValue redactedSnapshot(const QJsonValue& value, const std::atomic_bool* stop, bool sensitive = false,
                            bool locators = false)
{
    if(stopRequested(stop)) {
        return {};
    }
    if(value.isObject()) {
        const auto object = value.toObject();
        QJsonObject redacted;
        for(auto it = object.begin(); it != object.end(); ++it) {
            if(stopRequested(stop)) {
                return {};
            }
            const QString key = it.key().toLower();
            const bool secret = sensitive || key.contains(u"signature") || key.contains(u"token")
                             || key.contains(u"password") || key.contains(u"authorization");
            const bool url = locators || key == u"file" || key == u"url" || key.endsWith(u"_url");
            redacted.insert(safeText(it.key()), redactedSnapshot(it.value(), stop, secret, url));
        }
        return redacted;
    }
    if(value.isArray()) {
        QJsonArray array;
        for(const auto& child : value.toArray()) {
            if(stopRequested(stop)) {
                return {};
            }
            array.append(redactedSnapshot(child, stop, sensitive, locators));
        }
        return array;
    }
    if(sensitive && !value.isNull()) {
        return QStringLiteral("<redacted>");
    }
    if(value.isString()) {
        return locators ? redactUrlForLog(QUrl(value.toString())) : safeText(value.toString());
    }
    return value;
}

void logSnapshot(const QJsonValue& root, const CorrelationContext& context, const std::atomic_bool* stop)
{
    if(!BANDCAMP_TRACE_PARSER().isDebugEnabled() || stopRequested(stop)) {
        return;
    }
    const auto redacted = redactedSnapshot(root, stop);
    if(stopRequested(stop)) {
        return;
    }
    const QByteArray encoded = QJsonDocument(QJsonArray{redacted}).toJson(QJsonDocument::Compact);
    QString snapshot         = QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
    QString escaped;
    escaped.reserve(snapshot.size());
    for(qsizetype i = 0; i < snapshot.size(); ++i) {
        if((i & 0x3ff) == 0 && stopRequested(stop)) {
            return;
        }
        const QChar ch      = snapshot.at(i);
        const auto category = ch.category();
        if(category == QChar::Other_Control || category == QChar::Separator_Line
           || category == QChar::Separator_Paragraph) {
            escaped += QStringLiteral("\\u%1").arg(static_cast<uint>(ch.unicode()), 4, 16, QLatin1Char('0'));
        }
        else {
            escaped += ch;
        }
    }
    // One bounded-by-input, encoded record; never pass the snapshot through field truncation.
    qCDebug(BANDCAMP_TRACE_PARSER).noquote()
        << formatTraceEvent(context, QStringLiteral("source_snapshot"), {}) + QStringLiteral(" json=") + escaped;
}

bool withinFields(const QJsonValue& value, qsizetype& records, const std::atomic_bool* stop)
{
    if(stopRequested(stop)) {
        return false;
    }
    if(++records > MaxSourceRecords) {
        return false;
    }
    if(value.isString()) {
        return value.toString().size() <= MaxSourceFieldChars;
    }
    if(value.isArray()) {
        for(const auto& child : value.toArray()) {
            if(!withinFields(child, records, stop)) {
                return false;
            }
        }
    }
    if(value.isObject()) {
        const auto object = value.toObject();
        for(auto it = object.begin(); it != object.end(); ++it) {
            if(stopRequested(stop) || it.key().size() > MaxSourceFieldChars
               || !withinFields(it.value(), records, stop)) {
                return false;
            }
        }
    }
    return true;
}

template <typename T, typename Reader>
CandidateField<T> read(const QJsonValue& value, Reader reader)
{
    if(value.isUndefined()) {
        return CandidateField<T>::missing();
    }
    T result{};
    if(!reader(value, result)) {
        return CandidateField<T>::invalid();
    }
    return CandidateField<T>::usable(std::move(result), FieldProvenance::Explicit);
}

CandidateField<QString> text(const QJsonValue& value)
{
    return read<QString>(value, [](const QJsonValue& v, QString& out) {
        if(!v.isString()) {
            return false;
        }
        out = cleanText(v.toString());
        return !out.isEmpty();
    });
}

CandidateField<QDate> date(const QJsonValue& value)
{
    return read<QDate>(value, [](const QJsonValue& v, QDate& out) {
        if(!v.isString()) {
            return false;
        }
        const auto string = cleanText(v.toString());
        out               = QLocale::c().toDate(string.section(QLatin1Char(' '), 0, 2), QStringLiteral("d MMM yyyy"));
        if(!out.isValid()) {
            out = QDate::fromString(string, Qt::ISODate);
        }
        if(!out.isValid()) {
            out = QDateTime::fromString(string, Qt::ISODate).date();
        }
        return out.isValid();
    });
}

bool durationSeconds(const QString& text, double& seconds)
{
    const auto parts = text.trimmed().split(u':');
    if(parts.isEmpty() || parts.size() > 3) {
        return false;
    }
    seconds = 0;
    for(qsizetype i = 0; i < parts.size(); ++i) {
        const auto& part = parts.at(i);
        if(part.isEmpty()) {
            return false;
        }
        if(i + 1 < parts.size()) {
            for(QChar ch : part) {
                if(ch < u'0' || ch > u'9') {
                    return false;
                }
            }
        }
        bool ok            = false;
        const double value = QLocale::c().toDouble(part, &ok);
        if(!ok || !std::isfinite(value) || value < 0 || (i > 0 && value >= 60)) {
            return false;
        }
        seconds = seconds * 60 + value;
    }
    return true;
}

CandidateField<qint64> duration(const QJsonValue& value)
{
    return read<qint64>(value, [](const QJsonValue& v, qint64& out) {
        double seconds = 0;
        if(v.isDouble()) {
            seconds = v.toDouble();
        }
        else if(v.isString()) {
            if(!durationSeconds(v.toString(), seconds)) {
                return false;
            }
        }
        else {
            return false;
        }
        if(!std::isfinite(seconds) || seconds < 0 || seconds > static_cast<double>(MaxDurationMs) / 1000) {
            return false;
        }
        out = static_cast<qint64>(std::llround(seconds * 1000));
        return true;
    });
}

CandidateField<QUrl> location(const QJsonValue& value, const QUrl& base)
{
    return read<QUrl>(value, [&base](const QJsonValue& v, QUrl& out) {
        if(!v.isString() || v.toString().isEmpty()) {
            return false;
        }
        const QUrl supplied(v.toString(), QUrl::StrictMode);
        out = base.resolved(supplied);
        return out.isValid() && !out.host().isEmpty() && out.userInfo().isEmpty()
            && (out.scheme() == u"https" || out.scheme() == u"http");
    });
}

CandidateField<QString> streamLocator(const QJsonValue& value)
{
    return read<QString>(value, [](const QJsonValue& input, QString& out) {
        if(!input.isString() || input.toString().isEmpty()) {
            return false;
        }
        out = input.toString();
        const QUrl url(out, QUrl::StrictMode);
        return url.isValid() && url.scheme() == u"https" && !url.host().isEmpty() && url.userInfo().isEmpty()
            && !url.hasFragment();
    });
}

CandidateField<SourceKind> kind(const QJsonValue& value)
{
    return read<SourceKind>(value, [](const QJsonValue& v, SourceKind& out) {
        if(!v.isString()) {
            return false;
        }
        if(v.toString() == u"album" || v.toString() == u"a") {
            out = SourceKind::Album;
        }
        else if(v.toString() == u"track" || v.toString() == u"t") {
            out = SourceKind::Track;
        }
        else {
            return false;
        }
        return true;
    });
}

QString describe(const QString& value)
{
    return safeText(value);
}
QString describe(const QUrl& value)
{
    return redactUrlForLog(value);
}
QString describe(const QDate& value)
{
    return value.toString(Qt::ISODate);
}
QString describe(qint64 value)
{
    return QString::number(value);
}
QString describe(SourceKind value)
{
    return value == SourceKind::Album ? QStringLiteral("album") : QStringLiteral("track");
}

template <typename T>
QString describe(const CandidateField<T>& value)
{
    if(value.isUsable()) {
        return describe(value.value);
    }
    return value.state == FieldState::Invalid ? QStringLiteral("<invalid>") : QStringLiteral("<unset>");
}

class Mapper
{
public:
    explicit Mapper(CorrelationContext context)
        : m_context{std::move(context)}
    { }

    void trackContext(qint64 id)
    {
        m_context = m_context.withTrack(id);
    }

    template <typename T>
    CandidateField<T> select(const QString& field, CandidateField<T> primary, const QString& primaryPath,
                             CandidateField<T> fallback = {}, const QString& fallbackPath = {},
                             FieldProvenance fallbackProvenance = FieldProvenance::Fallback,
                             bool identityAliases               = false)
    {
        const bool conflict = primary.isUsable() && fallback.isUsable() && primary.value != fallback.value;
        if(conflict) {
            qCDebug(BANDCAMP_PARSER).noquote() << formatTraceEvent(
                m_context, QStringLiteral("source_conflict"),
                {{QStringLiteral("field"), field},
                 {QStringLiteral("primary_path"), primaryPath},
                 {QStringLiteral("primary"), describe(primary)},
                 {QStringLiteral("alternate_path"), fallbackPath},
                 {QStringLiteral("alternate"), describe(fallback)},
                 {QStringLiteral("chosen"), identityAliases ? QStringLiteral("<invalid>") : primaryPath},
                 {QStringLiteral("reason"), identityAliases ? QStringLiteral("identity aliases disagree")
                                                            : QStringLiteral("usable primary takes precedence")}});
        }
        auto chosen  = primary;
        QString path = primaryPath;
        if(conflict && identityAliases) {
            chosen = CandidateField<T>::invalid();
        }
        else if(!chosen.isUsable() && fallback.isUsable()) {
            chosen            = fallback;
            chosen.provenance = fallbackProvenance;
            path              = fallbackPath;
        }
        if(!chosen.isUsable()) {
            if(fallback.state == FieldState::Invalid) {
                chosen.state = FieldState::Invalid;
            }
            qCDebug(BANDCAMP_PARSER).noquote() << formatTraceEvent(m_context, QStringLiteral("source_unresolved"),
                                                                   {{QStringLiteral("field"), field},
                                                                    {QStringLiteral("primary_path"), primaryPath},
                                                                    {QStringLiteral("primary"), describe(primary)},
                                                                    {QStringLiteral("alternate_path"), fallbackPath},
                                                                    {QStringLiteral("alternate"), describe(fallback)}});
        }
        else {
            qCDebug(BANDCAMP_TRACE_PARSER).noquote() << formatTraceEvent(
                m_context, QStringLiteral("source_mapping"),
                {{QStringLiteral("field"), field},
                 {QStringLiteral("old"), path == primaryPath ? QStringLiteral("<unset>") : describe(primary)},
                 {QStringLiteral("new"), describe(chosen)},
                 {QStringLiteral("source"), path},
                 {QStringLiteral("reason"),
                  path == primaryPath ? QStringLiteral("local extraction") : QStringLiteral("usable local fallback")}});
        }
        return chosen;
    }

    template <typename T>
    CandidateField<T> owned(const QString& field, CandidateField<T> value, const QString& path)
    {
        return select(field, std::move(value), path);
    }

    TrackCandidate row(const QJsonValue& value, const QJsonValue& numbers, qsizetype index)
    {
        TrackCandidate track;
        track.declaredIndex = index;
        const QString path  = QStringLiteral("$.trackinfo[%1]").arg(index);
        Mapper scoped(m_context.withTrack(0));
        if(!value.isObject()) {
            track.recordState = FieldState::Invalid;
            track.id          = CandidateField<qint64>::invalid();
            scoped.owned(QStringLiteral("member"), track.id, path);
            return track;
        }
        track.recordState = FieldState::Usable;
        const SourceObject object{value.toObject(), numbers.toObject()};
        const auto id    = object.integer(u"id");
        const auto alias = object.integer(u"track_id");
        // Contradictory stable aliases do not identify a usable member.
        track.id = scoped.select(QStringLiteral("track.id"), id, path + QStringLiteral(".id"), alias,
                                 path + QStringLiteral(".track_id"), FieldProvenance::Explicit, true);
        if(track.id.isUsable()) {
            scoped.trackContext(track.id.value);
        }
        track.title  = scoped.owned(QStringLiteral("track.title"), text(object.value(u"title")),
                                    path + QStringLiteral(".title"));
        track.artist = scoped.owned(QStringLiteral("track.artist"), text(object.value(u"artist")),
                                    path + QStringLiteral(".artist"));
        track.trackNumber
            = scoped.owned(QStringLiteral("track.trackNumber"), object.integer(u"track_num", MaxTrackNumber),
                           path + QStringLiteral(".track_num"));
        track.durationMs = scoped.owned(QStringLiteral("track.durationMs"), duration(object.value(u"duration")),
                                        path + QStringLiteral(".duration"));
        const auto file  = object.value(u"file");
        auto stream      = file.isUndefined() ? CandidateField<QString>::missing() : CandidateField<QString>::invalid();
        if(file.isObject()) {
            stream = streamLocator(file.toObject().value(u"mp3-128"));
        }
        track.streamUrl
            = scoped.owned(QStringLiteral("track.streamUrl"), stream, path + QStringLiteral(".file['mp3-128']"));
        return track;
    }

private:
    CorrelationContext m_context;
};

template <typename T>
bool conflicts(const CandidateField<T>& a, const CandidateField<T>& b)
{
    return a.isUsable() && b.isUsable() && a.value != b.value;
}

struct HtmlParserContextDeleter
{
    void operator()(htmlParserCtxtPtr context) const
    {
        if(context) {
            htmlFreeParserCtxt(context);
        }
    }
};

struct HtmlDocumentDeleter
{
    void operator()(xmlDocPtr document) const
    {
        if(document) {
            xmlFreeDoc(document);
        }
    }
};

struct HtmlStringDeleter
{
    void operator()(xmlChar* value) const
    {
        if(value) {
            xmlFree(value);
        }
    }
};

using HtmlParserContext = std::unique_ptr<_xmlParserCtxt, HtmlParserContextDeleter>;
using HtmlDocument      = std::unique_ptr<_xmlDoc, HtmlDocumentDeleter>;
using HtmlString        = std::unique_ptr<xmlChar, HtmlStringDeleter>;

bool isIncompleteHtmlError(int code)
{
    switch(code) {
        case XML_ERR_ATTRIBUTE_NOT_FINISHED:
        case XML_ERR_ATTRIBUTE_REDEFINED:
        case XML_ERR_CDATA_NOT_FINISHED:
        case XML_ERR_COMMENT_NOT_FINISHED:
        case XML_ERR_DOCTYPE_NOT_FINISHED:
        case XML_ERR_ENTITY_NOT_FINISHED:
        case XML_ERR_GT_REQUIRED:
        case XML_ERR_LITERAL_NOT_FINISHED:
        case XML_ERR_PI_NOT_FINISHED:
        case XML_ERR_STRING_NOT_CLOSED:
        case XML_ERR_TAG_NOT_FINISHED:
            return true;
        default:
            return false;
    }
}

struct HtmlIssues
{
    bool incomplete{false};
    bool resourceLimit{false};
};

void recordHtmlError(void* context, const char*, ...)
{
    auto* parser          = static_cast<xmlParserCtxtPtr>(context);
    const xmlError* error = parser ? xmlCtxtGetLastError(parser) : nullptr;
    if(!parser || !parser->_private || !error) {
        return;
    }
    auto& issues = *static_cast<HtmlIssues*>(parser->_private);
    issues.incomplete |= isIncompleteHtmlError(error->code);
    issues.resourceLimit |= error->code == XML_ERR_INTERNAL_ERROR || error->code == XML_ERR_NO_MEMORY;
}

enum class HtmlAttributeStatus : quint8
{
    Valid,
    ResourceLimit,
    Malformed,
    Cancelled
};

using HtmlAttributes = QHash<QString, QString>;

HtmlAttributeStatus readHtmlAttributes(xmlDocPtr document, xmlNodePtr node, HtmlAttributes& output,
                                       qsizetype& sourceRecords, const std::atomic_bool* stop)
{
    output.clear();
    for(xmlAttrPtr attribute = node->properties; attribute; attribute = attribute->next) {
        if(stopRequested(stop)) {
            return HtmlAttributeStatus::Cancelled;
        }
        if(++sourceRecords > MaxSourceRecords) {
            return HtmlAttributeStatus::ResourceLimit;
        }

        const QString name = QString::fromUtf8(reinterpret_cast<const char*>(attribute->name)).toLower();
        if(name.size() > MaxSourceFieldChars) {
            return HtmlAttributeStatus::ResourceLimit;
        }
        if(name == u"data-blob"_s) {
            continue;
        }

        HtmlString rawValue{xmlNodeListGetString(document, attribute->children, 1)};
        if(stopRequested(stop)) {
            return HtmlAttributeStatus::Cancelled;
        }
        const QString value   = rawValue ? QString::fromUtf8(reinterpret_cast<const char*>(rawValue.get()),
                                                             static_cast<qsizetype>(xmlStrlen(rawValue.get())))
                                         : QString{};
        const qsizetype limit = name == u"data-client-items"_s ? MaxPageBytes : MaxSourceFieldChars;
        if(value.size() > limit) {
            return HtmlAttributeStatus::ResourceLimit;
        }
        if(output.contains(name)) {
            return HtmlAttributeStatus::Malformed;
        }
        output.insert(name, value);
    }
    return HtmlAttributeStatus::Valid;
}

QString htmlAttribute(xmlNodePtr node, const char* name)
{
    HtmlString value{xmlGetProp(node, reinterpret_cast<const xmlChar*>(name))};
    return value ? QString::fromUtf8(reinterpret_cast<const char*>(value.get())) : QString{};
}

bool listingIdentity(const QString& value, BandcampPageKind& kind, qint64& id)
{
    QStringView digits;
    if(value.startsWith(u"album-"_s)) {
        kind   = BandcampPageKind::Album;
        digits = QStringView(value).mid(6);
    }
    else if(value.startsWith(u"track-"_s)) {
        kind   = BandcampPageKind::Track;
        digits = QStringView(value).mid(6);
    }
    else {
        return false;
    }
    if(digits.isEmpty() || (digits.size() > 1 && digits.at(0) == u'0')) {
        return false;
    }
    for(QChar ch : digits) {
        if(ch < u'0' || ch > u'9') {
            return false;
        }
    }
    bool ok{false};
    id = digits.toString().toLongLong(&ok);
    return ok && id > 0;
}

ResolutionFailure listingFailure(ResolutionErrorCode code, const QUrl& url, const QString& field, const QString& reason)
{
    return {{code, ResolutionStage::Parse, url, 0, field, reason}};
}

bool unsupportedContinuationElement(const QString& tagName, const HtmlAttributes& attributes, bool musicGrid)
{
    if(attributes.contains(QStringLiteral("data-client-items")) && !musicGrid) {
        return true;
    }
    for(const QString& name : {QStringLiteral("data-next-page"), QStringLiteral("data-load-more"),
                               QStringLiteral("data-has-more"), QStringLiteral("data-continuation")}) {
        if(attributes.contains(name)) {
            return true;
        }
    }

    const QString marker
        = (attributes.value(QStringLiteral("id")) + u' ' + attributes.value(QStringLiteral("class"))).toLower();
    if(marker.contains(u"load-more") || marker.contains(u"load_more") || marker.contains(u"loadmore")) {
        return true;
    }
    if(tagName == u"script"_s) {
        const QString type = attributes.value(QStringLiteral("type")).toLower();
        const QString id   = attributes.value(QStringLiteral("id")).toLower();
        return (type == u"text/template" || type == u"text/html")
            && (id.contains(u"music-grid") || id.contains(u"music-list") || id.contains(u"discography"));
    }
    return false;
}

using ClientItemsOutcome = std::variant<QList<BandcampListingReference>, ResolutionFailure, ResolutionCancelled>;

ClientItemsOutcome parseClientItems(const QString& json, const QUrl& pageUrl, const std::atomic_bool* stop)
{
    const auto malformed = [&pageUrl](const QString& field, const QString& reason) {
        return ClientItemsOutcome{listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, field, reason)};
    };
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(!withinDepth(json, stop)) {
        return stopRequested(stop)
                 ? ClientItemsOutcome{ResolutionCancelled{}}
                 : ClientItemsOutcome{listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl,
                                                     QStringLiteral("listing.clientItems"),
                                                     QStringLiteral("embedded listing JSON depth limit exceeded"))};
    }

    const QByteArray encoded = json.toUtf8();
    if(encoded.size() > MaxPageBytes) {
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl,
                              QStringLiteral("listing.clientItems"),
                              QStringLiteral("embedded listing JSON byte limit exceeded"));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &error);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(error.error != QJsonParseError::NoError || !document.isArray()) {
        return malformed(QStringLiteral("listing.clientItems"),
                         QStringLiteral("embedded listing data is not an array"));
    }

    qsizetype fieldCount   = 0;
    const QJsonArray items = document.array();
    if(!withinFields(items, fieldCount, stop)) {
        return stopRequested(stop)
                 ? ClientItemsOutcome{ResolutionCancelled{}}
                 : ClientItemsOutcome{listingFailure(
                       ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.clientItems"),
                       QStringLiteral("embedded listing field or record limit exceeded"))};
    }
    const QJsonDocument numericDocument = numericLexemes(json, stop);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(!numericDocument.isArray() || numericDocument.array().size() != items.size()) {
        return malformed(QStringLiteral("listing.clientItems"),
                         QStringLiteral("embedded listing identities are invalid"));
    }
    const QJsonArray numericItems = numericDocument.array();
    QList<BandcampListingReference> references;
    references.reserve(items.size());
    for(qsizetype i = 0; i < items.size(); ++i) {
        if((i & 0x3ff) == 0 && stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        if(!items.at(i).isObject() || !numericItems.at(i).isObject()) {
            return malformed(QStringLiteral("listing.clientItems"), QStringLiteral("embedded entry is not an object"));
        }
        const SourceObject entry{items.at(i).toObject(), numericItems.at(i).toObject()};
        const QJsonValue typeValue = entry.value(u"type");
        if(!typeValue.isString()) {
            return malformed(QStringLiteral("listing.itemType"), QStringLiteral("embedded entry has an invalid type"));
        }
        BandcampPageKind kind;
        if(typeValue.toString() == u"album"_s) {
            kind = BandcampPageKind::Album;
        }
        else if(typeValue.toString() == u"track"_s) {
            kind = BandcampPageKind::Track;
        }
        else {
            return malformed(QStringLiteral("listing.itemType"),
                             QStringLiteral("embedded entry has an unsupported type"));
        }

        const QJsonValue idValue        = entry.value(u"id");
        const CandidateField<qint64> id = entry.integer(u"id");
        if(!idValue.isDouble() || !id.isUsable()) {
            return malformed(QStringLiteral("listing.itemId"),
                             QStringLiteral("embedded entry has an invalid identity"));
        }
        const QJsonValue pageUrlValue = entry.value(u"page_url");
        if(!pageUrlValue.isString()) {
            return malformed(QStringLiteral("listing.itemUrl"),
                             QStringLiteral("embedded entry has no release location"));
        }
        const CandidateField<QUrl> locationValue = location(pageUrlValue, pageUrl);
        if(!locationValue.isUsable() || locationValue.value.host().compare(pageUrl.host(), Qt::CaseInsensitive) != 0) {
            return malformed(QStringLiteral("listing.itemUrl"),
                             QStringLiteral("embedded entry has an invalid or external release location"));
        }
        const BandcampPageUrl parsed = parseBandcampPageUrl(locationValue.value.toString(QUrl::FullyEncoded));
        if(!parsed.isValid() || parsed.kind != kind) {
            return malformed(QStringLiteral("listing.itemUrl"),
                             QStringLiteral("embedded entry location disagrees with its type"));
        }
        references.append({i, id.value, parsed});
    }
    return references;
}
} // namespace

ReleaseParseOutcome parseReleasePage(const QByteArray& body, const QUrl& pageUrl, CorrelationContext context,
                                     const std::atomic_bool* stop)
{
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    context.resourceUrl = pageUrl;
    const auto decoded  = extractTralbum(body, pageUrl, stop);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(const auto* error = std::get_if<ResolutionFailure>(&decoded)) {
        return *error;
    }
    const auto& source       = std::get<DecodedSource>(decoded);
    const auto& decodedValue = source.value;
    logSnapshot(decodedValue, context, stop);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(!decodedValue.isObject()) {
        return failure(ResolutionErrorCode::MalformedPage, pageUrl, {},
                       QStringLiteral("release JSON must be an object"));
    }
    const auto rootObject = decodedValue.toObject();
    qsizetype records     = 0;
    if(!withinFields(rootObject, records, stop)) {
        if(stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        return failure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, {},
                       QStringLiteral("source field or record limit exceeded"));
    }
    const auto numbers = numericLexemes(source.json, stop);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    const SourceObject root{rootObject, numbers.object()};
    const auto currentValue = root.value(u"current");
    if(!currentValue.isUndefined() && !currentValue.isObject()) {
        return failure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("identity"),
                       QStringLiteral("invalid current object"));
    }
    const SourceObject current{currentValue.toObject(), root.numbers.value(u"current").toObject()};
    Mapper mapper(context);
    const auto currentKind = kind(current.value(u"type"));
    const auto rootKind    = kind(root.value(u"item_type"));
    const auto sourceKind  = mapper.select(QStringLiteral("kind"), currentKind, QStringLiteral("$.current.type"),
                                           rootKind, QStringLiteral("$.item_type"), FieldProvenance::Explicit, true);
    const auto currentId   = current.integer(u"id");
    const auto rootId      = root.integer(u"id");
    const auto id          = mapper.select(QStringLiteral("id"), currentId, QStringLiteral("$.current.id"), rootId,
                                           QStringLiteral("$.id"), FieldProvenance::Explicit, true);
    if(!sourceKind.isUsable() || conflicts(currentKind, rootKind) || conflicts(currentId, rootId)) {
        return failure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("identity"),
                       QStringLiteral("unknown or contradictory release identity"));
    }
    const auto infoValue = root.value(u"trackinfo");
    if(!infoValue.isArray()) {
        return failure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("members"),
                       QStringLiteral("trackinfo must be an array"));
    }
    const auto info        = infoValue.toArray();
    const auto infoNumbers = root.numbers.value(u"trackinfo").toArray();
    if(info.size() > MaxDeclaredTracks) {
        return failure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("members"),
                       QStringLiteral("declared track limit exceeded"));
    }
    const auto canonical = pageUrl.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment);
    auto sourceUrl       = mapper.owned(QStringLiteral("sourceUrl"), location(canonical.toString(), {}),
                                        QStringLiteral("<page location>"));
    sourceUrl.provenance = FieldProvenance::Derived;
    if(sourceKind.value == SourceKind::Album) {
        AlbumCandidate album;
        album.id        = id;
        album.sourceUrl = sourceUrl;
        album.title     = mapper.owned(QStringLiteral("album.title"), text(current.value(u"title")),
                                       QStringLiteral("$.current.title"));
        album.artist    = mapper.select(QStringLiteral("album.artist"), text(current.value(u"artist")),
                                        QStringLiteral("$.current.artist"), text(root.value(u"artist")),
                                        QStringLiteral("$.artist"));
        album.releaseDate
            = mapper.select(QStringLiteral("album.releaseDate"), date(current.value(u"release_date")),
                            QStringLiteral("$.current.release_date"), date(root.value(u"album_release_date")),
                            QStringLiteral("$.album_release_date"));
        album.tracks.reserve(info.size());
        for(qsizetype i = 0; i < info.size(); ++i) {
            if(stopRequested(stop)) {
                return ResolutionCancelled{};
            }
            album.tracks.append(mapper.row(info.at(i), infoNumbers.at(i), i));
        }
        qCInfo(BANDCAMP_PARSER).noquote()
            << formatTraceEvent(context, QStringLiteral("parsed_album"),
                                {DiagnosticField::number(QStringLiteral("declared_tracks"), info.size())});
        return ResolutionSuccess<ReleaseCandidate>{std::move(album)};
    }

    TrackPageCandidate page;
    page.sourceUrl = sourceUrl;
    if(id.isUsable()) {
        mapper.trackContext(id.value);
        context = context.withTrack(id.value);
    }
    TrackCandidate matched;
    for(qsizetype i = 0; i < info.size(); ++i) {
        if(stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        auto row = mapper.row(info.at(i), infoNumbers.at(i), i);
        if(id.isUsable() && row.id.isUsable() && row.id.value == id.value) {
            if(page.matchingRecord.state != FieldState::Missing) {
                page.matchingRecord = CandidateField<qsizetype>::invalid();
                matched             = {};
            }
            else {
                page.matchingRecord = CandidateField<qsizetype>::usable(i, FieldProvenance::Explicit);
                matched             = std::move(row);
            }
        }
    }
    mapper.owned(QStringLiteral("matchingRecord"), page.matchingRecord, QStringLiteral("$.trackinfo[*].id/track_id"));
    page.track            = std::move(matched);
    page.track.id         = id;
    const QString rowPath = page.matchingRecord.isUsable()
                              ? QStringLiteral("$.trackinfo[%1]").arg(page.matchingRecord.value)
                              : QStringLiteral("$.trackinfo[unmatched]");
    page.track.title
        = mapper.select(QStringLiteral("track.title"), text(current.value(u"title")), QStringLiteral("$.current.title"),
                        page.track.title, rowPath + QStringLiteral(".title"), FieldProvenance::Explicit);
    page.track.artist = mapper.select(QStringLiteral("track.artist"), text(current.value(u"artist")),
                                      QStringLiteral("$.current.artist"), page.track.artist,
                                      rowPath + QStringLiteral(".artist"), FieldProvenance::Explicit);
    page.track.trackNumber
        = mapper.select(QStringLiteral("track.trackNumber"), current.integer(u"track_number", MaxTrackNumber),
                        QStringLiteral("$.current.track_number"), page.track.trackNumber,
                        rowPath + QStringLiteral(".track_num"), FieldProvenance::Explicit);
    page.track.releaseDate = mapper.owned(QStringLiteral("track.releaseDate"), date(current.value(u"release_date")),
                                          QStringLiteral("$.current.release_date"));
    page.fallbackArtist
        = mapper.owned(QStringLiteral("fallbackArtist"), text(root.value(u"artist")), QStringLiteral("$.artist"));
    page.fallbackArtist.provenance = FieldProvenance::Fallback;

    const auto parentId  = current.value(u"album_id");
    page.parent.albumUrl = mapper.owned(QStringLiteral("parent.albumUrl"), location(root.value(u"album_url"), pageUrl),
                                        QStringLiteral("$.album_url"));
    if(current.explicitNoParent(u"album_id")) {
        page.parent.declaration = ParentDeclaration::NoParent;
    }
    else {
        page.parent.albumId = mapper.owned(QStringLiteral("parent.albumId"), current.integer(u"album_id"),
                                           QStringLiteral("$.current.album_id"));
        if(page.parent.albumId.isUsable()) {
            page.parent.declaration = ParentDeclaration::AlbumBacked;
        }
        else if(!parentId.isUndefined()) {
            page.parent.declaration = ParentDeclaration::Invalid;
        }
    }
    qCDebug(BANDCAMP_TRACE_PARSER).noquote() << formatTraceEvent(
        context, QStringLiteral("source_mapping"),
        {{QStringLiteral("field"), QStringLiteral("parent.declaration")},
         {QStringLiteral("old"), QStringLiteral("<unset>")},
         {QStringLiteral("new"), page.parent.declaration == ParentDeclaration::NoParent ? QStringLiteral("no_parent")
                                 : page.parent.declaration == ParentDeclaration::AlbumBacked
                                     ? QStringLiteral("album_backed")
                                 : page.parent.declaration == ParentDeclaration::Invalid ? QStringLiteral("<invalid>")
                                                                                         : QStringLiteral("<unset>")},
         {QStringLiteral("source"), QStringLiteral("$.current.album_id")},
         {QStringLiteral("reason"),
          QStringLiteral("declared membership evidence; location does not establish absence")}});
    qCInfo(BANDCAMP_PARSER).noquote() << formatTraceEvent(
        context, QStringLiteral("parsed_track"),
        {DiagnosticField::number(QStringLiteral("matching_record"), page.matchingRecord.isUsable() ? 1 : 0)});
    return ResolutionSuccess<ReleaseCandidate>{std::move(page)};
}

BandcampListingParseOutcome parseDiscographyPage(const QByteArray& body, const QUrl& pageUrl,
                                                 CorrelationContext context, const std::atomic_bool* stop)
{
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    context.resourceUrl = pageUrl;
    const auto decoded  = extractTralbum(body, pageUrl, stop);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(const auto* error = std::get_if<ResolutionFailure>(&decoded)) {
        return *error;
    }
    const auto& source       = std::get<DecodedSource>(decoded);
    const auto& decodedValue = source.value;
    logSnapshot(decodedValue, context, stop);
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(!decodedValue.isObject()) {
        return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing"),
                              QStringLiteral("discography JSON must be an object"));
    }
    qsizetype records = 0;
    if(!withinFields(decodedValue, records, stop)) {
        if(stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing"),
                              QStringLiteral("source field or record limit exceeded"));
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    {
        [[maybe_unused]] const QString utf8Check = decoder(body);
        if(decoder.hasError()) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing"),
                                  QStringLiteral("invalid listing UTF-8"));
        }
    }

    HtmlIssues htmlIssues;
    HtmlParserContext parser{htmlNewParserCtxt()};
    if(!parser) {
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("could not allocate the HTML parser context"));
    }
    parser->_private        = &htmlIssues;
    parser->sax->warning    = recordHtmlError;
    parser->sax->error      = recordHtmlError;
    parser->sax->fatalError = recordHtmlError;

    BandcampListingBatch batch;
    batch.effectiveUrl          = pageUrl;
    const QByteArray encodedUrl = pageUrl.toString(QUrl::FullyEncoded).toUtf8();
    constexpr int parserOptions = HTML_PARSE_NONET | HTML_PARSE_COMPACT;
    HtmlDocument document{htmlCtxtReadMemory(parser.get(), body.constData(), static_cast<int>(body.size()),
                                             encodedUrl.constData(), "UTF-8", parserOptions)};
    if(stopRequested(stop)) {
        return ResolutionCancelled{};
    }
    if(!document) {
        return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("libxml2 could not recover the listing document"));
    }
    if(htmlIssues.resourceLimit) {
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("HTML nesting limit exceeded"));
    }
    if(htmlIssues.incomplete) {
        return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("listing markup ends inside an incomplete construct"));
    }

    using XPathContext = std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)>;
    using XPathObject  = std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)>;
    XPathContext xpath{xmlXPathNewContext(document.get()), xmlXPathFreeContext};
    if(!xpath) {
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("could not allocate the HTML query context"));
    }
    const auto select = [&xpath](const char* expression) {
        return XPathObject{xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(expression), xpath.get()),
                           xmlXPathFreeObject};
    };
    const auto queryFailure
        = listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                         QStringLiteral("could not evaluate the HTML selection"));
    const auto nodeCount = select("count(//node()) + count(//@*)");
    if(!nodeCount) {
        return queryFailure;
    }
    if(nodeCount->floatval > MaxSourceRecords) {
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("HTML node and attribute limit exceeded"));
    }
    const QByteArray depthExpression = "//*[count(ancestor::*) > " + QByteArray::number(MaxHtmlDepth) + "]";
    const auto excessiveDepth        = select(depthExpression.constData());
    if(!excessiveDepth) {
        return queryFailure;
    }
    if(excessiveDepth->nodesetval && excessiveDepth->nodesetval->nodeNr > 0) {
        return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                              QStringLiteral("HTML nesting limit exceeded"));
    }
    const auto grids = select("//ol[@id='music-grid']");
    if(!grids) {
        return queryFailure;
    }
    if(!grids->nodesetval || grids->nodesetval->nodeNr != 1) {
        return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing"),
                              QStringLiteral("exactly one structured music listing is required"));
    }
    xmlNodePtr grid               = grids->nodesetval->nodeTab[0];
    const bool hasClientItems     = xmlHasProp(grid, reinterpret_cast<const xmlChar*>("data-client-items")) != nullptr;
    const QString clientItemsJson = htmlAttribute(grid, "data-client-items");
    const auto elements           = select("//*");
    if(!elements) {
        return queryFailure;
    }
    qsizetype sourceRecords = 0;
    for(int i = 0; elements->nodesetval && i < elements->nodesetval->nodeNr; ++i) {
        if(stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        xmlNodePtr node = elements->nodesetval->nodeTab[i];
        HtmlAttributes attributes;
        const HtmlAttributeStatus status = readHtmlAttributes(document.get(), node, attributes, sourceRecords, stop);
        if(status == HtmlAttributeStatus::Cancelled || stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        if(status == HtmlAttributeStatus::ResourceLimit) {
            return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl, QStringLiteral("listing.html"),
                                  QStringLiteral("HTML attribute or record limit exceeded"));
        }
        if(status == HtmlAttributeStatus::Malformed) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.html"),
                                  QStringLiteral("listing contains duplicate relevant attributes"));
        }

        const QString tagName = QString::fromUtf8(reinterpret_cast<const char*>(node->name)).toLower();
        const bool isGrid     = node == grid;

        if(unsupportedContinuationElement(tagName, attributes, isGrid)) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.continuation"),
                                  QStringLiteral("recognized embedded continuation data is unsupported"));
        }
    }
    const auto nextPages = select("//*[self::a or self::link][contains(concat(' ', normalize-space(translate(@rel, "
                                  "'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')), ' '), ' next ')]");
    if(!nextPages) {
        return queryFailure;
    }
    for(int i = 0; nextPages->nodesetval && i < nextPages->nodesetval->nodeNr; ++i) {
        if(stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        const QString href = htmlAttribute(nextPages->nodesetval->nodeTab[i], "href");
        if(href.isEmpty()) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.continuation"),
                                  QStringLiteral("next-page link has no location"));
        }
        QUrl next                    = pageUrl.resolved(QUrl(href, QUrl::StrictMode));
        const BandcampPageUrl parsed = parseBandcampPageUrl(next.toString(QUrl::FullyEncoded));
        if(!next.isValid() || next.host().compare(pageUrl.host(), Qt::CaseInsensitive) != 0
           || !next.userInfo().isEmpty() || !parsed.isValid() || parsed.kind != BandcampPageKind::Discography) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.continuation"),
                                  QStringLiteral("next-page link is not a same-site discography"));
        }
        next.setFragment(QString());
        if(batch.continuation && batch.continuation->locator != next) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.continuation"),
                                  QStringLiteral("music listing has conflicting next-page links"));
        }
        batch.continuation = BandcampListingContinuation{std::move(next)};
    }

    xpath->node     = grid;
    const auto rows = select("./li");
    if(!rows) {
        return queryFailure;
    }
    QSet<QString> listedReferences;
    const auto appendReference = [&](qint64 id, const BandcampPageUrl& page) {
        const QString key = QString::number(static_cast<int>(page.kind)) + u':' + QString::number(id) + u':' + page.url;
        if(listedReferences.contains(key)) {
            return;
        }
        listedReferences.insert(key);
        const qsizetype ordinal = batch.references.size();
        batch.references.append({ordinal, id, page});
        const CorrelationContext itemContext = context.withTrack(page.kind == BandcampPageKind::Track ? id : 0);
        qCDebug(BANDCAMP_TRACE_PARSER).noquote()
            << formatTraceEvent(itemContext, QStringLiteral("listing_reference"),
                                {DiagnosticField::number(QStringLiteral("ordinal"), ordinal),
                                 DiagnosticField::text(QStringLiteral("kind"), pageKindName(page.kind)),
                                 DiagnosticField::number(QStringLiteral("id"), id),
                                 DiagnosticField::text(QStringLiteral("url"), page.url)});
    };
    for(int i = 0; rows->nodesetval && i < rows->nodesetval->nodeNr; ++i) {
        if(stopRequested(stop)) {
            return ResolutionCancelled{};
        }
        xmlNodePtr row           = rows->nodesetval->nodeTab[i];
        const QString declaredId = htmlAttribute(row, "data-item-id");
        BandcampPageKind kind{BandcampPageKind::Discography};
        qint64 id{0};
        if(declaredId.isEmpty() || !listingIdentity(declaredId, kind, id)) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.itemId"),
                                  QStringLiteral("music listing entry has an invalid identity"));
        }

        xpath->node        = row;
        const auto anchors = select(".//a[@href]");
        if(!anchors) {
            return queryFailure;
        }
        if(!anchors->nodesetval || anchors->nodesetval->nodeNr == 0) {
            return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.itemUrl"),
                                  QStringLiteral("music listing entry has no release link"));
        }
        std::optional<BandcampPageUrl> resolvedPage;
        for(int j = 0; j < anchors->nodesetval->nodeNr; ++j) {
            if(stopRequested(stop)) {
                return ResolutionCancelled{};
            }
            const QString link           = htmlAttribute(anchors->nodesetval->nodeTab[j], "href");
            const QUrl target            = pageUrl.resolved(QUrl(link, QUrl::StrictMode));
            const BandcampPageUrl parsed = parseBandcampPageUrl(target.toString(QUrl::FullyEncoded));
            if(!target.isValid() || target.host().compare(pageUrl.host(), Qt::CaseInsensitive) != 0
               || !target.userInfo().isEmpty() || !parsed.isValid() || parsed.kind != kind) {
                return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.itemUrl"),
                                      QStringLiteral("music listing entry has an invalid release link"));
            }
            if(resolvedPage && resolvedPage->url != parsed.url) {
                return listingFailure(ResolutionErrorCode::MalformedPage, pageUrl, QStringLiteral("listing.itemUrl"),
                                      QStringLiteral("music listing entry has conflicting release links"));
            }
            resolvedPage = parsed;
        }
        if(batch.references.size() >= MaxSourceRecords) {
            return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl,
                                  QStringLiteral("listing.entries"),
                                  QStringLiteral("music listing record limit exceeded"));
        }
        appendReference(id, *resolvedPage);
    }

    if(hasClientItems) {
        ClientItemsOutcome embedded = parseClientItems(clientItemsJson, pageUrl, stop);
        if(std::holds_alternative<ResolutionCancelled>(embedded)) {
            return ResolutionCancelled{};
        }
        if(const auto* failure = std::get_if<ResolutionFailure>(&embedded)) {
            return *failure;
        }
        const auto& references = std::get<QList<BandcampListingReference>>(embedded);
        if(references.size() > MaxSourceRecords - batch.references.size()) {
            return listingFailure(ResolutionErrorCode::ResourceLimitExceeded, pageUrl,
                                  QStringLiteral("listing.entries"),
                                  QStringLiteral("music listing record limit exceeded"));
        }
        for(const BandcampListingReference& reference : references) {
            appendReference(reference.id, reference.page);
        }
    }
    if(batch.references.isEmpty()) {
        return listingFailure(ResolutionErrorCode::NoResolvableTracks, pageUrl, QStringLiteral("listing"),
                              QStringLiteral("music listing contains no release references"));
    }

    if(batch.continuation) {
        qCDebug(BANDCAMP_TRACE_PARSER).noquote() << formatTraceEvent(
            context, QStringLiteral("listing_continuation"),
            {DiagnosticField::text(QStringLiteral("url"), redactUrlForLog(batch.continuation->locator))});
    }
    qCInfo(BANDCAMP_PARSER).noquote() << formatTraceEvent(
        context, QStringLiteral("parsed_discography"),
        {DiagnosticField::number(QStringLiteral("references"), batch.references.size()),
         DiagnosticField::number(QStringLiteral("has_continuation"), batch.continuation.has_value() ? 1 : 0)});
    return ResolutionSuccess<BandcampListingBatch>{std::move(batch)};
}
} // namespace Fooyin::Bandcamp
