#pragma once

#include "bandcampmodel.h"

#include <QList>
#include <QStringList>

#include <memory>
#include <variant>
#include <vector>

namespace Fooyin::Bandcamp {
// Base for every post-processor. A processor works on either an album or a single track.
class BandcampPostProcessor
{
public:
    virtual ~BandcampPostProcessor() = default;

    // Stable name used in logs.
    [[nodiscard]] virtual QString name() const = 0;
};

// Works on an album, including its track list.
class AlbumPostProcessor : public BandcampPostProcessor
{
public:
    // Changes the album and its tracks in place and returns the number of fields changed.
    // Warnings go to the caller's list.
    virtual int process(BandcampAlbum& album, QStringList& warnings) const = 0;
};

// Works on one track. The album is the track's parent, or nullptr for a standalone track page.
class TrackPostProcessor : public BandcampPostProcessor
{
public:
    // Changes the track in place and returns the number of fields changed. Warnings go to the caller's list.
    virtual int process(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const = 0;
};

class BandcampPostProcessorPipeline
{
public:
    void add(std::unique_ptr<AlbumPostProcessor> processor);
    void add(std::unique_ptr<TrackPostProcessor> processor);

    // Runs every step in the order it was added. Album steps run once. Track steps run on each track,
    // with the album as their parent.
    void runAlbum(BandcampAlbum& album, QStringList& warnings) const;

    // Runs the track steps on one track and skips the album steps.
    void runTrack(BandcampTrack& track, const BandcampAlbum* album, QStringList& warnings) const;

private:
    using Step = std::variant<std::unique_ptr<AlbumPostProcessor>, std::unique_ptr<TrackPostProcessor>>;

    std::vector<Step> m_steps;
};

// The processors applied to every parsed page, in run order.
const BandcampPostProcessorPipeline& standardPostProcessors();

// The processors applied to a track of an album, in run order. Fills the track from the album's release fields.
const BandcampPostProcessorPipeline& albumTrackPostProcessors();

// Runs the standard pipeline over an album and its tracks, and writes the release fields back.
void postProcessAlbum(BandcampAlbum& album, QStringList& warnings);

// Runs the standard pipeline over the single track of a track page.
void postProcessTrack(BandcampTrack& track, QStringList& warnings);

// Runs the album track pipeline over a track of an album, using the album's release fields.
void postProcessAlbumTrack(BandcampTrack& track, const BandcampAlbum& album, QStringList& warnings);
} // namespace Fooyin::Bandcamp
