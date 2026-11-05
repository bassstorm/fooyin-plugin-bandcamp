#include "bandcamppostprocessor.h"

#include "bandcampdirtytitle.h"
#include "bandcamplogging.h"

#include <QHash>
#include <QLoggingCategory>

#include <utility>

namespace Fooyin::Bandcamp {
namespace {
QList<TrackProcessorFacts> prepareTrackProcessorFacts(const QList<BandcampTrack>& siblings, bool completeSiblingSet,
                                                      const std::atomic_bool* stop)
{
    QList<TrackProcessorFacts> facts;
    facts.resize(siblings.size());
    if(!completeSiblingSet || siblings.isEmpty()) {
        return facts;
    }

    bool canInferArtists = true;
    QList<QString> inferredArtists;
    inferredArtists.reserve(siblings.size());
    for(const BandcampTrack& sibling : siblings) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return facts;
        }
        if(!sibling.artist.isEmpty()) {
            canInferArtists = false;
            break;
        }
        const DirtyTitle parts = splitDirtyTitle(sibling.title);
        if(!parts.matched) {
            canInferArtists = false;
            break;
        }
        inferredArtists.append(parts.artist);
    }
    if(canInferArtists) {
        for(qsizetype i = 0; i < facts.size(); ++i) {
            if(stop && stop->load(std::memory_order_relaxed)) {
                return facts;
            }
            facts[i].inferredArtist = std::move(inferredArtists[i]);
        }
    }

    QHash<int, int> occupiedSuccessors;
    for(const BandcampTrack& sibling : siblings) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return facts;
        }
        if(sibling.trackNumber > 0 && sibling.trackNumber <= MaxTrackNumber) {
            occupiedSuccessors.insert(sibling.trackNumber, sibling.trackNumber + 1);
        }
    }

    // Compress occupied ranges so repeated explicit numbers cannot cause quadratic rescanning.
    const auto firstAvailable = [&occupiedSuccessors, stop](int number) {
        int available = number;
        while(occupiedSuccessors.contains(available) && (!stop || !stop->load(std::memory_order_relaxed))) {
            available = occupiedSuccessors.value(available);
        }
        while(number != available) {
            const int next             = occupiedSuccessors.value(number);
            occupiedSuccessors[number] = available;
            number                     = next;
        }
        return available;
    };

    int previous = 0;
    for(qsizetype i = 0; i < siblings.size(); ++i) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return facts;
        }
        const int supplied = siblings.at(i).trackNumber;
        if(supplied > 0 && supplied <= MaxTrackNumber) {
            previous = supplied;
            continue;
        }

        const int candidate = firstAvailable(previous + 1);
        if(candidate > MaxTrackNumber) {
            continue;
        }
        previous = candidate;
        occupiedSuccessors.insert(candidate, candidate + 1);
        facts[i].repairedTrackNumber = candidate;
    }
    return facts;
}
} // namespace

void BandcampAlbumProcessorPipeline::add(std::unique_ptr<AlbumPostProcessor> processor)
{
    m_steps.push_back(std::move(processor));
}

void BandcampAlbumProcessorPipeline::run(BandcampAlbumMetadata& album, AlbumProcessorContext& context) const
{
    for(const auto& processor : m_steps) {
        if(context.stop && context.stop->load(std::memory_order_relaxed)) {
            return;
        }
        processor->process(album, context);
    }
}

void BandcampTrackProcessorPipeline::add(std::unique_ptr<TrackPostProcessor> processor)
{
    m_steps.push_back(std::move(processor));
}

void BandcampTrackProcessorPipeline::run(BandcampTrack& track, TrackProcessorContext& context) const
{
    for(const auto& processor : m_steps) {
        if(context.stop && context.stop->load(std::memory_order_relaxed)) {
            return;
        }
        processor->process(track, context);
    }
}

const BandcampAlbumProcessorPipeline& standardAlbumProcessors()
{
    static const BandcampAlbumProcessorPipeline pipeline = [] {
        BandcampAlbumProcessorPipeline built;
        built.add(std::make_unique<AlbumArtistInferenceProcessor>());
        built.add(std::make_unique<AlbumTitlePrefixProcessor>());
        return built;
    }();
    return pipeline;
}

const BandcampTrackProcessorPipeline& standardTrackProcessors()
{
    static const BandcampTrackProcessorPipeline pipeline = [] {
        BandcampTrackProcessorPipeline built;
        built.add(std::make_unique<TrackArtistInferenceProcessor>());
        built.add(std::make_unique<TrackArtistFallbackProcessor>());
        built.add(std::make_unique<TrackDateInheritanceProcessor>());
        built.add(std::make_unique<TrackTitlePrefixProcessor>());
        built.add(std::make_unique<TrackNumberRepairProcessor>());
        return built;
    }();
    return pipeline;
}

void postProcessAlbum(AlbumAdmission& admission, const std::atomic_bool* stop)
{
    const QList<BandcampTrack> siblings    = admission.album.tracks;
    const QList<TrackProcessorFacts> facts = prepareTrackProcessorFacts(siblings, true, stop);
    AlbumProcessorContext albumContext{siblings, true, admission.correlation, admission.metadataProvenance, stop};
    standardAlbumProcessors().run(admission.album.metadata, albumContext);

    for(qsizetype i = 0; i < admission.album.tracks.size(); ++i) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return;
        }
        TrackProcessorContext trackContext{&admission.album.metadata,
                                           siblings,
                                           true,
                                           i,
                                           {},
                                           admission.correlation.withTrack(admission.album.tracks.at(i).id),
                                           admission.trackProvenance[i],
                                           facts.at(i),
                                           stop};
        standardTrackProcessors().run(admission.album.tracks[i], trackContext);
    }
}

void postProcessTrack(TrackAdmission& admission, const std::atomic_bool* stop)
{
    if(stop && stop->load(std::memory_order_relaxed)) {
        return;
    }
    if(admission.result.album) {
        CorrelationContext parentCorrelation = admission.correlation;
        parentCorrelation.trackId            = 0;
        AlbumProcessorContext albumContext{admission.siblings, admission.completeSiblingSet, admission.correlation,
                                           admission.parentProvenance, stop};
        albumContext.correlation = parentCorrelation;
        standardAlbumProcessors().run(*admission.result.album, albumContext);
    }

    const QList<TrackProcessorFacts> facts
        = prepareTrackProcessorFacts(admission.siblings, admission.completeSiblingSet, stop);
    if(stop && stop->load(std::memory_order_relaxed)) {
        return;
    }
    TrackProcessorFacts trackFacts;
    if(admission.siblingIndex >= 0 && admission.siblingIndex < facts.size()) {
        trackFacts = facts.at(admission.siblingIndex);
    }
    TrackProcessorContext trackContext{admission.result.album ? &*admission.result.album : nullptr,
                                       admission.siblings,
                                       admission.completeSiblingSet,
                                       admission.siblingIndex,
                                       admission.fallbackArtist,
                                       admission.correlation.withTrack(admission.result.track.id),
                                       admission.trackProvenance,
                                       std::move(trackFacts),
                                       stop};
    standardTrackProcessors().run(admission.result.track, trackContext);
}

namespace {
void logMutation(const QString& processor, const QString& oldValue, const QString& newValue,
                 const CorrelationContext& correlation)
{
    if(!BANDCAMP_TRACKS().isDebugEnabled()) {
        return;
    }
    qCDebug(BANDCAMP_TRACKS).noquote() << formatTraceEvent(
        correlation, QStringLiteral("mutated"),
        {DiagnosticField::text(QStringLiteral("processor"), processor),
         DiagnosticField::text(QStringLiteral("old"), oldValue), DiagnosticField::text(QStringLiteral("new"), newValue)});
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
    logMutation(name(), QStringLiteral("<unset>"), album.artist, context.correlation);
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
    logMutation(name(), old, album.title, context.correlation);
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
    // Inferred artist equal to the parent album artist is inherited by the fallback processor instead.
    if(context.parent && sameArtist(*context.facts.inferredArtist, context.parent->artist)) {
        return 0;
    }

    track.artist              = *context.facts.inferredArtist;
    context.provenance.artist = FieldProvenance::Derived;
    logMutation(name(), QStringLiteral("<unset>"), track.artist, context.correlation);
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
    if(source == FieldProvenance::Fallback) {
        logMutation(name(), QStringLiteral("<unset>"), track.artist, context.correlation);
    }
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
    track.releaseDate              = context.parent->releaseDate;
    context.provenance.releaseDate = FieldProvenance::Inherited;
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
    logMutation(name(), old, track.title, context.correlation);
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
    logMutation(name(), old > 0 ? QString::number(old) : QStringLiteral("<unset>"),
                QString::number(track.trackNumber), context.correlation);
    return 1;
}
} // namespace Fooyin::Bandcamp
