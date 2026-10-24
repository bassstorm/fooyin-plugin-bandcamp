#pragma once

#include <QString>

namespace Fooyin::Bandcamp {
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
} // namespace Fooyin::Bandcamp
