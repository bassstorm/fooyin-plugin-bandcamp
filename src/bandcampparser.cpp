#include "bandcampparser.h"

#include "bandcamplogging.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QStringDecoder>

#include <cmath>
#include <limits>

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
        album.artId
            = mapper.select(QStringLiteral("album.artId"), current.integer(u"art_id"),
                            QStringLiteral("$.current.art_id"), root.integer(u"art_id"), QStringLiteral("$.art_id"));
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
    page.track.artId
        = mapper.owned(QStringLiteral("track.artId"), current.integer(u"art_id"), QStringLiteral("$.current.art_id"));
    page.fallbackArtist
        = mapper.owned(QStringLiteral("fallbackArtist"), text(root.value(u"artist")), QStringLiteral("$.artist"));
    page.fallbackArtist.provenance = FieldProvenance::Fallback;
    page.fallbackArtId
        = mapper.owned(QStringLiteral("fallbackArtId"), root.integer(u"art_id"), QStringLiteral("$.art_id"));
    page.fallbackArtId.provenance = FieldProvenance::Fallback;

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
} // namespace Fooyin::Bandcamp
