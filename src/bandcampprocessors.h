#pragma once

#include "bandcamppostprocessor.h"

namespace Fooyin::Bandcamp {
// Album pages only. Counts the tracks Bandcamp lists, so it must run before any track is removed.
class TrackTotalProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};

// Album tracks take the album's artist as their album artist, even when the track page names its own.
class AlbumTrackArtistProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const override;
};

// Album pages only. Takes the album artist from the one artist all tracks share, when the page has none.
class AlbumArtistInferenceProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};

// Copies album release fields onto a track that lacks them.
class ReleaseFieldsProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const override;
};

// Album pages only. Removes tracks with no stream. Warns when no playable track is left.
class PlayableTracksProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};

// Album pages only. Sets the artist from "Artist - Title" titles when no track has an artist of its own
// and every title has that shape.
class ArtistInferenceProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};

// Fills a missing track artist. Album tracks take the album artist, standalone tracks the page artist.
class ReleaseArtistFallbackProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const override;
};

// Album pages only. Removes the album artist's "Artist - " prefix from the album title.
class AlbumTitlePrefixProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};

// Removes the track artist's "Artist - " prefix from the track title. Works on album and standalone tracks.
class TrackTitlePrefixProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const override;
};

// Album pages only. Warns when the album has no title.
class AlbumMissingTitleProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};

// Warns when a track has no title. Works on album and standalone tracks.
class TrackMissingTitleProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const override;
};

// Album pages only. Keeps Bandcamp's numbers. A missing or repeated number takes the previous number plus one.
class TrackNumberRepairProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbum& album, QStringList& warnings) const override;
};
} // namespace Fooyin::Bandcamp
