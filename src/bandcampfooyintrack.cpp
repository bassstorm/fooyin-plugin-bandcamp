#include "bandcampfooyintrack.h"

#include "bandcamptrackuri.h"

#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <limits>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
int sourceBitDepth(const QString& encoding, int depth)
{
    // Lossy decoders report PCM precision, not a source bit depth.
    return encoding == u"Lossy"_s ? -1 : depth;
}

std::optional<BandcampPageUrl> canonicalReleaseSource(const QString& text)
{
    auto source = parseBandcampPageUrl(text);
    if(!source.isValid() || source.kind == BandcampPageKind::Discography || source.url != text) {
        return std::nullopt;
    }
    return source;
}
} // namespace

BandcampResolvedTrackLookup::BandcampResolvedTrackLookup(const BandcampResolvedData& data)
{
    if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
        m_album = &album->metadata;
        m_tracks.reserve(album->tracks.size());
        for(const BandcampTrack& track : album->tracks) {
            m_tracks.insert(track.id, &track);
        }
    }
    else {
        const auto& track = std::get<BandcampResolvedTrack>(data);
        m_tracks.insert(track.track.id, &track.track);
        m_album = track.album ? &*track.album : nullptr;
    }
}

std::optional<BandcampResolvedTrack> BandcampResolvedTrackLookup::find(qint64 id) const
{
    const auto track = m_tracks.constFind(id);
    if(track == m_tracks.cend()) {
        return std::nullopt;
    }
    return BandcampResolvedTrack{**track, m_album ? std::optional{*m_album} : std::nullopt};
}

Fooyin::Track makeFooyinTrack(const BandcampResolvedTrack& resolved,
                              std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore)
{
    const BandcampTrack& source = resolved.track;
    Fooyin::Track track{bandcampTrackUri(source.id), std::move(metadataStore)};
    track.setTitle(source.title);
    if(!source.artist.isEmpty()) {
        track.setArtists({source.artist});
    }
    if(resolved.album) {
        track.setAlbum(resolved.album->title);
        if(!resolved.album->artist.isEmpty()) {
            track.setAlbumArtists({resolved.album->artist});
        }
        track.setTrackTotal(QString::number(resolved.album->trackTotal));
    }
    if(source.releaseDate.isValid()) {
        track.setDate(source.releaseDate.toString(Qt::ISODate));
    }
    if(source.trackNumber != 0) {
        track.setTrackNumber(QString::number(source.trackNumber));
    }
    if(source.durationMs != 0) {
        track.setDuration(static_cast<uint64_t>(source.durationMs));
    }
    track.setMetadataWasRead(true);
    setBandcampTransport(track, source);
    track.generateHash();
    track.clearWasModified();
    return track;
}

Fooyin::Track makeFooyinImportTrack(const BandcampImportTrack& imported,
                                    std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore)
{
    Fooyin::Track track = makeFooyinTrack(imported.resolved, std::move(metadataStore));
    applyBandcampStreamProperties(track, imported.streamProperties);
    track.generateHash();
    track.clearWasModified();
    return track;
}

bool BandcampStreamProperties::isEmpty() const
{
    return codec.isEmpty() && encoding.isEmpty() && sampleRate <= 0 && channels <= 0 && bitDepth <= 0 && bitrate <= 0;
}

BandcampStreamProperties bandcampStreamProperties(const Fooyin::Track& track)
{
    return {
        .codec      = track.codec(),
        .encoding   = track.encoding(),
        .sampleRate = track.sampleRate(),
        .channels   = track.channels(),
        .bitDepth   = track.bitDepth(),
        .bitrate    = track.bitrate(),
    };
}

BandcampStreamProperties bandcampDecoderStreamProperties(const Fooyin::Track& track, const Fooyin::AudioFormat& format)
{
    return {
        .codec      = track.codec(),
        .encoding   = track.encoding(),
        .sampleRate = format.sampleRate(),
        .channels   = format.channelCount(),
        .bitDepth   = sourceBitDepth(track.encoding(), format.bitdepth()),
        .bitrate    = track.bitrate(),
    };
}

void applyBandcampStreamProperties(Fooyin::Track& track, const BandcampStreamProperties& properties)
{
    if(!properties.codec.isEmpty()) {
        track.setCodec(properties.codec);
    }
    if(!properties.encoding.isEmpty()) {
        track.setEncoding(properties.encoding);
    }
    if(properties.sampleRate > 0) {
        track.setSampleRate(properties.sampleRate);
    }
    if(properties.channels > 0) {
        track.setChannels(properties.channels);
    }
    if(properties.bitDepth > 0) {
        track.setBitDepth(properties.bitDepth);
    }
    track.setBitDepth(sourceBitDepth(track.encoding(), track.bitDepth()));
    if(properties.bitrate > 0) {
        track.setBitrate(properties.bitrate);
    }
}

void setBandcampTransport(Fooyin::Track& track, const BandcampTrack& source)
{
    track.setExtraProperty(StreamProviderProperty, u"bandcamp"_s);
    track.setExtraProperty(BandcampSourceUrlProperty, source.sourceUrl);
    track.setExtraProperty(BandcampStreamUrlProperty, source.streamUrl);
    if(const auto expiry = bandcampStreamExpiryMs(source.streamUrl)) {
        track.setExtraProperty(BandcampExpiresAtProperty, QString::number(*expiry));
    }
}

std::optional<qint64> bandcampStreamExpiryMs(const QString& streamUrl)
{
    const QUrl url{streamUrl, QUrl::StrictMode};
    static const QRegularExpression bandcampStreamHost{u"^t[0-9]+\\.bcbits\\.com$"_s};
    const QString path = url.path(QUrl::FullyDecoded);
    if(!url.host().contains(bandcampStreamHost) || !path.startsWith(u"/stream/"_s) || path.size() <= qsizetype{8}
       || path.at(8) == u'/') {
        return std::nullopt;
    }

    const QUrlQuery query{url};
    std::optional<QString> timestamp;
    for(const auto& [key, value] : query.queryItems(QUrl::FullyDecoded)) {
        if(key != u"ts"_s) {
            continue;
        }
        if(timestamp) {
            return std::nullopt;
        }
        timestamp = value;
    }
    if(!timestamp || timestamp->isEmpty()) {
        return std::nullopt;
    }
    for(const QChar ch : *timestamp) {
        if(ch < u'0' || ch > u'9') {
            return std::nullopt;
        }
    }

    bool ok                     = false;
    const qint64 seconds        = timestamp->toLongLong(&ok);
    constexpr qint64 MaxSeconds = std::numeric_limits<qint64>::max() / 1000;
    if(!ok || seconds <= 0 || seconds > MaxSeconds) {
        return std::nullopt;
    }
    return seconds * 1000;
}

BandcampPersistedTransportResult readBandcampPersistedTransport(const Fooyin::Track& track, qint64 id)
{
    const auto properties = track.extraProperties();
    if(!id || properties.value(StreamProviderProperty) != u"bandcamp"_s
       || (track.hasExtraProperty(BandcampTrackIdProperty)
           && properties.value(BandcampTrackIdProperty) != QString::number(id))) {
        return BandcampTransportFailure{
            QStringLiteral("The selected track has inconsistent Bandcamp identity or provider state.")};
    }
    const auto source = canonicalReleaseSource(properties.value(BandcampSourceUrlProperty));
    if(!source) {
        return BandcampTransportFailure{QStringLiteral("The selected track has no canonical release source.")};
    }
    const QString streamText = properties.value(BandcampStreamUrlProperty);
    if(!isSupportedBandcampStreamUrl(streamText)) {
        return BandcampTransportFailure{QStringLiteral("The selected track has an invalid HTTPS stream locator.")};
    }
    BandcampStreamExpiry expiry;
    const QString expiryText = properties.value(BandcampExpiresAtProperty);
    if(!expiryText.isEmpty()) {
        bool ok                   = false;
        const qint64 milliseconds = expiryText.size() <= 19 ? expiryText.toLongLong(&ok) : 0;
        const auto observed       = bandcampStreamExpiryMs(streamText);
        if(ok && milliseconds > 0 && QString::number(milliseconds) == expiryText && observed
           && *observed == milliseconds) {
            expiry = {BandcampExpiryState::Valid, milliseconds};
        }
        else {
            expiry.state = BandcampExpiryState::Invalid;
        }
    }
    return BandcampPersistedTransport{*source, streamText, expiry};
}

bool bandcampRefreshNeeded(BandcampStreamExpiry expiry, qint64 nowMs, qint64 windowMs)
{
    return expiry.state != BandcampExpiryState::Valid || expiry.milliseconds <= nowMs
        || expiry.milliseconds - nowMs < windowMs;
}

std::optional<QString> bandcampReleaseUrl(const Fooyin::Track& track)
{
    if(!parseBandcampTrackUri(track.filepath())) {
        return std::nullopt;
    }
    const auto source = canonicalReleaseSource(track.extraProperties().value(BandcampSourceUrlProperty));
    if(!source) {
        return std::nullopt;
    }
    return source->url;
}

QStringList bandcampReleaseUrls(const Fooyin::TrackList& tracks)
{
    QStringList urls;
    QSet<QString> seen;
    for(const Fooyin::Track& track : tracks) {
        const auto url = bandcampReleaseUrl(track);
        if(url && !seen.contains(*url)) {
            seen.insert(*url);
            urls.append(*url);
        }
    }
    return urls;
}
} // namespace Fooyin::Bandcamp
