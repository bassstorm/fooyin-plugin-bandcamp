#pragma once

#include "bandcampmodel.h"
#include "bandcampparser.h"

#include <atomic>

namespace Fooyin::Bandcamp {
struct AlbumMetadataProvenance
{
    FieldProvenance title{FieldProvenance::Explicit};
    FieldProvenance artist{FieldProvenance::Explicit};
    FieldProvenance releaseDate{FieldProvenance::Explicit};
};

struct TrackMetadataProvenance
{
    FieldProvenance title{FieldProvenance::Explicit};
    FieldProvenance artist{FieldProvenance::Explicit};
    FieldProvenance trackNumber{FieldProvenance::Explicit};
    FieldProvenance durationMs{FieldProvenance::Explicit};
    FieldProvenance releaseDate{FieldProvenance::Explicit};
    FieldProvenance streamUrl{FieldProvenance::Explicit};
};

struct AlbumAdmission
{
    BandcampAlbum album;
    AlbumMetadataProvenance metadataProvenance;
    QList<TrackMetadataProvenance> trackProvenance;
    CorrelationContext correlation;
};

struct TrackAdmission
{
    BandcampResolvedTrack result;
    TrackMetadataProvenance trackProvenance;
    AlbumMetadataProvenance parentProvenance;
    CandidateField<QString> fallbackArtist;
    QList<BandcampTrack> siblings;
    bool completeSiblingSet{false};
    qsizetype siblingIndex{-1};
    CorrelationContext correlation;
};

struct StandaloneParentEvidence
{ };

struct RequiredAlbumParent
{
    QUrl url;
    std::optional<qint64> albumId;
};

using ParentRequirement = std::variant<StandaloneParentEvidence, RequiredAlbumParent, ResolutionFailure>;

ParentRequirement classifyParentEvidence(const TrackPageCandidate& candidate, CorrelationContext context = {});

ResolutionOutcome<AlbumAdmission> admitAlbum(const AlbumCandidate& candidate, CorrelationContext context = {},
                                             const std::atomic_bool* stop = nullptr);
ResolutionOutcome<TrackAdmission> admitStandaloneTrack(const TrackPageCandidate& candidate,
                                                       CorrelationContext context   = {},
                                                       const std::atomic_bool* stop = nullptr);
ResolutionOutcome<TrackAdmission> admitAlbumTrack(const TrackPageCandidate& trackPage, const AlbumCandidate& album,
                                                  CorrelationContext context   = {},
                                                  const std::atomic_bool* stop = nullptr);
} // namespace Fooyin::Bandcamp
