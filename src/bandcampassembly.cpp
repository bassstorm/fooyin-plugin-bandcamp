#include "bandcampassembly.h"

#include "bandcampdiagnostics.h"
#include "bandcampdirtytitle.h"
#include "bandcamplogging.h"
#include "bandcampparser.h"

#include <QLoggingCategory>
#include <QSet>

#include <algorithm>
#include <atomic>

namespace Fooyin::Bandcamp {
namespace {
ResolutionFailure failure(ResolutionErrorCode code, ResolutionStage stage, const QUrl& url, qint64 trackId,
                          const QString& field, const QString& reason)
{
    return {{code, stage, url, trackId, field, reason}};
}

ResolutionFailure withAlbum(ResolutionFailure result, qint64 albumId)
{
    if(result.error.albumId <= 0) {
        result.error.albumId = albumId;
    }
    return result;
}

template <typename T>
std::optional<ResolutionFailure> validateRequired(const CandidateField<T>& field, const QString& name, const QUrl& url,
                                                  qint64 trackId = 0)
{
    if(field.state == FieldState::Missing) {
        ResolutionFailure result = failure(ResolutionErrorCode::RequiredFieldMissing, ResolutionStage::Admit, url,
                                           trackId, name, QStringLiteral("required field is missing"));
        result.error.value       = QStringLiteral("null");
        return result;
    }
    if(!field.isUsable()) {
        ResolutionFailure result = failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, url, trackId,
                                           name, QStringLiteral("required field is invalid"));
        result.error.value       = QStringLiteral("invalid");
        return result;
    }
    return std::nullopt;
}

std::optional<ResolutionFailure> validateTrackRequired(const TrackCandidate& source, const QUrl& url, qint64 trackId,
                                                       bool requireTitle = true)
{
    if(source.recordState == FieldState::Invalid) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, url, trackId,
                       QStringLiteral("track.id"), QStringLiteral("declared child record is malformed"));
    }
    if(auto error = validateRequired(source.id, QStringLiteral("track.id"), url, trackId)) {
        return error;
    }
    if(requireTitle) {
        if(auto error = validateRequired(source.title, QStringLiteral("track.title"), url, source.id.value)) {
            return error;
        }
    }
    return std::nullopt;
}

BandcampAlbumMetadata makeAlbumMetadata(const AlbumCandidate& source, const QUrl& sourceUrl)
{
    BandcampAlbumMetadata metadata;
    metadata.id          = source.id.value;
    metadata.title       = source.title.value;
    metadata.artist      = source.artist.isUsable() ? source.artist.value : QString{};
    metadata.sourceUrl   = sourceUrl.toString(QUrl::FullyEncoded);
    metadata.releaseDate = source.releaseDate.isUsable() ? source.releaseDate.value : QDate{};
    metadata.trackTotal  = static_cast<int>(source.tracks.size());
    return metadata;
}

AlbumMetadataProvenance albumProvenance(const AlbumCandidate& source)
{
    return {source.title.provenance, source.artist.provenance, source.releaseDate.provenance};
}

TrackMetadataProvenance trackProvenance(const TrackCandidate& source)
{
    return {source.title.provenance,      source.artist.provenance,      source.trackNumber.provenance,
            source.durationMs.provenance, source.releaseDate.provenance, source.streamUrl.provenance};
}

BandcampTrack makeTrack(const TrackCandidate& source, const QString& sourceUrl)
{
    BandcampTrack track;
    track.id          = source.id.value;
    track.title       = source.title.value;
    track.artist      = source.artist.isUsable() ? source.artist.value : QString{};
    track.sourceUrl   = sourceUrl;
    track.streamUrl   = source.streamUrl.value;
    track.releaseDate = source.releaseDate.isUsable() ? source.releaseDate.value : QDate{};
    track.trackNumber = source.trackNumber.isUsable() ? static_cast<int>(source.trackNumber.value) : 0;
    track.durationMs  = source.durationMs.isUsable() ? source.durationMs.value : 0;
    return track;
}

template <typename T>
bool differs(const T& left, const T& right, const QString&)
{
    return left != right;
}

bool differs(const QString& left, const QString& right, const QString& field)
{
    if(field == QStringLiteral("track.artist")) {
        return !sameArtist(left, right);
    }
    return left != right;
}

template <typename T>
CandidateField<T> authoritative(const CandidateField<T>& parent, const CandidateField<T>& selectedPage,
                                const CorrelationContext& context, const QString& field)
{
    if(!parent.isUsable()) {
        return selectedPage;
    }
    if(!selectedPage.isUsable()) {
        return parent;
    }
    const bool parentExplicit = parent.provenance == FieldProvenance::Explicit;
    const bool pageExplicit   = selectedPage.provenance == FieldProvenance::Explicit;
    const bool chooseParent   = parentExplicit || !pageExplicit;
    if(parentExplicit && pageExplicit && differs(parent.value, selectedPage.value, field)) {
        logFieldConflict(BANDCAMP_RESOLVE(), context, field,
                         describeCandidateField(chooseParent ? parent : selectedPage),
                         describeCandidateField(chooseParent ? selectedPage : parent));
    }
    return chooseParent ? parent : selectedPage;
}

TrackAdmission makeTrackAdmission(const TrackPageCandidate& page, const TrackCandidate& assembled,
                                  std::optional<BandcampAlbumMetadata> parent, const QString& releaseSourceUrl,
                                  AlbumMetadataProvenance parentProvenance, const QList<BandcampTrack>& siblings,
                                  bool completeSiblingSet, qsizetype siblingIndex, CorrelationContext context)
{
    TrackAdmission admission;
    admission.result.album       = std::move(parent);
    admission.parentProvenance   = parentProvenance;
    const QString source         = admission.result.album ? admission.result.album->sourceUrl : releaseSourceUrl;
    admission.result.track       = makeTrack(assembled, source);
    admission.trackProvenance    = trackProvenance(assembled);
    admission.fallbackArtist     = page.fallbackArtist;
    admission.siblings           = siblings;
    admission.completeSiblingSet = completeSiblingSet;
    admission.siblingIndex       = siblingIndex;
    admission.correlation        = context.withTrack(admission.result.track.id);
    return admission;
}

std::variant<BandcampAlbumMetadata, ResolutionFailure> admitParentMetadata(const AlbumCandidate& parent,
                                                                           const QUrl& declaredUrl,
                                                                           std::optional<qint64> declaredId,
                                                                           const CorrelationContext& context)
{
    if(auto error
       = validateRequired(parent.sourceUrl, QStringLiteral("album.sourceUrl"),
                          parent.sourceUrl.isUsable() ? parent.sourceUrl.value : declaredUrl, context.trackId)) {
        return *error;
    }
    if(auto missing = validateRequired(parent.id, QStringLiteral("album.id"), declaredUrl, context.trackId)) {
        return *missing;
    }
    if(declaredId && parent.id.value != *declaredId) {
        return withAlbum(failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, declaredUrl,
                                 context.trackId, QStringLiteral("parent.albumId"),
                                 QStringLiteral("fetched parent ID does not match the declared album ID")),
                         *declaredId);
    }
    if(auto missing = validateRequired(parent.title, QStringLiteral("album.title"), declaredUrl, context.trackId)) {
        return withAlbum(*missing, parent.id.value);
    }
    return makeAlbumMetadata(parent, parent.sourceUrl.value);
}

TrackCandidate selectedTrack(const TrackCandidate& parentChild, const TrackPageCandidate& page,
                             const CorrelationContext& context)
{
    TrackCandidate result = parentChild;
    result.title          = authoritative(parentChild.title, page.track.title, context, QStringLiteral("track.title"));
    result.artist = authoritative(parentChild.artist, page.track.artist, context, QStringLiteral("track.artist"));
    result.trackNumber
        = authoritative(parentChild.trackNumber, page.track.trackNumber, context, QStringLiteral("track.trackNumber"));
    result.durationMs
        = authoritative(parentChild.durationMs, page.track.durationMs, context, QStringLiteral("track.durationMs"));
    result.releaseDate = page.track.releaseDate;
    result.id          = parentChild.id;
    result.recordState = parentChild.recordState;
    return result;
}

} // namespace

ParentRequirement classifyParentEvidence(const TrackPageCandidate& candidate, CorrelationContext context)
{
    const QUrl resource    = context.resourceUrl.isValid() ? context.resourceUrl : candidate.sourceUrl.value;
    const bool hasLocation = candidate.parent.albumUrl.isUsable();
    std::optional<qint64> declaredId;
    if(candidate.parent.albumId.isUsable()) {
        declaredId = candidate.parent.albumId.value;
    }

    auto required = [&]() -> ParentRequirement {
        if(!hasLocation) {
            return failure(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire, resource,
                           candidate.track.id.isUsable() ? candidate.track.id.value : 0,
                           QStringLiteral("parent.albumUrl"),
                           QStringLiteral("declared album parent has no usable location"));
        }
        return RequiredAlbumParent{candidate.parent.albumUrl.value, declaredId};
    };

    switch(candidate.parent.declaration) {
        case ParentDeclaration::NoParent:
            if(candidate.parent.albumId.isUsable()) {
                return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, resource,
                               candidate.track.id.isUsable() ? candidate.track.id.value : 0,
                               QStringLiteral("parent.albumId"),
                               QStringLiteral("no-parent declaration conflicts with a positive album ID"));
            }
            if(hasLocation) {
                return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, resource,
                               candidate.track.id.isUsable() ? candidate.track.id.value : 0,
                               QStringLiteral("parent.albumUrl"),
                               QStringLiteral("no-parent declaration conflicts with a usable album location"));
            }
            return StandaloneParentEvidence{};
        case ParentDeclaration::AlbumBacked:
            if(!candidate.parent.albumId.isUsable()) {
                return failure(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire, resource,
                               candidate.track.id.isUsable() ? candidate.track.id.value : 0,
                               QStringLiteral("parent.albumId"), QStringLiteral("declared album ID is unusable"));
            }
            return required();
        case ParentDeclaration::Unknown:
            if(hasLocation) {
                return required();
            }
            return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, resource,
                           candidate.track.id.isUsable() ? candidate.track.id.value : 0, QStringLiteral("parent"),
                           QStringLiteral("parent membership is unknown without a usable location"));
        case ParentDeclaration::Invalid:
            if(hasLocation) {
                return required();
            }
            return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, resource,
                           candidate.track.id.isUsable() ? candidate.track.id.value : 0, QStringLiteral("parent"),
                           QStringLiteral("invalid parent evidence cannot establish standalone status"));
    }
    return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, resource, 0,
                   QStringLiteral("parent"), QStringLiteral("unrecognized parent evidence"));
}

ResolutionOutcome<AlbumAdmission> admitAlbum(const AlbumCandidate& candidate, CorrelationContext context,
                                             const std::atomic_bool* stop)
{
    if(stop && stop->load(std::memory_order_relaxed)) {
        return ResolutionCancelled{};
    }
    context.resourceUrl = context.resourceUrl.isValid() ? context.resourceUrl : candidate.sourceUrl.value;
    if(auto error
       = validateRequired(candidate.sourceUrl, QStringLiteral("album.sourceUrl"),
                          candidate.sourceUrl.isUsable() ? candidate.sourceUrl.value : context.resourceUrl)) {
        return *error;
    }
    const QUrl& sourceUrl = candidate.sourceUrl.value;
    if(auto error = validateRequired(candidate.id, QStringLiteral("album.id"), sourceUrl)) {
        return *error;
    }
    context = context.withAlbum(candidate.id.value);
    if(auto error = validateRequired(candidate.title, QStringLiteral("album.title"), sourceUrl)) {
        return withAlbum(*error, candidate.id.value);
    }
    if(candidate.tracks.isEmpty()) {
        return withAlbum(failure(ResolutionErrorCode::NoResolvableTracks, ResolutionStage::Admit, sourceUrl, 0,
                                 QStringLiteral("members"), QStringLiteral("album declares no tracks")),
                         candidate.id.value);
    }
    AlbumAdmission admission;
    admission.album.metadata     = makeAlbumMetadata(candidate, sourceUrl);
    admission.correlation        = context;
    admission.metadataProvenance = albumProvenance(candidate);
    admission.album.tracks.reserve(candidate.tracks.size());
    admission.trackProvenance.reserve(candidate.tracks.size());

    QSet<qint64> trackIds;
    for(const TrackCandidate& child : candidate.tracks) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const qint64 id = child.id.isUsable() ? child.id.value : 0;
        if(auto error = validateTrackRequired(child, sourceUrl, id)) {
            return withAlbum(*error, candidate.id.value);
        }
        if(trackIds.contains(id)) {
            return withAlbum(failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Admit, sourceUrl, id,
                                     QStringLiteral("track.id"),
                                     QStringLiteral("duplicate child ID in declared album membership")),
                             candidate.id.value);
        }
        trackIds.insert(id);
        if(auto error
           = validateRequired(child.streamUrl, QStringLiteral("track.streamUrl"), sourceUrl, child.id.value)) {
            return withAlbum(*error, candidate.id.value);
        }
        admission.album.tracks.append(makeTrack(child, admission.album.metadata.sourceUrl));
        admission.trackProvenance.append(trackProvenance(child));
    }
    return ResolutionSuccess<AlbumAdmission>{std::move(admission)};
}

ResolutionOutcome<TrackAdmission> admitStandaloneTrack(const TrackPageCandidate& candidate, CorrelationContext context,
                                                       const std::atomic_bool* stop)
{
    if(stop && stop->load(std::memory_order_relaxed)) {
        return ResolutionCancelled{};
    }
    context.resourceUrl    = context.resourceUrl.isValid() ? context.resourceUrl : candidate.sourceUrl.value;
    const auto requirement = classifyParentEvidence(candidate, context);
    if(std::holds_alternative<ResolutionFailure>(requirement)) {
        return std::get<ResolutionFailure>(requirement);
    }
    if(!std::holds_alternative<StandaloneParentEvidence>(requirement)) {
        return failure(ResolutionErrorCode::ParentUnavailable, ResolutionStage::Acquire, context.resourceUrl,
                       candidate.track.id.isUsable() ? candidate.track.id.value : 0, QStringLiteral("parent"),
                       QStringLiteral("album-backed track cannot be admitted without its parent"));
    }
    if(auto error = validateTrackRequired(candidate.track, context.resourceUrl,
                                          candidate.track.id.isUsable() ? candidate.track.id.value : 0, false)) {
        return *error;
    }
    if(auto error = validateRequired(candidate.sourceUrl, QStringLiteral("track.sourceUrl"),
                                     candidate.sourceUrl.isUsable() ? candidate.sourceUrl.value : context.resourceUrl,
                                     candidate.track.id.value)) {
        return *error;
    }
    const QUrl& sourceUrl = candidate.sourceUrl.value;
    if(!candidate.matchingRecord.isUsable()) {
        return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Admit, sourceUrl,
                       candidate.track.id.value, QStringLiteral("track.streamUrl"),
                       QStringLiteral("standalone track has no unique matching source record"));
    }
    if(auto error
       = validateRequired(candidate.track.title, QStringLiteral("track.title"), sourceUrl, candidate.track.id.value)) {
        return *error;
    }
    if(auto error = validateRequired(candidate.track.streamUrl, QStringLiteral("track.streamUrl"), sourceUrl,
                                     candidate.track.id.value)) {
        return *error;
    }

    return ResolutionSuccess<TrackAdmission>{makeTrackAdmission(candidate, candidate.track, std::nullopt,
                                                                sourceUrl.toString(QUrl::FullyEncoded), {}, {}, false,
                                                                -1, context.withTrack(candidate.track.id.value))};
}

ResolutionOutcome<TrackAdmission> admitAlbumTrack(const TrackPageCandidate& trackPage, const AlbumCandidate& album,
                                                  CorrelationContext context, const std::atomic_bool* stop)
{
    if(stop && stop->load(std::memory_order_relaxed)) {
        return ResolutionCancelled{};
    }
    context.resourceUrl    = context.resourceUrl.isValid() ? context.resourceUrl : trackPage.sourceUrl.value;
    const auto requirement = classifyParentEvidence(trackPage, context);
    if(std::holds_alternative<ResolutionFailure>(requirement)) {
        return std::get<ResolutionFailure>(requirement);
    }
    if(!std::holds_alternative<RequiredAlbumParent>(requirement)) {
        return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, context.resourceUrl,
                       trackPage.track.id.isUsable() ? trackPage.track.id.value : 0, QStringLiteral("parent"),
                       QStringLiteral("standalone declaration cannot use an album parent"));
    }
    const auto requiredParent = std::get<RequiredAlbumParent>(requirement);
    const CorrelationContext trackContext
        = context.withTrack(trackPage.track.id.isUsable() ? trackPage.track.id.value : 0);
    if(auto error = validateRequired(trackPage.track.id, QStringLiteral("track.id"), context.resourceUrl)) {
        return *error;
    }
    const auto parentOutcome = admitParentMetadata(album, requiredParent.url, requiredParent.albumId, trackContext);
    if(const auto* parentError = std::get_if<ResolutionFailure>(&parentOutcome)) {
        ResolutionFailure failure = *parentError;
        failure.error.trackId     = trackPage.track.id.value;
        return failure;
    }
    BandcampAlbumMetadata parentMetadata  = std::get<BandcampAlbumMetadata>(parentOutcome);
    const CorrelationContext albumContext = trackContext.withAlbum(parentMetadata.id);

    qsizetype matchingIndex = -1;
    int matchingCount       = 0;
    for(qsizetype i = 0; i < album.tracks.size(); ++i) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const auto& child = album.tracks.at(i);
        if(child.id.isUsable() && child.id.value == trackPage.track.id.value) {
            matchingIndex = i;
            ++matchingCount;
        }
    }
    if(matchingCount != 1) {
        return withAlbum(failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble,
                                 requiredParent.url, trackPage.track.id.value, QStringLiteral("parent.membership"),
                                 matchingCount == 0 ? QStringLiteral("selected track is not a declared album child")
                                                    : QStringLiteral("selected child ID is not unique in the album")),
                         parentMetadata.id);
    }

    const TrackCandidate& child = album.tracks.at(matchingIndex);
    if(auto error = validateTrackRequired(child, requiredParent.url, trackPage.track.id.value, false)) {
        return withAlbum(*error, parentMetadata.id);
    }
    if(auto error = validateRequired(child.streamUrl, QStringLiteral("track.streamUrl"), requiredParent.url,
                                     trackPage.track.id.value)) {
        return withAlbum(*error, parentMetadata.id);
    }

    TrackCandidate selected = selectedTrack(child, trackPage, albumContext);
    if(child.streamUrl.provenance == FieldProvenance::Explicit
       && trackPage.track.streamUrl.provenance == FieldProvenance::Explicit
       && child.streamUrl.value != trackPage.track.streamUrl.value) {
        // Signed variants of the same file differ only in query tokens, so only path or host differences are conflicts.
        const QString chosenStream  = cleanUrlForLog(QUrl(child.streamUrl.value));
        const QString ignoredStream = cleanUrlForLog(QUrl(trackPage.track.streamUrl.value));
        if(chosenStream != ignoredStream) {
            logFieldConflict(BANDCAMP_RESOLVE(), albumContext, QStringLiteral("track.streamUrl"), chosenStream,
                             ignoredStream);
        }
    }
    if(selected.title.state == FieldState::Invalid) {
        return withAlbum(failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, requiredParent.url,
                                 trackPage.track.id.value, QStringLiteral("track.title"),
                                 QStringLiteral("selected track title is invalid on both pages")),
                         parentMetadata.id);
    }
    if(!selected.title.isUsable()) {
        return withAlbum(failure(ResolutionErrorCode::RequiredFieldMissing, ResolutionStage::Admit, requiredParent.url,
                                 trackPage.track.id.value, QStringLiteral("track.title"),
                                 QStringLiteral("selected track has no usable title on either page")),
                         parentMetadata.id);
    }
    QList<BandcampTrack> siblings;
    bool complete = true;
    QSet<qint64> siblingIds;
    for(qsizetype i = 0; i < album.tracks.size(); ++i) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const TrackCandidate& member = album.tracks.at(i);
        TrackCandidate resolved      = i == matchingIndex ? selected : member;
        const qint64 siblingId       = resolved.id.isUsable() ? resolved.id.value : 0;
        if(siblingIds.contains(siblingId)
           || (i != matchingIndex
               && (validateTrackRequired(resolved, requiredParent.url, siblingId)
                   || validateRequired(resolved.streamUrl, QStringLiteral("track.streamUrl"), requiredParent.url,
                                       siblingId)))) {
            complete = false;
            break;
        }
        siblingIds.insert(siblingId);
        siblings.append(makeTrack(resolved, parentMetadata.sourceUrl));
    }
    if(!complete) {
        siblings.clear();
    }

    TrackAdmission admission
        = makeTrackAdmission(trackPage, selected, parentMetadata, parentMetadata.sourceUrl, albumProvenance(album),
                             siblings, complete, matchingIndex, albumContext);
    return ResolutionSuccess<TrackAdmission>{std::move(admission)};
}
} // namespace Fooyin::Bandcamp
