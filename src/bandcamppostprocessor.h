#pragma once

#include "bandcampassembly.h"

#include <atomic>
#include <memory>
#include <vector>

namespace Fooyin::Bandcamp {
struct AlbumProcessorContext
{
    const QList<BandcampTrack>& siblings;
    bool completeSiblingSet{false};
    CorrelationContext correlation;
    AlbumMetadataProvenance& provenance;
    const std::atomic_bool* stop{nullptr};
};

struct TrackProcessorFacts
{
    std::optional<QString> inferredArtist;
    std::optional<int> repairedTrackNumber;
};

struct TrackProcessorContext
{
    const BandcampAlbumMetadata* parent{nullptr};
    const QList<BandcampTrack>& siblings;
    bool completeSiblingSet{false};
    qsizetype siblingIndex{-1};
    CandidateField<QString> fallbackArtist;
    CandidateField<qint64> fallbackArtId;
    CorrelationContext correlation;
    TrackMetadataProvenance& provenance;
    TrackProcessorFacts facts;
    const std::atomic_bool* stop{nullptr};
};

class AlbumPostProcessor
{
public:
    virtual ~AlbumPostProcessor()                                                           = default;
    [[nodiscard]] virtual QString name() const                                              = 0;
    virtual int process(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const = 0;
};

class TrackPostProcessor
{
public:
    virtual ~TrackPostProcessor()                                                   = default;
    [[nodiscard]] virtual QString name() const                                      = 0;
    virtual int process(BandcampTrack& track, TrackProcessorContext& context) const = 0;
};

class BandcampAlbumProcessorPipeline
{
public:
    void add(std::unique_ptr<AlbumPostProcessor> processor);
    void run(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const;

private:
    std::vector<std::unique_ptr<AlbumPostProcessor>> m_steps;
};

class BandcampTrackProcessorPipeline
{
public:
    void add(std::unique_ptr<TrackPostProcessor> processor);
    void run(BandcampTrack& track, TrackProcessorContext& context) const;

private:
    std::vector<std::unique_ptr<TrackPostProcessor>> m_steps;
};

const BandcampAlbumProcessorPipeline& standardAlbumProcessors();
const BandcampTrackProcessorPipeline& standardTrackProcessors();

void postProcessAlbum(AlbumAdmission& admission, const std::atomic_bool* stop = nullptr);
void postProcessTrack(TrackAdmission& admission, const std::atomic_bool* stop = nullptr);
} // namespace Fooyin::Bandcamp
