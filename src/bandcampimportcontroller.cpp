#include "bandcampimportcontroller.h"

#include "bandcamplogging.h"

#include <QHash>
#include <QThread>

#include <utility>

namespace Fooyin::Bandcamp {
BandcampImportController::BandcampImportController(BandcampOperationId operationId,
                                                   QList<BandcampPageUrl> submittedPages, bool forceNewPlaylist,
                                                   Fooyin::UId selectedPlaylistId, FooyinLibraryApi libraryApi,
                                                   BandcampPlaylistApi playlistApi, CancelOperation cancelOperation,
                                                   BandcampStreamProbe* streamProbe, bool probeStreams, QObject* parent,
                                                   BandcampLibraryGateway* sharedGateway)
    : QObject{parent}
    , m_operationId{operationId}
    , m_submittedPages{std::move(submittedPages)}
    , m_destinationId{std::move(selectedPlaylistId)}
    , m_libraryOwner{libraryApi.owner}
    , m_playlistApi{std::move(playlistApi)}
    , m_cancelOperation{std::move(cancelOperation)}
    , m_gateway{sharedGateway ? sharedGateway : new BandcampLibraryGateway{std::move(libraryApi), this}}
    , m_ownsGateway{!sharedGateway}
    , m_streamProbe{streamProbe}
    , m_probeStreams{probeStreams}
{
    const bool ownersAvailable = playlistOwnersAvailable();
    if(m_initialFailure || !ownersAvailable || !m_playlistApi.inspect || !m_playlistApi.createNew
       || !m_playlistApi.insert) {
        m_initialFailure = true;
        m_stopReason     = tr("The playlist service is unavailable.");
    }
    else if(forceNewPlaylist || !m_destinationId.isValid()) {
        m_pendingNewPlaylist = true;
    }
    else {
        switch(m_playlistApi.inspect(m_destinationId)) {
            case BandcampPlaylistAvailability::Writable:
                break;
            case BandcampPlaylistAvailability::OwnerUnavailable:
                m_initialFailure = true;
                m_stopReason     = tr("The playlist service is unavailable.");
                break;
            case BandcampPlaylistAvailability::Missing:
            case BandcampPlaylistAvailability::Temporary:
            case BandcampPlaylistAvailability::Automatic:
            case BandcampPlaylistAvailability::Locked:
                m_pendingNewPlaylist = true;
                break;
        }
    }
}

BandcampImportController::~BandcampImportController()
{
    const bool withdrawResolution = !m_resolutionFinished && !m_resolutionCancellationIssued;
    m_finished                    = true;
    m_stopping                    = true;
    m_pendingScopes.clear();
    m_handoffPhase = HandoffPhase::None;
    if(m_streamProbe && m_probeToken != 0) {
        m_streamProbe->cancel(m_probeToken);
    }
    cancelGatewayRequest();
    if(withdrawResolution) {
        requestResolutionCancellation();
    }
}

void BandcampImportController::start()
{
    if(m_started || m_finished) {
        return;
    }
    m_started = true;
    if(m_initialFailure) {
        failRun(m_stopReason, false);
    }
}

void BandcampImportController::cancel()
{
    if(m_finished || m_stopping) {
        return;
    }

    const QPointer<BandcampImportController> guard{this};
    m_stopping = true;
    clearPendingAsCancelled();
    switch(m_handoffPhase) {
        case HandoffPhase::Probing:
        case HandoffPhase::Gateway:
            ++m_cancelled;
            m_handoffPhase = HandoffPhase::None;
            break;
        case HandoffPhase::PreparingDestination:
            ++m_cancelled;
            break;
        case HandoffPhase::None:
        case HandoffPhase::Inserting:
            break;
    }
    const quint64 probeToken = std::exchange(m_probeToken, 0);
    if(m_streamProbe && probeToken != 0) {
        m_streamProbe->cancel(probeToken);
    }
    cancelGatewayRequest();
    if(!guard) {
        return;
    }
    guard->requestResolutionCancellation();
    if(guard) {
        guard->finishIfReady();
    }
}

void BandcampImportController::acceptScopeResult(const BandcampScopeResult& result)
{
    if(result.operationId != m_operationId || m_finished) {
        return;
    }
    if(m_stopping) {
        ++m_cancelled;
        return;
    }

    m_pendingScopes.enqueue(result);
    pump();
}

void BandcampImportController::acceptProgress(const BandcampOperationProgress& progress)
{
    if(progress.operationId == m_operationId && !m_finished && !m_stopping) {
        emit progressChanged(progress);
    }
}

void BandcampImportController::finishResolution(const BandcampOperationSummary& summary)
{
    if(summary.operationId != m_operationId || m_resolutionFinished || m_finished) {
        return;
    }
    m_resolutionFinished = true;
    if((m_handoffPhase != HandoffPhase::None || !m_pendingScopes.isEmpty()) && !m_importingEmitted) {
        m_importingEmitted = true;
        const QPointer<BandcampImportController> guard{this};
        emit importing();
        if(!guard) {
            return;
        }
    }
    finishIfReady();
}

void BandcampImportController::pump()
{
    if(!m_started || m_finished || m_stopping || m_handoffPhase != HandoffPhase::None) {
        return;
    }

    while(!m_pendingScopes.isEmpty()) {
        const BandcampScopeResult scope = m_pendingScopes.dequeue();
        if(std::holds_alternative<ResolutionFailure>(scope.outcome)) {
            ++m_failed;
            continue;
        }
        if(std::holds_alternative<ResolutionCancelled>(scope.outcome)) {
            ++m_cancelled;
            continue;
        }

        const QPointer<BandcampImportController> guard{this};
        const bool available = playlistOwnersAvailable();
        if(!guard) {
            return;
        }
        if(QThread::currentThread() != thread() || !available) {
            guard->failRun(tr("The playlist service is unavailable."), true);
            return;
        }
        if(!guard->m_pendingNewPlaylist) {
            const bool writable = guard->destinationWritable(guard->m_destinationId);
            if(!guard) {
                return;
            }
            if(!writable) {
                guard->failRun(tr("The selected playlist is no longer available or writable."), true);
                return;
            }
        }

        const BandcampResolvedData& data = std::get<ResolutionSuccess<BandcampResolvedData>>(scope.outcome).value;
        QList<BandcampResolvedTrack> tracks;
        if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
            tracks.reserve(album->tracks.size());
            for(const BandcampTrack& track : album->tracks) {
                tracks.append({track, album->metadata});
            }
        }
        else {
            const auto& resolvedTrack = std::get<BandcampResolvedTrack>(data);
            tracks.append(resolvedTrack);
        }

        if(guard->m_probeStreams) {
            const QList<BandcampResolvedTrack> fallbackTracks = tracks;
            guard->m_handoffPhase                             = HandoffPhase::Probing;
            if(!guard->m_streamProbe) {
                QList<BandcampStreamProbeResult> results;
                results.reserve(tracks.size());
                for(const BandcampResolvedTrack& track : tracks) {
                    qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(
                        CorrelationContext{.trackId = track.track.id}, QStringLiteral("stream_probe_failed"),
                        {DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("remote_unavailable"))});
                    results.append({track.track.id, {}, BandcampStreamProbeFailure::RemoteUnavailable});
                }
                guard->onProbe(scope, fallbackTracks, std::move(results));
                return;
            }
            const auto submission = guard->m_streamProbe->probe(
                tracks, guard, [guard, scope, tracks](QList<BandcampStreamProbeResult> results) mutable {
                    if(guard) {
                        guard->onProbe(scope, std::move(tracks), std::move(results));
                    }
                });
            guard->m_probeToken = submission.token;
            if(submission.token == 0) {
                QList<BandcampStreamProbeResult> results;
                results.reserve(fallbackTracks.size());
                const auto failure = submission.rejection.value_or(BandcampStreamProbeFailure::Cancelled);
                for(const BandcampResolvedTrack& track : fallbackTracks) {
                    results.append({track.track.id, {}, failure});
                }
                guard->onProbe(scope, fallbackTracks, std::move(results));
            }
        }
        else {
            QList<BandcampImportTrack> imported;
            imported.reserve(tracks.size());
            for(BandcampResolvedTrack& track : tracks) {
                imported.append({std::move(track), {}});
            }
            guard->commitScope(scope, std::move(imported));
        }
        return;
    }

    finishIfReady();
}

void BandcampImportController::onProbe(const BandcampScopeResult& scope, QList<BandcampResolvedTrack> tracks,
                                       QList<BandcampStreamProbeResult> results)
{
    if(m_finished || m_stopping || m_handoffPhase != HandoffPhase::Probing) {
        return;
    }
    m_probeToken = 0;
    QHash<qint64, BandcampStreamProbeResult> resultsByTrack;
    resultsByTrack.reserve(results.size());
    for(BandcampStreamProbeResult& result : results) {
        resultsByTrack.insert(result.trackId, std::move(result));
    }
    QList<BandcampImportTrack> imported;
    imported.reserve(tracks.size());
    for(BandcampResolvedTrack& track : tracks) {
        BandcampStreamProperties observed;
        const auto result = resultsByTrack.find(track.track.id);
        if(result == resultsByTrack.end()) {
            qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(CorrelationContext{.trackId = track.track.id},
                                                                     QStringLiteral("stream_probe_result_missing"), {});
        }
        else if(!result->failure) {
            observed = std::move(result->properties);
        }
        imported.append({std::move(track), std::move(observed)});
    }
    commitScope(scope, std::move(imported));
}

void BandcampImportController::commitScope(const BandcampScopeResult& scope, QList<BandcampImportTrack> tracks)
{
    if(m_finished || m_stopping || (m_handoffPhase != HandoffPhase::None && m_handoffPhase != HandoffPhase::Probing)) {
        return;
    }
    const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&scope.outcome);
    if(!success) {
        m_handoffPhase = HandoffPhase::None;
        return;
    }
    CorrelationContext correlation{.resourceUrl = QUrl(scope.page.url)};
    if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
        correlation.albumId = album->metadata.id;
    }
    else {
        const auto& track   = std::get<BandcampResolvedTrack>(success->value);
        correlation.albumId = track.album ? track.album->id : 0;
        correlation.trackId = track.track.id;
    }
    m_handoffPhase = HandoffPhase::Gateway;
    const QPointer<BandcampImportController> guard{this};
    m_gatewayRequest = m_gateway->commitImport(std::move(tracks), this, correlation,
                                               [guard, scope](FooyinTrackCommitOutcome outcome) mutable {
                                                   if(guard) {
                                                       guard->onCommit(scope, std::move(outcome));
                                                   }
                                               });
}

void BandcampImportController::onCommit(const BandcampScopeResult& scope, FooyinTrackCommitOutcome outcome)
{
    if(m_finished || m_handoffPhase != HandoffPhase::Gateway) {
        return;
    }
    m_gatewayRequest = 0;
    m_handoffPhase   = HandoffPhase::PreparingDestination;
    if(m_stopping) {
        m_handoffPhase = HandoffPhase::None;
        finishIfReady();
        return;
    }

    if(const auto* failure = std::get_if<FooyinTrackCommitFailure>(&outcome)) {
        m_handoffPhase = HandoffPhase::None;
        if(isFatalGatewayFailure(*failure)) {
            failRun(tr("The library service is unavailable."), true, false);
            return;
        }
        ++m_failed;
        pump();
        return;
    }

    const FooyinTrackCommitSuccess& prepared = std::get<FooyinTrackCommitSuccess>(outcome);
    Fooyin::UId destination;
    const QPointer<BandcampImportController> guard{this};
    const bool destinationPrepared = guard->prepareDestination(scope, destination);
    if(!guard) {
        return;
    }
    if(guard->m_stopping || guard->m_finished) {
        guard->m_handoffPhase = HandoffPhase::None;
        guard->finishIfReady();
        return;
    }
    if(!destinationPrepared) {
        guard->m_handoffPhase = HandoffPhase::None;
        guard->failRun(tr("A writable playlist could not be created."), true);
        return;
    }

    const auto insert     = guard->m_playlistApi.insert;
    guard->m_handoffPhase = HandoffPhase::Inserting;
    insert(destination, prepared.tracks);
    if(!guard) {
        return;
    }

    ++guard->m_added;
    guard->m_handoffPhase = HandoffPhase::None;
    if(guard->m_stopping) {
        guard->finishIfReady();
    }
    else {
        guard->pump();
    }
}

void BandcampImportController::failRun(QString reason, bool countCurrentScope, bool logFailure)
{
    if(m_finished || m_stopping) {
        return;
    }
    if(countCurrentScope) {
        ++m_failed;
    }
    m_stopping   = true;
    m_stopReason = std::move(reason);
    clearPendingAsCancelled();
    const QString stopReason = m_stopReason;
    if(logFailure) {
        qCWarning(BANDCAMP_UI).noquote() << formatTraceEvent(
            CorrelationContext{}, QStringLiteral("import_stopped"),
            {DiagnosticField::text(QStringLiteral("reason"), stopReason)});
    }
    const QPointer<BandcampImportController> guard{this};
    cancelGatewayRequest();
    if(!guard) {
        return;
    }
    emit integrationError(stopReason);
    if(!guard) {
        return;
    }
    guard->requestResolutionCancellation();
    if(guard) {
        guard->finishIfReady();
    }
}

void BandcampImportController::requestResolutionCancellation()
{
    if(m_resolutionCancellationIssued) {
        return;
    }

    m_resolutionCancellationIssued        = true;
    const BandcampOperationId operationId = m_operationId;
    const CancelOperation cancelOperation = m_cancelOperation;
    if(!cancelOperation) {
        return;
    }
    cancelOperation(operationId);
}

void BandcampImportController::cancelGatewayRequest()
{
    const quint64 token = std::exchange(m_gatewayRequest, 0);
    if(!m_gateway) {
        return;
    }
    if(m_ownsGateway) {
        m_gateway->cancel();
    }
    else {
        m_gateway->cancelRequest(token);
    }
}

void BandcampImportController::clearPendingAsCancelled()
{
    m_cancelled += m_pendingScopes.size();
    m_pendingScopes.clear();
}

void BandcampImportController::finishIfReady()
{
    if(m_finished || !m_resolutionFinished || m_handoffPhase != HandoffPhase::None || !m_pendingScopes.isEmpty()) {
        return;
    }
    m_finished = true;
    const BandcampImportSummary summary{m_operationId, m_added, m_failed, m_cancelled, m_stopReason};
    qCInfo(BANDCAMP_UI).noquote() << formatTraceEvent(
        CorrelationContext{}, QStringLiteral("import_completed"),
        {DiagnosticField::number(QStringLiteral("succeeded"), summary.added),
         DiagnosticField::number(QStringLiteral("failed"), summary.failed),
         DiagnosticField::number(QStringLiteral("cancelled"), summary.cancelled)});
    emit finished(summary);
}

bool BandcampImportController::playlistOwnersAvailable() const
{
    return m_playlistApi.owner && m_playlistApi.ownersAvailable && m_playlistApi.ownersAvailable();
}

bool BandcampImportController::destinationWritable(const Fooyin::UId& id) const
{
    if(!id.isValid() || !m_playlistApi.inspect || !playlistOwnersAvailable()) {
        return false;
    }
    return m_playlistApi.inspect(id) == BandcampPlaylistAvailability::Writable;
}

bool BandcampImportController::prepareDestination(const BandcampScopeResult& scope, Fooyin::UId& id)
{
    const QPointer<BandcampImportController> guard{this};
    const bool available = playlistOwnersAvailable();
    if(!guard) {
        return false;
    }
    if(!available || QThread::currentThread() != thread()) {
        guard->failRun(tr("The playlist service is unavailable."), true);
        return false;
    }
    if(guard->m_pendingNewPlaylist) {
        const auto createNew = guard->m_playlistApi.createNew;
        const QString name
            = guard->newPlaylistName(std::get<ResolutionSuccess<BandcampResolvedData>>(scope.outcome).value);
        const std::optional<Fooyin::UId> created = createNew(name);
        if(!guard) {
            return false;
        }
        if(guard->m_stopping || guard->m_finished) {
            return false;
        }
        if(!created || !created->isValid()) {
            guard->failRun(tr("A writable playlist could not be created."), true);
            return false;
        }
        guard->m_destinationId      = *created;
        guard->m_pendingNewPlaylist = false;
    }
    const bool writable = guard->destinationWritable(guard->m_destinationId);
    if(!guard) {
        return false;
    }
    if(!writable) {
        guard->failRun(tr("The selected playlist is no longer available or writable."), true);
        return false;
    }
    id = guard->m_destinationId;
    return true;
}

QString BandcampImportController::newPlaylistName(const BandcampResolvedData& data) const
{
    if(m_submittedPages.size() != 1 || m_submittedPages.front().kind == BandcampPageKind::Discography) {
        return tr("Bandcamp");
    }
    if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
        return album->metadata.title;
    }
    const BandcampResolvedTrack& track = std::get<BandcampResolvedTrack>(data);
    return track.album ? track.album->title : track.track.title;
}

bool BandcampImportController::isFatalGatewayFailure(const FooyinTrackCommitFailure& failure) const
{
    if(failure.code == FooyinTrackCommitErrorCode::LibraryUnavailable
       || failure.code == FooyinTrackCommitErrorCode::WrongThread) {
        return true;
    }
    return failure.code == FooyinTrackCommitErrorCode::HostOperationFailed && !m_libraryOwner;
}
} // namespace Fooyin::Bandcamp
