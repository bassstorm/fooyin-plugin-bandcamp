#pragma once

#include "bandcampmodel.h"
#include "bandcampurl.h"

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
void setBandcampTransport(Fooyin::Track& track, const BandcampTrack& source);
[[nodiscard]] std::optional<qint64> bandcampStreamExpiryMs(const QString& streamUrl);
[[nodiscard]] BandcampPersistedTransportResult readBandcampPersistedTransport(const Fooyin::Track& track, qint64 id);
[[nodiscard]] bool bandcampRefreshNeeded(BandcampStreamExpiry expiry, qint64 nowMs, qint64 windowMs);
} // namespace Fooyin::Bandcamp
