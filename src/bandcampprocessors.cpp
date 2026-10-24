#include "bandcampprocessors.h"

#include "bandcampdirtytitle.h"
#include "bandcamplogging.h"

#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
QString TrackTotalProcessor::name() const
{
    return u"track-total"_s;
}

int TrackTotalProcessor::process(BandcampAlbum& album, QStringList& /*warnings*/) const
{
    if(album.trackTotal > 0 || album.tracks.isEmpty()) {
        return 0;
    }

    // Counted before PlayableTracksProcessor removes tracks without a stream.
    album.trackTotal = static_cast<int>(album.tracks.size());
    qCDebug(BANDCAMP_TRACKS).noquote()
        << QStringLiteral("track-total: album track total = %1 (listed tracks)").arg(album.trackTotal);
    return 1;
}

QString AlbumTrackArtistProcessor::name() const
{
    return u"album-track-artist"_s;
}

int AlbumTrackArtistProcessor::process(BandcampTrack& track, const BandcampAlbum* album,
                                       QStringList& /*warnings*/) const
{
    if(!album || album->artist.isEmpty() || track.albumArtist == album->artist) {
        return 0;
    }

    qCDebug(BANDCAMP_TRACKS).noquote()
        << QStringLiteral("album-track-artist: track %1 album artist = %2 (was %3, album artist takes precedence)")
               .arg(track.id)
               .arg(album->artist)
               .arg(track.albumArtist);
    track.albumArtist = album->artist;
    return 1;
}

QString AlbumArtistInferenceProcessor::name() const
{
    return u"album-artist-inference"_s;
}

int AlbumArtistInferenceProcessor::process(BandcampAlbum& album, QStringList& /*warnings*/) const
{
    if(!album.artist.isEmpty() || album.tracks.isEmpty()) {
        return 0;
    }

    // Only a single artist shared by every track is taken as the album artist.
    const QString candidate = album.tracks.first().artist;
    if(candidate.isEmpty()) {
        return 0;
    }
    for(const BandcampTrack& track : std::as_const(album.tracks)) {
        if(!sameArtist(track.artist, candidate)) {
            return 0;
        }
    }

    album.artist = candidate;
    qCDebug(BANDCAMP_TRACKS).noquote()
        << QStringLiteral("album-artist-inference: album artist = %1 (shared by all tracks)").arg(candidate);
    return 1;
}

QString ReleaseFieldsProcessor::name() const
{
    return u"release-fields"_s;
}

int ReleaseFieldsProcessor::process(BandcampTrack& track, const BandcampAlbum* album, QStringList& /*warnings*/) const
{
    if(!album) {
        return 0;
    }

    // The track's own date is kept. Only the disagreement is logged.
    if(track.releaseDate.isValid() && album->releaseDate.isValid() && track.releaseDate != album->releaseDate) {
        qCDebug(BANDCAMP_TRACKS).noquote()
            << QStringLiteral("release-fields: track %1 keeps release date %2, album release date is %3")
                   .arg(track.id)
                   .arg(track.releaseDate.toString(Qt::ISODate))
                   .arg(album->releaseDate.toString(Qt::ISODate));
    }

    QStringList inherited;
    if(track.albumArtist.isEmpty() && !album->artist.isEmpty()) {
        track.albumArtist = album->artist;
        inherited << u"page artist"_s;
    }
    if(!track.releaseDate.isValid() && album->releaseDate.isValid()) {
        track.releaseDate = album->releaseDate;
        inherited << u"release date"_s;
    }
    if(track.artId == 0 && album->artId != 0) {
        track.artId = album->artId;
        inherited << u"art"_s;
    }
    if(track.trackTotal == 0 && album->trackTotal > 0) {
        track.trackTotal = album->trackTotal;
        inherited << u"track total"_s;
    }
    if(inherited.isEmpty()) {
        return 0;
    }

    qCDebug(BANDCAMP_TRACKS).noquote() << QStringLiteral("release-fields: track %1 inherits %2 from release")
                                              .arg(track.id)
                                              .arg(inherited.join(u", "_s));
    return static_cast<int>(inherited.size());
}

QString PlayableTracksProcessor::name() const
{
    return u"playable-tracks"_s;
}

int PlayableTracksProcessor::process(BandcampAlbum& album, QStringList& warnings) const
{
    int removed = 0;
    for(auto it = album.tracks.begin(); it != album.tracks.end();) {
        if(!it->streamUrl.isEmpty()) {
            ++it;
            continue;
        }
        warnings.append(QStringLiteral("track %1 has no usable stream, skipped").arg(it->id));
        qCDebug(BANDCAMP_TRACKS).noquote()
            << QStringLiteral("playable-tracks: track %1 removed (no stream)").arg(it->id);
        it = album.tracks.erase(it);
        ++removed;
    }

    if(album.tracks.isEmpty()) {
        warnings.append(QStringLiteral("album has no playable tracks"));
    }
    return removed;
}

QString ArtistInferenceProcessor::name() const
{
    return u"artist-inference"_s;
}

int ArtistInferenceProcessor::process(BandcampAlbum& album, QStringList& /*warnings*/) const
{
    // Inference is all-or-nothing: one explicit artist or one title without the shape disables it.
    QList<DirtyTitle> dirty;
    dirty.reserve(album.tracks.size());
    for(const BandcampTrack& track : std::as_const(album.tracks)) {
        if(!track.artist.isEmpty()) {
            return 0;
        }
        dirty.append(splitDirtyTitle(track.title));
        if(!dirty.last().matched) {
            return 0;
        }
    }

    int changes = 0;
    for(qsizetype i = 0; i < album.tracks.size(); ++i) {
        BandcampTrack& track = album.tracks[i];
        track.artist         = dirty.at(i).artist;
        ++changes;
        qCDebug(BANDCAMP_TRACKS).noquote()
            << QStringLiteral("artist-inference: track %1 artist = %2 (from title)").arg(track.id).arg(track.artist);
    }
    return changes;
}

QString ReleaseArtistFallbackProcessor::name() const
{
    return u"release-artist-fallback"_s;
}

int ReleaseArtistFallbackProcessor::process(BandcampTrack& track, const BandcampAlbum* album,
                                            QStringList& /*warnings*/) const
{
    if(!track.artist.isEmpty()) {
        return 0;
    }

    // A standalone track page has no album, so its page artist is the track's albumArtist.
    const QString fallback = album ? album->artist : track.albumArtist;
    if(fallback.isEmpty()) {
        return 0;
    }

    track.artist = fallback;
    qCDebug(BANDCAMP_TRACKS).noquote() << QStringLiteral(
                                              "release-artist-fallback: track %1 artist = %2 (from page artist)")
                                              .arg(track.id)
                                              .arg(track.artist);
    return 1;
}

QString AlbumTitlePrefixProcessor::name() const
{
    return u"album-title-prefix"_s;
}

int AlbumTitlePrefixProcessor::process(BandcampAlbum& album, QStringList& /*warnings*/) const
{
    const DirtyTitle parts = splitDirtyTitle(album.title);
    if(!parts.matched || !sameArtist(parts.artist, album.artist)) {
        return 0;
    }

    qCDebug(BANDCAMP_TRACKS).noquote()
        << QStringLiteral("album-title-prefix: album title = %1 (was %2)").arg(parts.title).arg(album.title);
    album.title = parts.title;
    return 1;
}

QString TrackTitlePrefixProcessor::name() const
{
    return u"track-title-prefix"_s;
}

int TrackTitlePrefixProcessor::process(BandcampTrack& track, const BandcampAlbum* /*album*/,
                                       QStringList& /*warnings*/) const
{
    const DirtyTitle parts = splitDirtyTitle(track.title);
    if(!parts.matched || !sameArtist(parts.artist, track.artist)) {
        return 0;
    }

    qCDebug(BANDCAMP_TRACKS).noquote() << QStringLiteral("track-title-prefix: track %1 title = %2 (was %3)")
                                              .arg(track.id)
                                              .arg(parts.title, track.title);
    track.title = parts.title;
    return 1;
}

QString AlbumMissingTitleProcessor::name() const
{
    return u"album-missing-title"_s;
}

int AlbumMissingTitleProcessor::process(BandcampAlbum& album, QStringList& warnings) const
{
    if(album.title.isEmpty()) {
        warnings.append(QStringLiteral("album has no title"));
    }
    return 0;
}

QString TrackMissingTitleProcessor::name() const
{
    return u"track-missing-title"_s;
}

int TrackMissingTitleProcessor::process(BandcampTrack& track, const BandcampAlbum* /*album*/,
                                        QStringList& warnings) const
{
    if(track.title.isEmpty()) {
        warnings.append(QStringLiteral("track %1 has no title").arg(track.id));
    }
    return 0;
}

QString TrackNumberRepairProcessor::name() const
{
    return u"track-number-repair"_s;
}

int TrackNumberRepairProcessor::process(BandcampAlbum& album, QStringList& warnings) const
{
    QSet<int> used;
    int previous = 0;
    int changes  = 0;
    for(qsizetype i = 0; i < album.tracks.size(); ++i) {
        BandcampTrack& track = album.tracks[i];
        if(track.trackNumber > 0 && !used.contains(track.trackNumber)) {
            used.insert(track.trackNumber);
            previous = track.trackNumber;
            continue;
        }

        const int original = track.trackNumber;
        int fallback       = previous + 1;
        while(used.contains(fallback)) {
            ++fallback;
        }
        track.trackNumber = fallback;
        used.insert(fallback);
        previous = fallback;
        ++changes;

        if(original > 0) {
            warnings.append(
                QStringLiteral("track %1 number %2 is repeated, using %3").arg(i + 1).arg(original).arg(fallback));
        }
        else {
            warnings.append(QStringLiteral("track %1 has no number, using %2").arg(i + 1).arg(fallback));
        }
        qCDebug(BANDCAMP_TRACKS).noquote() << QStringLiteral("track-number-repair: track %1 number = %2 (was %3)")
                                                  .arg(track.id)
                                                  .arg(fallback)
                                                  .arg(original);
    }
    return changes;
}
} // namespace Fooyin::Bandcamp
