#include "bandcamplegacyadapter.h"

#include "bandcampparser.h"

namespace Fooyin::Bandcamp {
namespace {
template <typename T> T optionalValue(const CandidateField<T>& field)
{
    return field.isUsable() ? field.value : T{};
}

QString urlValue(const CandidateField<QUrl>& field)
{
    return optionalValue(field).toString(QUrl::FullyEncoded);
}

BandcampTrack legacyTrack(const TrackCandidate& candidate, const QString& sourceUrl)
{
    BandcampTrack track;
    track.id          = optionalValue(candidate.id);
    track.title       = optionalValue(candidate.title);
    track.artist      = optionalValue(candidate.artist);
    track.trackNumber = static_cast<int>(optionalValue(candidate.trackNumber));
    track.durationMs  = optionalValue(candidate.durationMs);
    track.releaseDate = optionalValue(candidate.releaseDate);
    track.artId       = optionalValue(candidate.artId);
    track.streamUrl   = optionalValue(candidate.streamUrl);
    track.sourceUrl   = sourceUrl;
    return track;
}

template <typename Candidate, typename Result, typename Convert>
Result parseLegacy(const QByteArray& body, const QUrl& url, CorrelationContext context, Convert convert)
{
    const auto parsed = parseReleasePage(body, url, std::move(context));
    Result result;
    if(const auto* success = std::get_if<ResolutionSuccess<ReleaseCandidate>>(&parsed)) {
        if(const auto* candidate = std::get_if<Candidate>(&success->value)) {
            result.value = convert(*candidate);
        } else {
            result.error = QStringLiteral("release kind differs from requested selection");
        }
    } else if(const auto* failed = std::get_if<ResolutionFailure>(&parsed)) {
        result.error = failed->error.reason;
    } else {
        result.error = QStringLiteral("parsing cancelled");
    }
    return result;
}
} // namespace

LegacyAlbumResult parseLegacyAlbumPage(const QByteArray& body, const QUrl& url, CorrelationContext context)
{
    return parseLegacy<AlbumCandidate, LegacyAlbumResult>(
        body, url, std::move(context), [](const AlbumCandidate& candidate) {
            BandcampAlbum album;
            album.id          = optionalValue(candidate.id);
            album.title       = optionalValue(candidate.title);
            album.artist      = optionalValue(candidate.artist);
            album.releaseDate = optionalValue(candidate.releaseDate);
            album.artId       = optionalValue(candidate.artId);
            album.sourceUrl   = urlValue(candidate.sourceUrl);
            album.trackTotal  = static_cast<int>(candidate.tracks.size());
            for(const auto& child : candidate.tracks) {
                album.tracks.append(legacyTrack(child, album.sourceUrl));
            }
            return album;
        });
}

LegacyTrackResult parseLegacyTrackPage(const QByteArray& body, const QUrl& url, CorrelationContext context)
{
    return parseLegacy<TrackPageCandidate, LegacyTrackResult>(
        body, url, std::move(context), [](const TrackPageCandidate& candidate) {
            const auto source = candidate.parent.albumUrl.isUsable() ? candidate.parent.albumUrl : candidate.sourceUrl;
            auto track        = legacyTrack(candidate.track, urlValue(source));
            track.albumArtist = candidate.fallbackArtist.isUsable() ? candidate.fallbackArtist.value : track.artist;
            return track;
        });
}
} // namespace Fooyin::Bandcamp
