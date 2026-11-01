#include "bandcamptrackuri.h"

namespace Fooyin::Bandcamp {
namespace {
constexpr char UriPrefix[]          = "bandcamp://track/";
constexpr qsizetype UriPrefixLength = sizeof(UriPrefix) - 1;
constexpr qsizetype MaxTrackIdDigits = 19;
constexpr qsizetype MaxUriLength = UriPrefixLength + MaxTrackIdDigits;
} // namespace

QString bandcampTrackUri(qint64 trackId)
{
    if(trackId <= 0) {
        return {};
    }

    return QString::fromLatin1(UriPrefix) + QString::number(trackId);
}

std::optional<qint64> parseBandcampTrackUri(const QString& uri)
{
    if(uri.size() <= UriPrefixLength || uri.size() > MaxUriLength
       || !uri.startsWith(QLatin1StringView{UriPrefix})) {
        return std::nullopt;
    }

    const QStringView digits{uri.constData() + UriPrefixLength, uri.size() - UriPrefixLength};
    if(digits.isEmpty() || digits.front() < u'1' || digits.front() > u'9') {
        return std::nullopt;
    }
    for(const QChar digit : digits) {
        if(digit < u'0' || digit > u'9') {
            return std::nullopt;
        }
    }

    bool ok  = false;
    const QString idText = digits.toString();
    const auto id         = idText.toLongLong(&ok);
    if(!ok || id <= 0 || QString::number(id) != idText) {
        return std::nullopt;
    }

    return id;
}
} // namespace Fooyin::Bandcamp
