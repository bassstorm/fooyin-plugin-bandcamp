#include "bandcamppostprocessor.h"

#include "bandcamplogging.h"
#include "bandcampprocessors.h"

#include <QLoggingCategory>

#include <utility>

namespace Fooyin::Bandcamp {
namespace {
void logChanges(const BandcampPostProcessor& processor, int changes)
{
    if(changes > 0) {
        qCDebug(BANDCAMP_TRACKS) << "post-processor" << processor.name() << "changed" << changes << "fields";
    }
}
} // namespace

void BandcampPostProcessorPipeline::add(std::unique_ptr<AlbumPostProcessor> processor)
{
    m_steps.emplace_back(std::move(processor));
}

void BandcampPostProcessorPipeline::add(std::unique_ptr<TrackPostProcessor> processor)
{
    m_steps.emplace_back(std::move(processor));
}

void BandcampPostProcessorPipeline::runAlbum(BandcampAlbum& album, QStringList& warnings) const
{
    int total = 0;
    for(const Step& step : m_steps) {
        if(const auto* processor = std::get_if<std::unique_ptr<AlbumPostProcessor>>(&step)) {
            const int changes = (*processor)->process(album, warnings);
            logChanges(**processor, changes);
            total += changes;
            continue;
        }

        const auto& processor = std::get<std::unique_ptr<TrackPostProcessor>>(step);
        int changes           = 0;
        for(BandcampTrack& track : album.tracks) {
            changes += processor->process(track, &album, warnings);
        }
        logChanges(*processor, changes);
        total += changes;
    }
    qCInfo(BANDCAMP_TRACKS) << "post-processing" << album.tracks.size() << "tracks:" << total << "changes";
}

void BandcampPostProcessorPipeline::runTrack(BandcampTrack& track, const BandcampAlbum* album,
                                             QStringList& warnings) const
{
    int total = 0;
    for(const Step& step : m_steps) {
        const auto* processor = std::get_if<std::unique_ptr<TrackPostProcessor>>(&step);
        if(!processor) {
            continue;
        }
        const int changes = (*processor)->process(track, album, warnings);
        logChanges(**processor, changes);
        total += changes;
    }
    qCDebug(BANDCAMP_TRACKS) << "post-processing 1 tracks:" << total << "changes";
}

const BandcampPostProcessorPipeline& standardPostProcessors()
{
    static const BandcampPostProcessorPipeline pipeline = [] {
        BandcampPostProcessorPipeline built;
        built.add(std::make_unique<TrackTotalProcessor>());
        built.add(std::make_unique<AlbumArtistInferenceProcessor>());
        built.add(std::make_unique<ReleaseFieldsProcessor>());
        built.add(std::make_unique<PlayableTracksProcessor>());
        built.add(std::make_unique<ArtistInferenceProcessor>());
        built.add(std::make_unique<ReleaseArtistFallbackProcessor>());
        built.add(std::make_unique<AlbumTitlePrefixProcessor>());
        built.add(std::make_unique<TrackTitlePrefixProcessor>());
        built.add(std::make_unique<AlbumMissingTitleProcessor>());
        built.add(std::make_unique<TrackMissingTitleProcessor>());
        built.add(std::make_unique<TrackNumberRepairProcessor>());
        return built;
    }();
    return pipeline;
}

const BandcampPostProcessorPipeline& albumTrackPostProcessors()
{
    static const BandcampPostProcessorPipeline pipeline = [] {
        BandcampPostProcessorPipeline built;
        built.add(std::make_unique<AlbumTrackArtistProcessor>());
        built.add(std::make_unique<ReleaseFieldsProcessor>());
        return built;
    }();
    return pipeline;
}

void postProcessAlbum(BandcampAlbum& album, QStringList& warnings)
{
    standardPostProcessors().runAlbum(album, warnings);
}

void postProcessTrack(BandcampTrack& track, QStringList& warnings)
{
    standardPostProcessors().runTrack(track, nullptr, warnings);
}

void postProcessAlbumTrack(BandcampTrack& track, const BandcampAlbum& album, QStringList& warnings)
{
    albumTrackPostProcessors().runTrack(track, &album, warnings);
}
} // namespace Fooyin::Bandcamp
