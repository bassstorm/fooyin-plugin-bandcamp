#pragma once

#include "bandcamppostprocessor.h"

namespace Fooyin::Bandcamp {
class AlbumArtistInferenceProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const override;
};

class AlbumTitlePrefixProcessor final : public AlbumPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const override;
};

class TrackArtistInferenceProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};

class TrackArtistFallbackProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};

class TrackDateInheritanceProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};

class TrackArtworkInheritanceProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};

class TrackArtworkFallbackProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};

class TrackTitlePrefixProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};

class TrackNumberRepairProcessor final : public TrackPostProcessor
{
public:
    [[nodiscard]] QString name() const override;
    int process(BandcampTrack& track, TrackProcessorContext& context) const override;
};
} // namespace Fooyin::Bandcamp
