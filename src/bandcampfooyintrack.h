#pragma once

#include "bandcampmodel.h"
#include "bandcampurl.h"

#include <core/engine/audioformat.h>
#include <core/track.h>
#include <core/trackmetadatastore.h>

#include <QHash>

#include <memory>
#include <optional>
#include <variant>

namespace Fooyin::Bandcamp {
inline constexpr auto StreamProviderProperty    = "_STREAM_PROVIDER";
inline constexpr auto BandcampSourceUrlProperty = "_BANDCAMP_SOURCE_URL";
inline constexpr auto BandcampStreamUrlProperty = "_BANDCAMP_STREAM_URL";
inline constexpr auto BandcampExpiresAtProperty = "_BANDCAMP_EXPIRES_AT";
inline constexpr auto BandcampTrackIdProperty   = "_BANDCAMP_TRACK_ID";

enum class BandcampExpiryState : quint8
{
    Valid,
    Unset,
    Invalid,
};

struct BandcampStreamExpiry
{
    BandcampExpiryState state{BandcampExpiryState::Unset};
    qint64 milliseconds{0};
};

struct BandcampPersistedTransport
{
    BandcampPageUrl source;
    QString streamUrl;
    BandcampStreamExpiry expiry;
};

struct BandcampStreamProperties
{
    QString codec;
    QString encoding;
    int sampleRate{0};
    int channels{0};
    int bitDepth{0};
    int bitrate{0};

    bool operator==(const BandcampStreamProperties&) const = default;
    [[nodiscard]] bool isEmpty() const;
};

struct BandcampImportTrack
{
    BandcampResolvedTrack resolved;
    BandcampStreamProperties streamProperties;
};

struct BandcampTransportFailure
{
    QString reason;
};

using BandcampPersistedTransportResult = std::variant<BandcampPersistedTransport, BandcampTransportFailure>;

class BandcampResolvedTrackLookup
{
public:
    explicit BandcampResolvedTrackLookup(const BandcampResolvedData& data);
    [[nodiscard]] std::optional<BandcampResolvedTrack> find(qint64 id) const;

private:
    QHash<qint64, const BandcampTrack*> m_tracks;
    const BandcampAlbumMetadata* m_album{nullptr};
};

[[nodiscard]] Fooyin::Track makeFooyinTrack(const BandcampResolvedTrack& resolved,
                                            std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore = {});
[[nodiscard]] Fooyin::Track makeFooyinImportTrack(const BandcampImportTrack& imported,
                                                  std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore = {});
[[nodiscard]] BandcampStreamProperties bandcampStreamProperties(const Fooyin::Track& track);
[[nodiscard]] BandcampStreamProperties bandcampDecoderStreamProperties(const Fooyin::Track& track,
                                                                       const Fooyin::AudioFormat& format);
void applyBandcampStreamProperties(Fooyin::Track& track, const BandcampStreamProperties& properties);
void setBandcampTransport(Fooyin::Track& track, const BandcampTrack& source);
[[nodiscard]] std::optional<qint64> bandcampStreamExpiryMs(const QString& streamUrl);
[[nodiscard]] BandcampPersistedTransportResult readBandcampPersistedTransport(const Fooyin::Track& track, qint64 id);
[[nodiscard]] bool bandcampRefreshNeeded(BandcampStreamExpiry expiry, qint64 nowMs, qint64 windowMs);
} // namespace Fooyin::Bandcamp
