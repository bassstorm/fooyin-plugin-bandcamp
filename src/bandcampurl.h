#pragma once

#include <QString>

namespace Fooyin::Bandcamp {
inline constexpr qsizetype MaxBandcampStreamUrlChars = 16384;

enum class BandcampPageKind : unsigned char
{
    Discography,
    Album,
    Track,
};

struct BandcampPageUrl
{
    BandcampPageKind kind{BandcampPageKind::Discography};
    // Canonical form: scheme, host, port, and path only.
    QString url;
    // Empty when the URL is accepted.
    QString error;

    [[nodiscard]] bool isValid() const
    {
        return error.isEmpty();
    }
};

BandcampPageUrl parseBandcampPageUrl(const QString& text);
QString pageKindName(BandcampPageKind kind);
[[nodiscard]] bool isSupportedBandcampStreamUrl(const QString& text);
} // namespace Fooyin::Bandcamp
