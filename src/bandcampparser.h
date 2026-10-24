#pragma once

#include "bandcampmodel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>

namespace Fooyin::Bandcamp {
constexpr qsizetype MaxPageBytes = 20 * 1024 * 1024;

template <typename T>
struct BandcampParseResult
{
    T value;
    // Empty when the page was parsed. Field-level problems go to warnings instead.
    QString error;
    QStringList warnings;

    [[nodiscard]] bool isValid() const
    {
        return error.isEmpty();
    }
};

using BandcampAlbumResult = BandcampParseResult<BandcampAlbum>;
using BandcampTrackResult = BandcampParseResult<BandcampTrack>;

// Returns the data-tralbum object of a page, or sets error when it is missing or invalid.
QJsonObject extractTralbum(const QByteArray& body, QString& error);

// Maps the album fields of the page as reported. Post-processing is left to the caller.
BandcampAlbumResult parseAlbumPage(const QByteArray& body, const QUrl& pageUrl);
// Maps the track fields of the page as reported. Post-processing is left to the caller.
BandcampTrackResult parseTrackPage(const QByteArray& body, const QUrl& pageUrl);
} // namespace Fooyin::Bandcamp
