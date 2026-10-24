#include "bandcampassembly.h"

#include "bandcampdirtytitle.h"
#include "bandcamplogging.h"
#include "bandcampurl.h"

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

template <typename T>
std::optional<ResolutionFailure> validateRequired(const CandidateField<T>& field, const QString& name, const QUrl& url,
                                                  qint64 trackId = 0)
{
    if(field.state == FieldState::Missing) {
        return failure(ResolutionErrorCode::RequiredFieldMissing, ResolutionStage::Admit, url, trackId, name,
                       QStringLiteral("required field is missing"));
    }
    if(!field.isUsable()) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, url, trackId, name,
                       QStringLiteral("required field is invalid"));
    }
    return std::nullopt;
}

template <typename T>
void markInvalid(CandidateField<T>& field)
{
    field.state = FieldState::Invalid;
}

void normalizeTrackOptionals(TrackCandidate& track)
{
    if(track.title.isUsable() && track.title.value.trimmed().isEmpty()) {
        markInvalid(track.title);
    }
    if(track.artist.isUsable() && track.artist.value.trimmed().isEmpty()) {
        markInvalid(track.artist);
    }
    if(track.trackNumber.isUsable() && (track.trackNumber.value <= 0 || track.trackNumber.value > MaxTrackNumber)) {
        markInvalid(track.trackNumber);
    }
    if(track.durationMs.isUsable() && (track.durationMs.value < 0 || track.durationMs.value > MaxDurationMs)) {
        markInvalid(track.durationMs);
    }
    if(track.releaseDate.isUsable() && !track.releaseDate.value.isValid()) {
        markInvalid(track.releaseDate);
    }
    if(track.artId.isUsable() && track.artId.value <= 0) {
        markInvalid(track.artId);
    }
}

bool normalizeAlbumOptionals(AlbumCandidate& album, const std::atomic_bool* stop)
{
    if(album.artist.isUsable() && album.artist.value.trimmed().isEmpty()) {
        markInvalid(album.artist);
    }
    if(album.releaseDate.isUsable() && !album.releaseDate.value.isValid()) {
        markInvalid(album.releaseDate);
    }
    if(album.artId.isUsable() && album.artId.value <= 0) {
        markInvalid(album.artId);
    }
    for(TrackCandidate& track : album.tracks) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return false;
        }
        normalizeTrackOptionals(track);
    }
    return !stop || !stop->load(std::memory_order_relaxed);
}

void normalizeTrackPageOptionals(TrackPageCandidate& page)
{
    normalizeTrackOptionals(page.track);
    if(page.fallbackArtist.isUsable() && page.fallbackArtist.value.trimmed().isEmpty()) {
        markInvalid(page.fallbackArtist);
    }
    if(page.fallbackArtId.isUsable() && page.fallbackArtId.value <= 0) {
        markInvalid(page.fallbackArtId);
    }
}

QUrl candidateUrl(const CandidateField<QUrl>& field, const CorrelationContext& context)
{
    return field.isUsable() ? field.value : context.resourceUrl;
}

std::optional<QUrl> canonicalUrl(const CandidateField<QUrl>& field, BandcampPageKind expected, const QString& name,
                                 const CorrelationContext& context, std::optional<ResolutionFailure>& error,
                                 bool allowDecoration = false)
{
    if(auto invalid = validateRequired(field, name, candidateUrl(field, context))) {
        error = std::move(invalid);
        return std::nullopt;
    }

    const QUrl& supplied = field.value;
    if(!supplied.isValid() || supplied.host().isEmpty() || !supplied.userInfo().isEmpty()
       || (supplied.scheme() != QStringLiteral("https") && supplied.scheme() != QStringLiteral("http"))
       || (!allowDecoration && (supplied.hasQuery() || supplied.hasFragment()))) {
        error = failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, supplied, 0, name,
                        QStringLiteral("source URL is not canonical"));
        return std::nullopt;
    }

    const auto parsed = parseBandcampPageUrl(supplied.toString(QUrl::FullyEncoded));
    if(!parsed.isValid() || parsed.kind != expected) {
        error = failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, supplied, 0, name,
                        QStringLiteral("source URL has the wrong release kind"));
        return std::nullopt;
    }
    return QUrl(parsed.url, QUrl::StrictMode);
}

std::optional<ResolutionFailure> validateStream(const CandidateField<QString>& field, const QUrl& url, qint64 trackId)
{
    if(auto error = validateRequired(field, QStringLiteral("track.streamUrl"), url, trackId)) {
        return error;
    }
    const QUrl parsed(field.value, QUrl::StrictMode);
    if(!parsed.isValid() || parsed.scheme() != QStringLiteral("https") || parsed.host().isEmpty()
       || !parsed.userInfo().isEmpty() || parsed.hasFragment()) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, url, trackId,
                       QStringLiteral("track.streamUrl"),
                       QStringLiteral("stream URL is not a supported HTTPS locator"));
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
    if(source.id.value <= 0) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, url, trackId,
                       QStringLiteral("track.id"), QStringLiteral("track ID must be positive"));
    }
    if(requireTitle) {
        if(auto error = validateRequired(source.title, QStringLiteral("track.title"), url, source.id.value)) {
            return error;
        }
        if(source.title.value.trimmed().isEmpty()) {
            return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, url, source.id.value,
                           QStringLiteral("track.title"), QStringLiteral("track title is blank"));
        }
    }
    return std::nullopt;
}

std::optional<ResolutionFailure> validateTrackCandidate(const TrackCandidate& source, const QUrl& url,
                                                        qint64 fallbackId)
{
    const qint64 id = source.id.isUsable() ? source.id.value : fallbackId;
    if(auto error = validateTrackRequired(source, url, id)) {
        return error;
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
    metadata.artId       = source.artId.isUsable() ? source.artId.value : 0;
    metadata.trackTotal  = static_cast<int>(source.tracks.size());
    return metadata;
}

void logAlbumComposition(const CorrelationContext& context, const QUrl& suppliedSourceUrl,
                         const QUrl& canonicalSourceUrl, qsizetype declaredTracks)
{
    if(!BANDCAMP_TRACE_RESOLVE().isDebugEnabled()) {
        return;
    }
    qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
        context, QStringLiteral("assembly_field"),
        {DiagnosticField::text(QStringLiteral("field"), QStringLiteral("album.sourceUrl")),
         DiagnosticField::url(QStringLiteral("old"), suppliedSourceUrl),
         DiagnosticField::url(QStringLiteral("new"), canonicalSourceUrl),
         DiagnosticField::text(QStringLiteral("source"), QStringLiteral("validated album candidate")),
         DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("canonical release URL admitted"))});
    qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
        context, QStringLiteral("assembly_field"),
        {DiagnosticField::text(QStringLiteral("field"), QStringLiteral("album.trackTotal")),
         DiagnosticField::text(QStringLiteral("old"), QStringLiteral("<unset>")),
         DiagnosticField::number(QStringLiteral("new"), declaredTracks),
         DiagnosticField::text(QStringLiteral("source"), QStringLiteral("declared album membership")),
         DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("total derived from declared membership"))});
}

AlbumMetadataProvenance albumProvenance(const AlbumCandidate& source)
{
    return {source.title.provenance, source.artist.provenance, source.releaseDate.provenance, source.artId.provenance};
}

TrackMetadataProvenance trackProvenance(const TrackCandidate& source)
{
    return {source.title.provenance,      source.artist.provenance,      source.trackNumber.provenance,
            source.durationMs.provenance, source.releaseDate.provenance, source.artId.provenance,
            source.streamUrl.provenance};
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
    track.artId       = source.artId.isUsable() ? source.artId.value : 0;
    track.trackNumber = source.trackNumber.isUsable() ? static_cast<int>(source.trackNumber.value) : 0;
    track.durationMs  = source.durationMs.isUsable() ? source.durationMs.value : 0;
    return track;
}

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

void logConflict(const CorrelationContext& context, const QString& field, const QString& albumChildValue,
                 const QString& selectedPageValue, const QString& decision, FieldProvenance chosen,
                 const QString& reason)
{
    qCDebug(BANDCAMP_RESOLVE).noquote() << formatTraceEvent(
        context, QStringLiteral("semantic_conflict"),
        {DiagnosticField::text(QStringLiteral("field"), field),
         DiagnosticField::text(QStringLiteral("album_child_value"), albumChildValue),
         DiagnosticField::text(QStringLiteral("selected_page_value"), selectedPageValue),
         DiagnosticField::text(QStringLiteral("decision"), decision),
         DiagnosticField::text(QStringLiteral("chosen_provenance"), provenanceName(chosen)),
         DiagnosticField::text(QStringLiteral("reason"), reason)});
}

QString valueText(const CandidateField<QString>& field)
{
    if(!field.isUsable()) {
        return field.state == FieldState::Invalid ? QStringLiteral("<invalid>") : QStringLiteral("<unset>");
    }
    return redactDiagnosticText(field.value);
}

QString valueText(const CandidateField<qint64>& field)
{
    if(!field.isUsable()) {
        return field.state == FieldState::Invalid ? QStringLiteral("<invalid>") : QStringLiteral("<unset>");
    }
    return QString::number(field.value);
}

template <typename T>
void logAssemblyChoice(const CorrelationContext& context, const QString& field, const CandidateField<T>& parent,
                       const CandidateField<T>& page, const CandidateField<T>& chosen)
{
    if(!chosen.isUsable() || !BANDCAMP_TRACE_RESOLVE().isDebugEnabled()) {
        return;
    }
    const bool choseParent = parent.isUsable()
                          && (!page.isUsable() || parent.provenance == FieldProvenance::Explicit
                              || page.provenance != FieldProvenance::Explicit);
    const CandidateField<T>& replaced = choseParent ? page : parent;
    const CandidateField<T>& selected = choseParent ? parent : page;
    qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
        context, QStringLiteral("assembly_field"),
        {DiagnosticField::text(QStringLiteral("field"), field),
         DiagnosticField::text(QStringLiteral("old"), valueText(replaced)),
         DiagnosticField::text(QStringLiteral("new"), valueText(selected)),
         DiagnosticField::text(QStringLiteral("provenance"), provenanceName(chosen.provenance)),
         DiagnosticField::text(QStringLiteral("source"),
                               choseParent ? QStringLiteral("album_child") : QStringLiteral("selected_track_page")),
         DiagnosticField::text(QStringLiteral("reason"), choseParent
                                                             ? QStringLiteral("album child is authoritative")
                                                             : QStringLiteral("selected track-page value fills or "
                                                                              "outranks the child"))});
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
    if(differs(parent.value, selectedPage.value, field)) {
        logConflict(context.withTrack(context.trackId), field, valueText(parent), valueText(selectedPage),
                    chooseParent ? QStringLiteral("album_child") : QStringLiteral("selected_track_page"),
                    chooseParent ? parent.provenance : selectedPage.provenance,
                    chooseParent ? QStringLiteral("explicit album child precedes selected track page")
                                 : QStringLiteral("explicit selected-page value precedes "
                                                  "inferred parent value"));
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
    admission.fallbackArtId      = page.fallbackArtId;
    admission.siblings           = siblings;
    admission.completeSiblingSet = completeSiblingSet;
    admission.siblingIndex       = siblingIndex;
    admission.correlation        = context.withTrack(admission.result.track.id);
    return admission;
}

std::optional<ResolutionFailure> validateTrackPage(const TrackPageCandidate& page, const CorrelationContext& context)
{
    if(auto error = validateRequired(page.track.id, QStringLiteral("track.id"), context.resourceUrl)) {
        return error;
    }
    if(page.track.id.value <= 0) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, context.resourceUrl,
                       page.track.id.value, QStringLiteral("track.id"), QStringLiteral("track ID must be positive"));
    }
    return std::nullopt;
}

std::variant<BandcampAlbumMetadata, ResolutionFailure> admitParentMetadata(const AlbumCandidate& parent,
                                                                           const QUrl& declaredUrl,
                                                                           std::optional<qint64> declaredId,
                                                                           const CorrelationContext& context)
{
    std::optional<ResolutionFailure> error;
    CorrelationContext parentContext = context;
    parentContext.resourceUrl        = declaredUrl;
    const auto canonical = canonicalUrl(parent.sourceUrl, BandcampPageKind::Album, QStringLiteral("album.sourceUrl"),
                                        parentContext, error);
    if(!canonical) {
        if(error && context.trackId > 0) {
            error->error.trackId = context.trackId;
        }
        return *error;
    }
    if(parent.tracks.size() > MaxDeclaredTracks) {
        return failure(ResolutionErrorCode::ResourceLimitExceeded, ResolutionStage::Admit, declaredUrl, 0,
                       QStringLiteral("members"), QStringLiteral("declared child limit exceeded"));
    }
    if(auto missing = validateRequired(parent.id, QStringLiteral("album.id"), declaredUrl)) {
        return *missing;
    }
    if(parent.id.value <= 0) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, declaredUrl, 0,
                       QStringLiteral("album.id"), QStringLiteral("album ID must be positive"));
    }
    if(declaredId && parent.id.value != *declaredId) {
        return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, declaredUrl, 0,
                       QStringLiteral("parent.albumId"),
                       QStringLiteral("fetched parent ID does not match the declared album ID"));
    }
    if(auto missing = validateRequired(parent.title, QStringLiteral("album.title"), declaredUrl)) {
        return *missing;
    }
    if(parent.title.value.trimmed().isEmpty()) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, declaredUrl, 0,
                       QStringLiteral("album.title"), QStringLiteral("album title is blank"));
    }
    const BandcampAlbumMetadata metadata = makeAlbumMetadata(parent, *canonical);
    logAlbumComposition(parentContext, parent.sourceUrl.value, *canonical, parent.tracks.size());
    return metadata;
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
    logAssemblyChoice(context, QStringLiteral("track.title"), parentChild.title, page.track.title, result.title);
    logAssemblyChoice(context, QStringLiteral("track.artist"), parentChild.artist, page.track.artist, result.artist);
    logAssemblyChoice(context, QStringLiteral("track.trackNumber"), parentChild.trackNumber, page.track.trackNumber,
                      result.trackNumber);
    logAssemblyChoice(context, QStringLiteral("track.durationMs"), parentChild.durationMs, page.track.durationMs,
                      result.durationMs);
    result.releaseDate = page.track.releaseDate;
    result.artId       = page.track.artId;
    if(result.releaseDate.isUsable() && BANDCAMP_TRACE_RESOLVE().isDebugEnabled()) {
        qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
            context, QStringLiteral("assembly_field"),
            {DiagnosticField::text(QStringLiteral("field"), QStringLiteral("track.releaseDate")),
             DiagnosticField::text(QStringLiteral("old"), QStringLiteral("<unset>")),
             DiagnosticField::text(QStringLiteral("new"), result.releaseDate.value.toString(Qt::ISODate)),
             DiagnosticField::text(QStringLiteral("provenance"), provenanceName(result.releaseDate.provenance)),
             DiagnosticField::text(QStringLiteral("source"), QStringLiteral("track-owned current data")),
             DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("track date remains track-owned"))});
    }
    if(result.artId.isUsable() && BANDCAMP_TRACE_RESOLVE().isDebugEnabled()) {
        qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
            context, QStringLiteral("assembly_field"),
            {DiagnosticField::text(QStringLiteral("field"), QStringLiteral("track.artId")),
             DiagnosticField::text(QStringLiteral("old"), QStringLiteral("<unset>")),
             DiagnosticField::text(QStringLiteral("new"), QString::number(result.artId.value)),
             DiagnosticField::text(QStringLiteral("provenance"), provenanceName(result.artId.provenance)),
             DiagnosticField::text(QStringLiteral("source"), QStringLiteral("track-owned current data")),
             DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("track artwork remains track-owned"))});
    }
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
        std::optional<ResolutionFailure> error;
        const auto url = canonicalUrl(candidate.parent.albumUrl, BandcampPageKind::Album,
                                      QStringLiteral("parent.albumUrl"), context, error, true);
        if(!url) {
            error->error.trackId = candidate.track.id.isUsable() ? candidate.track.id.value : 0;
            return error.value();
        }
        return RequiredAlbumParent{*url, declaredId};
    };

    switch(candidate.parent.declaration) {
        case ParentDeclaration::NoParent:
            if(candidate.parent.albumId.isUsable() && candidate.parent.albumId.value > 0) {
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
            if(!candidate.parent.albumId.isUsable() || candidate.parent.albumId.value <= 0) {
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
    AlbumCandidate normalized = candidate;
    if(!normalizeAlbumOptionals(normalized, stop)) {
        return ResolutionCancelled{};
    }
    context.resourceUrl = context.resourceUrl.isValid() ? context.resourceUrl : normalized.sourceUrl.value;
    std::optional<ResolutionFailure> urlError;
    const auto sourceUrl = canonicalUrl(normalized.sourceUrl, BandcampPageKind::Album,
                                        QStringLiteral("album.sourceUrl"), context, urlError);
    if(!sourceUrl) {
        return *urlError;
    }
    if(auto error = validateRequired(normalized.id, QStringLiteral("album.id"), *sourceUrl)) {
        return *error;
    }
    if(normalized.id.value <= 0) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, *sourceUrl, 0,
                       QStringLiteral("album.id"), QStringLiteral("album ID must be positive"));
    }
    if(auto error = validateRequired(normalized.title, QStringLiteral("album.title"), *sourceUrl)) {
        return *error;
    }
    if(normalized.title.value.trimmed().isEmpty()) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, *sourceUrl, 0,
                       QStringLiteral("album.title"), QStringLiteral("album title is blank"));
    }
    if(normalized.tracks.isEmpty()) {
        return failure(ResolutionErrorCode::NoResolvableTracks, ResolutionStage::Admit, *sourceUrl, 0,
                       QStringLiteral("members"), QStringLiteral("album declares no tracks"));
    }
    if(normalized.tracks.size() > MaxDeclaredTracks) {
        return failure(ResolutionErrorCode::ResourceLimitExceeded, ResolutionStage::Admit, *sourceUrl, 0,
                       QStringLiteral("members"), QStringLiteral("declared child limit exceeded"));
    }

    AlbumAdmission admission;
    admission.album.metadata     = makeAlbumMetadata(normalized, *sourceUrl);
    admission.correlation        = context;
    admission.metadataProvenance = albumProvenance(normalized);
    admission.album.tracks.reserve(normalized.tracks.size());
    admission.trackProvenance.reserve(normalized.tracks.size());

    QSet<qint64> trackIds;
    for(const TrackCandidate& child : normalized.tracks) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const qint64 id = child.id.isUsable() ? child.id.value : 0;
        if(auto error = validateTrackCandidate(child, *sourceUrl, id)) {
            return *error;
        }
        if(trackIds.contains(id)) {
            return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Admit, *sourceUrl, id,
                           QStringLiteral("track.id"),
                           QStringLiteral("duplicate child ID in declared album membership"));
        }
        trackIds.insert(id);
        if(auto error = validateStream(child.streamUrl, *sourceUrl, child.id.value)) {
            return *error;
        }
        admission.album.tracks.append(makeTrack(child, admission.album.metadata.sourceUrl));
        admission.trackProvenance.append(trackProvenance(child));
    }
    logAlbumComposition(context, normalized.sourceUrl.value, *sourceUrl, normalized.tracks.size());
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
    TrackPageCandidate normalized = candidate;
    normalizeTrackPageOptionals(normalized);
    if(auto error = validateTrackPage(normalized, context)) {
        return *error;
    }
    std::optional<ResolutionFailure> urlError;
    const auto sourceUrl = canonicalUrl(normalized.sourceUrl, BandcampPageKind::Track,
                                        QStringLiteral("track.sourceUrl"), context, urlError);
    if(!sourceUrl) {
        urlError->error.trackId = normalized.track.id.value;
        return *urlError;
    }
    if(!normalized.matchingRecord.isUsable()) {
        return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Admit, *sourceUrl,
                       normalized.track.id.value, QStringLiteral("track.streamUrl"),
                       QStringLiteral("standalone track has no unique matching source record"));
    }
    if(auto error = validateTrackCandidate(normalized.track, *sourceUrl, normalized.track.id.value)) {
        return *error;
    }
    if(auto error = validateStream(normalized.track.streamUrl, *sourceUrl, normalized.track.id.value)) {
        return *error;
    }

    return ResolutionSuccess<TrackAdmission>{makeTrackAdmission(normalized, normalized.track, std::nullopt,
                                                                sourceUrl->toString(QUrl::FullyEncoded), {}, {}, false,
                                                                -1, context.withTrack(normalized.track.id.value))};
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
    const auto requiredParent         = std::get<RequiredAlbumParent>(requirement);
    TrackPageCandidate normalizedPage = trackPage;
    normalizeTrackPageOptionals(normalizedPage);
    AlbumCandidate normalizedAlbum = album;
    if(!normalizeAlbumOptionals(normalizedAlbum, stop)) {
        return ResolutionCancelled{};
    }
    const CorrelationContext trackContext
        = context.withTrack(normalizedPage.track.id.isUsable() ? normalizedPage.track.id.value : 0);
    if(auto error = validateTrackPage(normalizedPage, context)) {
        return *error;
    }
    const auto parentOutcome
        = admitParentMetadata(normalizedAlbum, requiredParent.url, requiredParent.albumId, trackContext);
    if(const auto* parentError = std::get_if<ResolutionFailure>(&parentOutcome)) {
        ResolutionFailure failure = *parentError;
        failure.error.trackId     = normalizedPage.track.id.value;
        return failure;
    }
    BandcampAlbumMetadata parentMetadata = std::get<BandcampAlbumMetadata>(parentOutcome);

    qsizetype matchingIndex = -1;
    int matchingCount       = 0;
    for(qsizetype i = 0; i < normalizedAlbum.tracks.size(); ++i) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const auto& child = normalizedAlbum.tracks.at(i);
        if(child.id.isUsable() && child.id.value == normalizedPage.track.id.value) {
            matchingIndex = i;
            ++matchingCount;
        }
    }
    if(matchingCount != 1) {
        return failure(ResolutionErrorCode::RelationshipUnresolved, ResolutionStage::Assemble, requiredParent.url,
                       normalizedPage.track.id.value, QStringLiteral("parent.membership"),
                       matchingCount == 0 ? QStringLiteral("selected track is not a declared album child")
                                          : QStringLiteral("selected child ID is not unique in the album"));
    }

    const TrackCandidate& child = normalizedAlbum.tracks.at(matchingIndex);
    if(auto error = validateTrackRequired(child, requiredParent.url, normalizedPage.track.id.value, false)) {
        return *error;
    }
    if(auto error = validateStream(child.streamUrl, requiredParent.url, normalizedPage.track.id.value)) {
        return *error;
    }

    TrackCandidate selected = selectedTrack(child, normalizedPage, trackContext);
    if(BANDCAMP_TRACE_RESOLVE().isDebugEnabled()) {
        qCDebug(BANDCAMP_TRACE_RESOLVE).noquote() << formatTraceEvent(
            trackContext, QStringLiteral("assembly_field"),
            {DiagnosticField::text(QStringLiteral("field"), QStringLiteral("track.streamUrl")),
             DiagnosticField::text(QStringLiteral("old"), normalizedPage.track.streamUrl.isUsable()
                                                              ? QStringLiteral("<signed locator omitted>")
                                                              : QStringLiteral("<unset>")),
             DiagnosticField::text(QStringLiteral("new"), QStringLiteral("<signed locator omitted>")),
             DiagnosticField::text(QStringLiteral("provenance"), provenanceName(child.streamUrl.provenance)),
             DiagnosticField::text(QStringLiteral("source"), QStringLiteral("album_child")),
             DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("album child stream is authoritative"))});
    }
    if(child.streamUrl.isUsable() && normalizedPage.track.streamUrl.isUsable()
       && child.streamUrl.value != normalizedPage.track.streamUrl.value) {
        logConflict(trackContext, QStringLiteral("track.streamUrl"), valueText(child.streamUrl),
                    valueText(normalizedPage.track.streamUrl), QStringLiteral("album_child"),
                    child.streamUrl.provenance, QStringLiteral("album child stream is authoritative"));
    }
    if(selected.title.state == FieldState::Invalid) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, requiredParent.url,
                       normalizedPage.track.id.value, QStringLiteral("track.title"),
                       QStringLiteral("selected track title is invalid on both pages"));
    }
    if(!selected.title.isUsable()) {
        return failure(ResolutionErrorCode::RequiredFieldMissing, ResolutionStage::Admit, requiredParent.url,
                       normalizedPage.track.id.value, QStringLiteral("track.title"),
                       QStringLiteral("selected track has no usable title on either page"));
    }
    if(selected.title.value.trimmed().isEmpty()) {
        return failure(ResolutionErrorCode::InvalidField, ResolutionStage::Admit, requiredParent.url,
                       normalizedPage.track.id.value, QStringLiteral("track.title"),
                       QStringLiteral("selected track title is blank"));
    }

    QList<BandcampTrack> siblings;
    bool complete = true;
    QSet<qint64> siblingIds;
    for(qsizetype i = 0; i < normalizedAlbum.tracks.size(); ++i) {
        if(stop && stop->load(std::memory_order_relaxed)) {
            return ResolutionCancelled{};
        }
        const TrackCandidate& member = normalizedAlbum.tracks.at(i);
        TrackCandidate resolved      = i == matchingIndex ? selected : member;
        const qint64 siblingId       = resolved.id.isUsable() ? resolved.id.value : 0;
        if(siblingId <= 0 || siblingIds.contains(siblingId)
           || validateTrackCandidate(resolved, requiredParent.url, siblingId)
           || validateStream(resolved.streamUrl, requiredParent.url, siblingId)) {
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
        = makeTrackAdmission(normalizedPage, selected, parentMetadata, parentMetadata.sourceUrl,
                             albumProvenance(normalizedAlbum), siblings, complete, matchingIndex, trackContext);
    return ResolutionSuccess<TrackAdmission>{std::move(admission)};
}
} // namespace Fooyin::Bandcamp
