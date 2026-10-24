#pragma once

#include <QDate>
#include <QList>
#include <QString>

namespace Fooyin::Bandcamp {
struct BandcampTrack
{
    qint64 id{0};
    QString title;
    QString artist;
    QString albumArtist;
    // Where to re-fetch the track from. The album URL when the track belongs to an album, so one request
    // refreshes every track of that album. Otherwise the track's own URL.
    QString sourceUrl;
    QString streamUrl;
    QDate releaseDate;
    // Reserved for cover art. Not used yet.
    qint64 artId{0};
    // 0 when unknown.
    int trackNumber{0};
    // 0 when unknown.
    int trackTotal{0};
    // 0 when unknown.
    qint64 durationMs{0};
};

struct BandcampAlbum
{
    qint64 id{0};
    QString title;
    QString artist;
    QString sourceUrl;
    QDate releaseDate;
    // Reserved for cover art. Not used yet.
    qint64 artId{0};
    // Number of tracks the release lists, including any without a stream.
    int trackTotal{0};
    QList<BandcampTrack> tracks;
};
} // namespace Fooyin::Bandcamp
