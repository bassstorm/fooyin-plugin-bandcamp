#include "bandcampprocessors.h"

#include "bandcampdirtytitle.h"
#include "bandcamplogging.h"

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
QString provenanceName(FieldProvenance provenance)
{
    switch(provenance) {
        case FieldProvenance::Explicit:
            return QStringLiteral("explicit");
        case FieldProvenance::Fallback:
            return QStringLiteral("fallback");
        case FieldProvenance::Inherited:
            return QStringLiteral("inherited");
        case FieldProvenance::Derived:
            return QStringLiteral("derived");
    }
    return QStringLiteral("unknown");
}

void logAlbumChange(const QString& processor, const QString& field, const QString& oldValue, const QString& newValue,
                    const QString& reason, FieldProvenance provenance, const CorrelationContext& correlation)
{
    if(!BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
        return;
    }
    qCDebug(BANDCAMP_TRACE_TRACKS).noquote() << formatTraceEvent(
        correlation, QStringLiteral("processor_change"),
        {DiagnosticField::text(QStringLiteral("processor"), processor),
         DiagnosticField::text(QStringLiteral("field"), field), DiagnosticField::text(QStringLiteral("old"), oldValue),
         DiagnosticField::text(QStringLiteral("new"), newValue),
         DiagnosticField::text(QStringLiteral("provenance"), provenanceName(provenance)),
         DiagnosticField::text(QStringLiteral("reason"), reason)});
}

void logTrackChange(const QString& processor, const QString& field, const QString& oldValue, const QString& newValue,
                    const QString& reason, FieldProvenance provenance, const CorrelationContext& correlation)
{
    if(!BANDCAMP_TRACE_TRACKS().isDebugEnabled()) {
        return;
    }
    qCDebug(BANDCAMP_TRACE_TRACKS).noquote() << formatTraceEvent(
        correlation, QStringLiteral("processor_change"),
        {DiagnosticField::text(QStringLiteral("processor"), processor),
         DiagnosticField::text(QStringLiteral("field"), field), DiagnosticField::text(QStringLiteral("old"), oldValue),
         DiagnosticField::text(QStringLiteral("new"), newValue),
         DiagnosticField::text(QStringLiteral("provenance"), provenanceName(provenance)),
         DiagnosticField::text(QStringLiteral("reason"), reason)});
}

std::optional<QString> sharedTrackArtist(const QList<BandcampTrack>& siblings, bool complete,
                                         const std::atomic_bool* stop)
{
    if(!complete || siblings.isEmpty() || siblings.first().artist.isEmpty()) {
        return std::nullopt;
    }
    const QString candidate = siblings.first().artist;
    for(const BandcampTrack& sibling : siblings) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        if(sibling.artist.isEmpty() || !sameArtist(sibling.artist, candidate)) {
            return std::nullopt;
        }
    }
    return candidate;
}

} // namespace

QString AlbumArtistInferenceProcessor::name() const
{
    return QStringLiteral("album-artist-inference");
}

int AlbumArtistInferenceProcessor::process(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const
{
    if(!album.artist.isEmpty()) {
        return 0;
    }
    const auto inferred = sharedTrackArtist(context.siblings, context.completeSiblingSet, context.stop);
    if(!inferred) {
        return 0;
    }

    album.artist              = *inferred;
    context.provenance.artist = FieldProvenance::Derived;
    logAlbumChange(name(), QStringLiteral("album.artist"), QStringLiteral("<unset>"), album.artist,
                   QStringLiteral("all complete sibling tracks share the same artist"), context.provenance.artist,
                   context.correlation);
    return 1;
}

QString AlbumTitlePrefixProcessor::name() const
{
    return QStringLiteral("album-title-prefix");
}

int AlbumTitlePrefixProcessor::process(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const
{
    const DirtyTitle parts = splitDirtyTitle(album.title);
    if(!parts.matched || !sameArtist(parts.artist, album.artist) || parts.title.trimmed().isEmpty()) {
        return 0;
    }

    const QString old        = album.title;
    album.title              = parts.title;
    context.provenance.title = FieldProvenance::Derived;
    logAlbumChange(name(), QStringLiteral("album.title"), old, album.title,
                   QStringLiteral("matching album-artist prefix removed"), context.provenance.title,
                   context.correlation);
    return 1;
}

QString TrackArtistInferenceProcessor::name() const
{
    return QStringLiteral("track-artist-inference");
}

int TrackArtistInferenceProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    if(!track.artist.isEmpty() || !context.facts.inferredArtist) {
        return 0;
    }

    track.artist              = *context.facts.inferredArtist;
    context.provenance.artist = FieldProvenance::Derived;
    logTrackChange(name(), QStringLiteral("track.artist"), QStringLiteral("<unset>"), track.artist,
                   QStringLiteral("all complete sibling titles are shaped and have no explicit artists"),
                   context.provenance.artist, context.correlation);
    return 1;
}

QString TrackArtistFallbackProcessor::name() const
{
    return QStringLiteral("track-artist-fallback");
}

int TrackArtistFallbackProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    if(!track.artist.isEmpty()) {
        return 0;
    }

    QString fallback;
    FieldProvenance source = FieldProvenance::Fallback;
    if(context.parent && !context.parent->artist.isEmpty()) {
        fallback = context.parent->artist;
        source   = FieldProvenance::Inherited;
    }
    else if(context.fallbackArtist.isUsable()) {
        fallback = context.fallbackArtist.value;
    }
    if(fallback.isEmpty()) {
        return 0;
    }

    track.artist              = fallback;
    context.provenance.artist = source;
    logTrackChange(name(), QStringLiteral("track.artist"), QStringLiteral("<unset>"), track.artist,
                   context.parent && !context.parent->artist.isEmpty()
                       ? QStringLiteral("missing track artist inherited from its album")
                       : QStringLiteral("missing track artist filled from page fallback context"),
                   source, context.correlation);
    return 1;
}

QString TrackDateInheritanceProcessor::name() const
{
    return QStringLiteral("track-date-inheritance");
}

int TrackDateInheritanceProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    if(!context.parent || track.releaseDate.isValid() || !context.parent->releaseDate.isValid()) {
        return 0;
    }
    const QString value            = context.parent->releaseDate.toString(Qt::ISODate);
    track.releaseDate              = context.parent->releaseDate;
    context.provenance.releaseDate = FieldProvenance::Inherited;
    logTrackChange(name(), QStringLiteral("track.releaseDate"), QStringLiteral("<unset>"), value,
                   QStringLiteral("track has no owned release date; album release date inherited"),
                   context.provenance.releaseDate, context.correlation);
    return 1;
}

QString TrackArtworkInheritanceProcessor::name() const
{
    return QStringLiteral("track-artwork-inheritance");
}

int TrackArtworkInheritanceProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    if(!context.parent || track.artId != 0 || context.parent->artId <= 0) {
        return 0;
    }
    const qint64 old         = track.artId;
    track.artId              = context.parent->artId;
    context.provenance.artId = FieldProvenance::Inherited;
    logTrackChange(name(), QStringLiteral("track.artId"), old > 0 ? QString::number(old) : QStringLiteral("<unset>"),
                   QString::number(track.artId), QStringLiteral("track has no owned artwork; album artwork inherited"),
                   context.provenance.artId, context.correlation);
    return 1;
}

QString TrackArtworkFallbackProcessor::name() const
{
    return QStringLiteral("track-artwork-fallback");
}

int TrackArtworkFallbackProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    if(context.parent || track.artId != 0 || !context.fallbackArtId.isUsable() || context.fallbackArtId.value <= 0) {
        return 0;
    }
    track.artId              = context.fallbackArtId.value;
    context.provenance.artId = FieldProvenance::Fallback;
    logTrackChange(name(), QStringLiteral("track.artId"), QStringLiteral("<unset>"), QString::number(track.artId),
                   QStringLiteral("standalone track artwork filled from page fallback context"),
                   context.provenance.artId, context.correlation);
    return 1;
}

QString TrackTitlePrefixProcessor::name() const
{
    return QStringLiteral("track-title-prefix");
}

int TrackTitlePrefixProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    const DirtyTitle parts = splitDirtyTitle(track.title);
    if(!parts.matched || !sameArtist(parts.artist, track.artist) || parts.title.trimmed().isEmpty()) {
        return 0;
    }
    const QString old        = track.title;
    track.title              = parts.title;
    context.provenance.title = FieldProvenance::Derived;
    logTrackChange(name(), QStringLiteral("track.title"), old, track.title,
                   QStringLiteral("matching track-artist prefix removed"), context.provenance.title,
                   context.correlation);
    return 1;
}

QString TrackNumberRepairProcessor::name() const
{
    return QStringLiteral("track-number-repair");
}

int TrackNumberRepairProcessor::process(BandcampTrack& track, TrackProcessorContext& context) const
{
    if(!context.facts.repairedTrackNumber || *context.facts.repairedTrackNumber == track.trackNumber) {
        return 0;
    }
    const int old                  = track.trackNumber;
    track.trackNumber              = *context.facts.repairedTrackNumber;
    context.provenance.trackNumber = FieldProvenance::Derived;
    logTrackChange(name(), QStringLiteral("track.trackNumber"),
                   old > 0 ? QString::number(old) : QStringLiteral("<unset>"), QString::number(track.trackNumber),
                   QStringLiteral("missing number filled without changing valid supplied numbers"),
                   context.provenance.trackNumber, context.correlation);
    return 1;
}
} // namespace Fooyin::Bandcamp
