#include "bandcampparser.h"

#include "bandcamplogging.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr qsizetype MaxTitleLength  = 1024;
constexpr qsizetype MaxArtistLength = 1024;
constexpr qsizetype MaxUrlLength    = 4096;
constexpr qsizetype MaxTypeLength   = 64;
constexpr qsizetype MaxDateLength   = 64;
constexpr qsizetype MaxEntityLength = 10;
constexpr double MaxSafeInteger     = 9007199254740991.0;
constexpr double MaxTrackNumber     = 100000.0;
constexpr double MaxDurationSeconds = 7.0 * 24.0 * 60.0 * 60.0;

QString cleanText(QString text)
{
    for(QChar& ch : text) {
        const ushort code = ch.unicode();
        if(code < 0x20 || code == 0x7F) {
            ch = QLatin1Char(' ');
        }
    }
    return text.simplified();
}

bool decodeEntity(QStringView name, QString& out)
{
    if(name == u"amp") {
        out = QStringLiteral("&");
        return true;
    }
    if(name == u"quot") {
        out = QStringLiteral("\"");
        return true;
    }
    if(name == u"apos") {
        out = QStringLiteral("'");
        return true;
    }
    if(name == u"lt") {
        out = QStringLiteral("<");
        return true;
    }
    if(name == u"gt") {
        out = QStringLiteral(">");
        return true;
    }

    if(name.isEmpty() || name.at(0) != u'#') {
        return false;
    }

    const bool hex           = name.size() > 1 && (name.at(1) == u'x' || name.at(1) == u'X');
    const QStringView digits = name.mid(hex ? 2 : 1);
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
    return true;
}

// Single pass, so "&amp;quot;" decodes to "&quot;" and not to a quote.
QString decodeHtmlEntities(const QString& text)
{
    QString out;
    out.reserve(text.size());

    qsizetype i = 0;
    while(i < text.size()) {
        if(text.at(i) != u'&') {
            out.append(text.at(i));
            ++i;
            continue;
        }

        // Bounded look-ahead keeps this linear even when a page has many '&' and no ';'.
        const qsizetype windowEnd = qMin(text.size(), i + MaxEntityLength + 2);
        qsizetype semi            = i + 1;
        while(semi < windowEnd && text.at(semi) != u';') {
            ++semi;
        }

        QString decoded;
        if(semi < windowEnd && decodeEntity(QStringView(text).mid(i + 1, semi - i - 1), decoded)) {
            out.append(decoded);
            i = semi + 1;
        }
        else {
            out.append(text.at(i));
            ++i;
        }
    }
    return out;
}

} // namespace

QJsonObject extractTralbum(const QByteArray& body, QString& error)
{
    if(body.size() > MaxPageBytes) {
        error = QStringLiteral("page exceeds the size limit");
        return {};
    }

    static const QByteArray marker("data-tralbum=");
    const qsizetype markerPos = body.indexOf(marker);
    if(markerPos < 0) {
        error = QStringLiteral("no data-tralbum attribute found");
        return {};
    }

    const qsizetype quotePos = markerPos + marker.size();
    if(quotePos >= body.size()) {
        error = QStringLiteral("data-tralbum attribute is truncated");
        return {};
    }

    const char quote = body.at(quotePos);
    if(quote != '"' && quote != '\'') {
        error = QStringLiteral("data-tralbum attribute is not quoted");
        return {};
    }

    const qsizetype valueEnd = body.indexOf(quote, quotePos + 1);
    if(valueEnd < 0) {
        error = QStringLiteral("data-tralbum attribute is not terminated");
        return {};
    }
    if(valueEnd == quotePos + 1) {
        error = QStringLiteral("data-tralbum attribute is empty");
        return {};
    }

    const QByteArray raw = body.mid(quotePos + 1, valueEnd - quotePos - 1);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString escaped = decoder(raw);
    if(decoder.hasError()) {
        error = QStringLiteral("data-tralbum is not valid UTF-8");
        return {};
    }

    const QString json = decodeHtmlEntities(escaped);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if(parseError.error != QJsonParseError::NoError) {
        error = QStringLiteral("data-tralbum JSON is invalid: %1").arg(parseError.errorString());
        return {};
    }
    if(!doc.isObject()) {
        error = QStringLiteral("data-tralbum JSON is not an object");
        return {};
    }
    return doc.object();
}

namespace {
enum class FieldKind : quint8
{
    Text,
    Integer,
    Duration,
    Date,
    Url
};

// One row per field of the Bandcamp model. readField applies the same type checks, cleaning and
// length limits to every field. Fallbacks between fields belong to the callers.
struct Field
{
    QStringView key;
    FieldKind kind;
    bool required;
    qsizetype maxLength; // Text, Date and Url
    double maxValue;     // Integer
};

struct FieldValue
{
    QString text;
    qint64 number = 0; // Integer value, or milliseconds for Duration
    QDate date;
};

constexpr qsizetype MaxNumericTextLength = 32;

QString truncateText(QString text, qsizetype maxLength, QStringView key, QStringList& warnings)
{
    if(text.size() <= maxLength) {
        return text;
    }
    qsizetype length = maxLength;
    if(text.at(length - 1).isHighSurrogate()) {
        --length;
    }
    text.truncate(length);
    warnings.append(QStringLiteral("%1 truncated to %2 characters").arg(key).arg(maxLength));
    return text;
}

// Numbers may arrive as JSON numbers or as numeric text. Anything else is a warning and yields 0.
qint64 readInteger(const QJsonValue& value, const Field& field, QStringList& warnings)
{
    double number = 0.0;
    if(value.isDouble()) {
        number = value.toDouble();
    }
    else if(value.isString() && value.toString().size() <= MaxNumericTextLength) {
        bool ok = false;
        number  = QLocale::c().toDouble(value.toString().trimmed(), &ok);
        if(!ok) {
            warnings.append(QStringLiteral("%1 is not a number").arg(field.key));
            return 0;
        }
    }
    else {
        warnings.append(QStringLiteral("%1 is not a number").arg(field.key));
        return 0;
    }

    if(!std::isfinite(number) || number < 1.0 || number > field.maxValue || std::trunc(number) != number) {
        warnings.append(QStringLiteral("%1 is out of range").arg(field.key));
        return 0;
    }
    return static_cast<qint64>(number);
}

// Accepts plain seconds ("183.429") and clock notation ("3:03", "1:02:03.5").
bool parseDurationText(const QString& text, double& seconds)
{
    if(text.isEmpty() || text.size() > MaxNumericTextLength) {
        return false;
    }

    const QStringList parts = text.split(QLatin1Char(':'));
    if(parts.size() == 1) {
        bool ok = false;
        seconds = QLocale::c().toDouble(text, &ok);
        return ok;
    }
    if(parts.size() > 3) {
        return false;
    }

    seconds = 0.0;
    for(qsizetype i = 0; i < parts.size(); ++i) {
        const QString& part = parts.at(i);
        const bool isLast   = i == parts.size() - 1;
        if(part.isEmpty()) {
            return false;
        }
        // Only the seconds part may carry a fraction. Earlier parts are whole numbers.
        if(!isLast && !std::all_of(part.cbegin(), part.cend(), [](QChar ch) { return ch >= u'0' && ch <= u'9'; })) {
            return false;
        }

        bool ok            = false;
        const double value = QLocale::c().toDouble(part, &ok);
        if(!ok || !std::isfinite(value) || value < 0.0 || (i > 0 && value >= 60.0)) {
            return false;
        }
        seconds = seconds * 60.0 + value;
    }
    return true;
}

qint64 readDuration(const QJsonValue& value, const Field& field, QStringList& warnings)
{
    double seconds = 0.0;
    if(value.isDouble()) {
        seconds = value.toDouble();
    }
    else if(!value.isString() || !parseDurationText(value.toString().trimmed(), seconds)) {
        warnings.append(QStringLiteral("%1 is not a recognised duration").arg(field.key));
        return 0;
    }

    if(!std::isfinite(seconds) || seconds < 0.0 || seconds > MaxDurationSeconds) {
        warnings.append(QStringLiteral("%1 is out of range").arg(field.key));
        return 0;
    }
    return static_cast<qint64>(std::llround(seconds * 1000.0));
}

QDate readDate(const QJsonValue& value, const Field& field, QStringList& warnings)
{
    if(!value.isString()) {
        warnings.append(QStringLiteral("%1 is not a string").arg(field.key));
        return {};
    }
    const QString text = truncateText(cleanText(value.toString()), field.maxLength, field.key, warnings);
    if(text.isEmpty()) {
        return {};
    }

    // Bandcamp sends "15 Jan 2016 00:00:00 GMT". ISO dates are accepted as a fallback.
    QDate date = QLocale::c().toDate(text.section(QLatin1Char(' '), 0, 2), QStringLiteral("d MMM yyyy"));
    if(!date.isValid()) {
        date = QDate::fromString(text.left(10), Qt::ISODate);
    }
    if(!date.isValid()) {
        warnings.append(QStringLiteral("%1 is not a recognised date").arg(field.key));
    }
    return date;
}

// Accepts only absolute http(s) URLs with a host, resolved against the page URL.
QString resolveHttpUrl(const QString& raw, const QUrl& base, QStringView key, QStringList& warnings)
{
    if(raw.isEmpty()) {
        return {};
    }

    const QUrl url        = base.resolved(QUrl(raw, QUrl::StrictMode));
    const bool httpScheme = url.scheme() == u"https" || url.scheme() == u"http";
    if(!url.isValid() || !httpScheme || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        warnings.append(QStringLiteral("%1 is not a valid http(s) URL").arg(key));
        return {};
    }

    const QString text = url.toString(QUrl::FullyEncoded);
    if(text.size() > MaxUrlLength) {
        warnings.append(QStringLiteral("%1 is too long").arg(key));
        return {};
    }
    return text;
}

FieldValue readField(const QJsonObject& object, const Field& field, const QUrl& base, QStringList& warnings)
{
    FieldValue out;
    const QJsonValue value = object.value(field.key);
    if(value.isNull() || value.isUndefined()) {
        if(field.required) {
            warnings.append(QStringLiteral("%1 is missing").arg(field.key));
        }
        return out;
    }

    switch(field.kind) {
        case FieldKind::Text:
            if(value.isString()) {
                out.text = truncateText(cleanText(value.toString()), field.maxLength, field.key, warnings);
            }
            else {
                warnings.append(QStringLiteral("%1 is not a string").arg(field.key));
            }
            break;
        case FieldKind::Integer:
            out.number = readInteger(value, field, warnings);
            break;
        case FieldKind::Duration:
            out.number = readDuration(value, field, warnings);
            break;
        case FieldKind::Date:
            out.date = readDate(value, field, warnings);
            break;
        case FieldKind::Url:
            if(value.isString()) {
                out.text = resolveHttpUrl(value.toString(), base, field.key, warnings);
            }
            else {
                warnings.append(QStringLiteral("%1 is not a string").arg(field.key));
            }
            break;
    }
    return out;
}

bool isSet(const FieldValue& value)
{
    return !value.text.isEmpty() || value.number != 0 || value.date.isValid();
}

QString describe(const FieldValue& value, FieldKind kind)
{
    switch(kind) {
        case FieldKind::Integer:
        case FieldKind::Duration:
            return QString::number(value.number);
        case FieldKind::Date:
            return value.date.toString(Qt::ISODate);
        case FieldKind::Text:
        case FieldKind::Url:
            return value.text;
    }
    return {};
}

bool sameValue(const FieldValue& a, const FieldValue& b, FieldKind kind)
{
    switch(kind) {
        case FieldKind::Integer:
        case FieldKind::Duration:
            return a.number == b.number;
        case FieldKind::Date:
            return a.date == b.date;
        case FieldKind::Text:
        case FieldKind::Url:
            return a.text == b.text;
    }
    return false;
}

// Rule: current/<field> first, root/<field> as the fallback. The fallback is used when the primary value is empty.
// When both hold a value and they differ, the primary is kept and a warning is added. Only the parser sees both.
FieldValue readCurrentOrRoot(const QJsonObject& current, const QJsonObject& root, const Field& currentField,
                             const Field& rootField, const QUrl& base, QStringList& warnings)
{
    const FieldValue primary = readField(current, currentField, base, warnings);
    // Root warnings are kept only when the root value is used.
    QStringList rootWarnings;
    const FieldValue fallback = readField(root, rootField, base, rootWarnings);
    if(!isSet(primary)) {
        warnings.append(rootWarnings);
        return fallback;
    }
    if(isSet(fallback) && !sameValue(primary, fallback, currentField.kind)) {
        warnings.append(QStringLiteral("current/%1 (%2) differs from root/%3 (%4), keeping current")
                            .arg(currentField.key)
                            .arg(describe(primary, currentField.kind))
                            .arg(rootField.key)
                            .arg(describe(fallback, rootField.kind)));
    }
    return primary;
}

// Fields of the data-tralbum "current" object.
constexpr Field CurrentArtist{u"artist", FieldKind::Text, false, MaxArtistLength, 0.0};
constexpr Field CurrentType{u"type", FieldKind::Text, true, MaxTypeLength, 0.0};
constexpr Field CurrentId{u"id", FieldKind::Integer, false, 0, MaxSafeInteger};
constexpr Field CurrentTitle{u"title", FieldKind::Text, false, MaxTitleLength, 0.0};
constexpr Field CurrentReleaseDate{u"release_date", FieldKind::Date, false, MaxDateLength, 0.0};
constexpr Field CurrentArtId{u"art_id", FieldKind::Integer, false, 0, MaxSafeInteger};

// Fields of the data-tralbum root object.
constexpr Field RootArtist{u"artist", FieldKind::Text, false, MaxArtistLength, 0.0};
constexpr Field RootArtId{u"art_id", FieldKind::Integer, false, 0, MaxSafeInteger};
constexpr Field RootReleaseDate{u"album_release_date", FieldKind::Date, false, MaxDateLength, 0.0};
constexpr Field RootAlbumUrl{u"album_url", FieldKind::Url, false, MaxUrlLength, 0.0};

// Fields of trackinfo[] entries.
constexpr Field EntryId{u"id", FieldKind::Integer, false, 0, MaxSafeInteger};
constexpr Field EntryTitle{u"title", FieldKind::Text, false, MaxTitleLength, 0.0};
constexpr Field EntryArtist{u"artist", FieldKind::Text, false, MaxArtistLength, 0.0};
constexpr Field EntryTrackNumber{u"track_num", FieldKind::Integer, false, 0, MaxTrackNumber};
constexpr Field EntryDuration{u"duration", FieldKind::Duration, false, 0, 0.0};
constexpr Field StreamUrl{u"mp3-128", FieldKind::Url, false, MaxUrlLength, 0.0};

QString readStreamUrl(const QJsonObject& entry, const QUrl& base, QStringList& warnings)
{
    const QJsonValue file = entry.value(u"file");
    if(file.isNull() || file.isUndefined()) {
        return {};
    }
    if(!file.isObject()) {
        warnings.append(QStringLiteral("file is not an object"));
        return {};
    }
    return readField(file.toObject(), StreamUrl, base, warnings).text;
}

QString canonicalPageUrl(const QUrl& pageUrl)
{
    return pageUrl.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
}

BandcampTrack readTrackEntry(const QJsonObject& entry, const QUrl& pageUrl, QStringList& warnings)
{
    BandcampTrack track;
    track.id          = readField(entry, EntryId, pageUrl, warnings).number;
    track.title       = readField(entry, EntryTitle, pageUrl, warnings).text;
    track.artist      = readField(entry, EntryArtist, pageUrl, warnings).text;
    track.trackNumber = static_cast<int>(readField(entry, EntryTrackNumber, pageUrl, warnings).number);
    track.durationMs  = readField(entry, EntryDuration, pageUrl, warnings).number;
    track.streamUrl   = readStreamUrl(entry, pageUrl, warnings);
    return track;
}
} // namespace

BandcampAlbumResult parseAlbumPage(const QByteArray& body, const QUrl& pageUrl)
{
    BandcampAlbumResult result;
    const QJsonObject root = extractTralbum(body, result.error);
    if(!result.isValid()) {
        qCWarning(BANDCAMP_PARSER) << "Album page rejected:" << pageUrl.toString() << result.error;
        return result;
    }
    qCDebug(BANDCAMP_PARSER) << "Found data-tralbum with" << body.size() << "bytes of page";

    const QJsonObject current = root.value(u"current").toObject();
    const QString type        = readField(current, CurrentType, pageUrl, result.warnings).text;
    if(type != u"album") {
        result.error
            = QStringLiteral("page is not an album (type: %1)").arg(type.isEmpty() ? QStringLiteral("unknown") : type);
        qCWarning(BANDCAMP_PARSER) << "Album page rejected:" << pageUrl.toString() << result.error;
        return result;
    }

    BandcampAlbum& album = result.value;
    album.id             = readField(current, CurrentId, pageUrl, result.warnings).number;
    album.title          = readField(current, CurrentTitle, pageUrl, result.warnings).text;
    album.artist         = readCurrentOrRoot(current, root, CurrentArtist, RootArtist, pageUrl, result.warnings).text;
    album.releaseDate
        = readCurrentOrRoot(current, root, CurrentReleaseDate, RootReleaseDate, pageUrl, result.warnings).date;
    if(!album.releaseDate.isValid()) {
        result.warnings.append(QStringLiteral("album release date is missing"));
    }
    album.artId     = readCurrentOrRoot(current, root, CurrentArtId, RootArtId, pageUrl, result.warnings).number;
    album.sourceUrl = canonicalPageUrl(pageUrl);

    const QJsonValue infoValue = root.value(u"trackinfo");
    if(!infoValue.isArray()) {
        result.error = QStringLiteral("data-tralbum has no trackinfo list");
        qCWarning(BANDCAMP_PARSER) << "Album page rejected:" << pageUrl.toString() << result.error;
        return result;
    }

    const QJsonArray info = infoValue.toArray();
    QList<BandcampTrack> tracks;
    tracks.reserve(info.size());
    for(qsizetype i = 0; i < info.size(); ++i) {
        const QJsonValue entry = info.at(i);
        if(!entry.isObject()) {
            result.warnings.append(QStringLiteral("trackinfo[%1] is not an object, skipped").arg(i));
            continue;
        }

        tracks.append(readTrackEntry(entry.toObject(), pageUrl, result.warnings));
    }
    for(BandcampTrack& track : tracks) {
        track.sourceUrl = album.sourceUrl;
    }
    album.tracks = std::move(tracks);

    for(const QString& warning : std::as_const(result.warnings)) {
        qCDebug(BANDCAMP_PARSER) << "Album warning:" << warning;
    }
    qCInfo(BANDCAMP_PARSER) << "Parsed album" << album.title << "by" << album.artist << "with" << album.tracks.size()
                            << "tracks and" << result.warnings.size() << "warnings";
    return result;
}

BandcampTrackResult parseTrackPage(const QByteArray& body, const QUrl& pageUrl)
{
    BandcampTrackResult result;
    const QJsonObject root = extractTralbum(body, result.error);
    if(!result.isValid()) {
        qCWarning(BANDCAMP_PARSER) << "Track page rejected:" << pageUrl.toString() << result.error;
        return result;
    }
    qCDebug(BANDCAMP_PARSER) << "Found data-tralbum with" << body.size() << "bytes of page";

    const QJsonObject current = root.value(u"current").toObject();
    const QString type        = readField(current, CurrentType, pageUrl, result.warnings).text;
    if(type != u"track") {
        result.error
            = QStringLiteral("page is not a track (type: %1)").arg(type.isEmpty() ? QStringLiteral("unknown") : type);
        qCWarning(BANDCAMP_PARSER) << "Track page rejected:" << pageUrl.toString() << result.error;
        return result;
    }

    const QJsonValue infoValue = root.value(u"trackinfo");
    if(!infoValue.isArray() || infoValue.toArray().isEmpty()) {
        result.error = QStringLiteral("track page has no trackinfo entry");
        qCWarning(BANDCAMP_PARSER) << "Track page rejected:" << pageUrl.toString() << result.error;
        return result;
    }

    const QJsonArray info = infoValue.toArray();
    if(info.size() > 1) {
        result.warnings.append(QStringLiteral("track page lists %1 entries, using the first").arg(info.size()));
    }
    if(!info.at(0).isObject()) {
        result.error = QStringLiteral("track entry is not an object");
        qCWarning(BANDCAMP_PARSER) << "Track page rejected:" << pageUrl.toString() << result.error;
        return result;
    }

    BandcampTrack& track = result.value;
    track                = readTrackEntry(info.at(0).toObject(), pageUrl, result.warnings);

    track.albumArtist = readCurrentOrRoot(current, root, CurrentArtist, RootArtist, pageUrl, result.warnings).text;
    // root album_release_date belongs to the album, so it is not a fallback here. ReleaseFieldsProcessor inherits it.
    track.releaseDate      = readField(current, CurrentReleaseDate, pageUrl, result.warnings).date;
    track.artId            = readCurrentOrRoot(current, root, CurrentArtId, RootArtId, pageUrl, result.warnings).number;
    const QString albumUrl = readField(root, RootAlbumUrl, pageUrl, result.warnings).text;
    track.sourceUrl        = albumUrl.isEmpty() ? canonicalPageUrl(pageUrl) : albumUrl;

    for(const QString& warning : std::as_const(result.warnings)) {
        qCDebug(BANDCAMP_PARSER) << "Track warning:" << warning;
    }
    qCInfo(BANDCAMP_PARSER) << "Parsed track" << track.title << "by" << track.artist << "with" << result.warnings.size()
                            << "warnings";
    return result;
}
} // namespace Fooyin::Bandcamp
