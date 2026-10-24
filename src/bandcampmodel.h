#pragma once

#include <QDate>
#include <QList>
#include <QString>

#include <optional>
#include <variant>

namespace Fooyin::Bandcamp {
struct BandcampTrack
{
    qint64 id{0};
    QString title;
    QString artist;
    // Where to re-fetch the track from. The album URL when the track belongs to
    // an album, so one request refreshes every track of that album. Otherwise the
    // track's own URL.
    QString sourceUrl;
    QString streamUrl;
    QDate releaseDate;
    // Reserved for cover art. Not used yet.
    qint64 artId{0};
    // 0 when unknown.
    int trackNumber{0};
    // 0 when unknown.
    qint64 durationMs{0};
};

struct BandcampAlbumMetadata
{
    qint64 id{0};
    QString title;
    QString artist;
    QString sourceUrl;
    QDate releaseDate;
    // Reserved for cover art. Not used yet.
    qint64 artId{0};
    // Number of declared album members, including any without a usable stream.
    int trackTotal{0};
};

struct BandcampAlbum
{
    BandcampAlbumMetadata metadata;
    QList<BandcampTrack> tracks;
};

struct BandcampResolvedTrack
{
    BandcampTrack track;
    // Present only after a required parent has been validated. It does not imply
    // that all siblings resolved.
    std::optional<BandcampAlbumMetadata> album;
};

using BandcampResolvedData = std::variant<BandcampAlbum, BandcampResolvedTrack>;
} // namespace Fooyin::Bandcamp
