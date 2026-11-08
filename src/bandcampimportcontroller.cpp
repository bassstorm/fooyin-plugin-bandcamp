#include "bandcampimportcontroller.h"

#include "bandcamplogging.h"

#include <QThread>
#include <QTimer>

#include <memory>
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
    m_progress.operationId     = operationId;
    const bool ownersAvailable = playlistOwnersAvailable();
    if(!ownersAvailable || !m_playlistApi.inspect || !m_playlistApi.createNew || !m_playlistApi.insert) {
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
    configurePipeline();
    connect(&m_pipeline, &BandcampImportPipeline::progressChanged, this, &BandcampImportController::onPipelineProgress);
    connect(&m_pipeline, &BandcampImportPipeline::jobFinished, this, &BandcampImportController::onPipelineJobFinished);
    connect(&m_pipeline, &BandcampImportPipeline::becameIdle, this, &BandcampImportController::finishIfReady);
}

BandcampImportController::~BandcampImportController()
{
    const bool withdrawResolution = !m_resolutionFinished && !m_resolutionCancellationIssued;
    m_finished                    = true;
    m_stopping                    = true;
    m_pendingScopes.clear();
    m_pipeline.cancel();
    if(withdrawResolution) {
        requestResolutionCancellation();
    }
}

void BandcampImportController::configurePipeline()
{
    if(m_probeStreams) {
        m_pipeline.setProbeHandler(
            [this](const ImportJob& job, BandcampImportPipeline::ProbeCompletion completion) {
                m_pendingProbeRetries.insert(job.id, PendingProbeRetry{job, std::move(completion)});
                submitProbe(job.id);
            },
            [this](const ImportJob& job) { cancelProbe(job.id); });
    }

    m_pipeline.setCommitHandler(
        [this](const ImportJob& job, BandcampImportPipeline::CommitCompletion completion) {
            if(!m_gateway) {
                completion(FooyinTrackCommitFailure{FooyinTrackCommitErrorCode::LibraryUnavailable, 0,
                                                    QStringLiteral("library gateway unavailable")});
                return;
            }
            CorrelationContext correlation{.resourceUrl = QUrl(job.scope.page.url)};
            if(const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&job.scope.outcome)) {
                if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
                    correlation.albumId = album->metadata.id;
                }
                else {
                    const auto& track   = std::get<BandcampResolvedTrack>(success->value);
                    correlation.albumId = track.album ? track.album->id : 0;
                    correlation.trackId = track.track.id;
                }
            }

            const QPointer<BandcampImportController> guard{this};
            const quint64 jobId  = job.id;
            auto submissionState = std::make_shared<std::pair<quint64, bool>>(0, false);
            const quint64 token  = m_gateway->commitImport(
                job.importTracks, this, correlation,
                [guard, jobId, completion, submissionState](FooyinTrackCommitOutcome outcome) mutable {
                    submissionState->second = true;
                    if(guard) {
                        if(submissionState->first != 0) {
                            guard->m_gatewayTokens.remove(jobId);
                        }
                        completion(std::move(outcome));
                    }
                });
            submissionState->first = token;
            if(token != 0 && !submissionState->second) {
                m_gatewayTokens.insert(jobId, token);
                if(m_stopping) {
                    cancelGatewayRequest(jobId);
                }
            }
        },
        [this](const ImportJob& job) { cancelGatewayRequest(job.id); });

    m_pipeline.setInsertHandler([this](const ImportJob& job) {
        if(m_stopping || m_finished) {
            return false;
        }
        Fooyin::UId destination;
        if(!prepareDestination(job.scope, destination)) {
            return false;
        }
        if(m_stopping || m_finished) {
            return false;
        }
        const auto insert = m_playlistApi.insert;
        insert(destination, job.committedTracks);
        return true;
    });
}

void BandcampImportController::submitProbe(quint64 jobId)
{
    const auto pending = m_pendingProbeRetries.constFind(jobId);
    if(pending == m_pendingProbeRetries.cend() || m_finished || m_stopping) {
        return;
    }

    const ImportJob job   = pending->job;
    const auto completion = pending->completion;
    if(!m_streamProbe) {
        QList<BandcampStreamProbeResult> results;
        results.reserve(job.tracks.size());
        for(const BandcampResolvedTrack& track : job.tracks) {
            qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(
                CorrelationContext{.trackId = track.track.id}, QStringLiteral("stream_probe_failed"),
                {DiagnosticField::text(QStringLiteral("reason"), QStringLiteral("remote_unavailable"))});
            results.append({track.track.id, {}, BandcampStreamProbeFailure::RemoteUnavailable});
        }
        m_pendingProbeRetries.remove(jobId);
        completion(std::move(results));
        return;
    }

    const QPointer<BandcampImportController> guard{this};
    const BandcampStreamProbeSubmission submission = m_streamProbe->probe(
        job.tracks, this, [guard, jobId, completion](QList<BandcampStreamProbeResult> results) mutable {
            if(!guard) {
                return;
            }
            guard->m_probeTokens.remove(jobId);
            guard->m_pendingProbeRetries.remove(jobId);
            completion(std::move(results));
        });
    if(submission.token != 0) {
        m_pendingProbeRetries.remove(jobId);
        m_probeTokens.insert(jobId, submission.token);
        return;
    }

    if(submission.rejection == BandcampStreamProbeFailure::CapacityRejected) {
        QTimer::singleShot(50, this, [guard, jobId]() {
            if(guard) {
                guard->submitProbe(jobId);
            }
        });
        return;
    }

    QList<BandcampStreamProbeResult> results;
    results.reserve(job.tracks.size());
    const auto failure = submission.rejection.value_or(BandcampStreamProbeFailure::Cancelled);
    for(const BandcampResolvedTrack& track : job.tracks) {
        results.append({track.track.id, {}, failure});
    }
    m_pendingProbeRetries.remove(jobId);
    completion(std::move(results));
}

void BandcampImportController::start()
{
    if(m_started || m_finished) {
        return;
    }
    m_started = true;
    if(m_initialFailure) {
        failRun(m_stopReason);
        return;
    }
    const QPointer<BandcampImportController> guard{this};
    while(!m_pendingScopes.isEmpty()) {
        enqueueScope(m_pendingScopes.dequeue());
        if(!guard) {
            return;
        }
    }
}

void BandcampImportController::cancel()
{
    if(m_finished || m_stopping) {
        return;
    }
    const QPointer<BandcampImportController> guard{this};
    m_stopping = true;
    m_pipeline.cancel();
    if(!guard) {
        return;
    }
    while(!m_pendingScopes.isEmpty()) {
        enqueueScope(m_pendingScopes.dequeue());
        if(!guard) {
            return;
        }
    }
    requestResolutionCancellation();
    if(guard) {
        finishIfReady();
    }
}

void BandcampImportController::acceptScopeResult(const BandcampScopeResult& result)
{
    if(result.operationId != m_operationId || m_finished) {
        return;
    }
    if(!m_started) {
        m_pendingScopes.enqueue(result);
        return;
    }
    enqueueScope(result);
}

void BandcampImportController::acceptProgress(const BandcampOperationProgress& progress)
{
    if(progress.operationId != m_operationId || m_finished) {
        return;
    }
    m_progress  = progress;
    m_totalWork = progress.totalWork;
    const QPointer<BandcampImportController> guard{this};
    m_pipeline.setTotalWork(m_totalWork);
    if(guard) {
        publishImportProgress();
    }
}

void BandcampImportController::finishResolution(const BandcampOperationSummary& summary)
{
    if(summary.operationId != m_operationId || m_resolutionFinished || m_finished) {
        return;
    }
    m_resolutionFinished = true;
    const QPointer<BandcampImportController> guard{this};
    publishImportProgress();
    if(guard) {
        finishIfReady();
    }
}

void BandcampImportController::enqueueScope(const BandcampScopeResult& scope)
{
    ImportJob job{scope};
    if(const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&scope.outcome)) {
        if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
            job.tracks.reserve(album->tracks.size());
            for(const BandcampTrack& track : album->tracks) {
                job.tracks.append({track, album->metadata});
            }
        }
        else {
            job.tracks.append(std::get<BandcampResolvedTrack>(success->value));
        }

        if(!m_stopping) {
            const QPointer<BandcampImportController> guard{this};
            if(QThread::currentThread() != thread() || !playlistOwnersAvailable()) {
                job.admissionFailure = tr("The playlist service is unavailable.");
            }
            else if(!m_pendingNewPlaylist && !destinationWritable(m_destinationId)) {
                job.admissionFailure = tr("The selected playlist is no longer available or writable.");
            }
            if(!guard) {
                return;
            }
        }
    }
    m_pipeline.enqueue(std::move(job));
}

void BandcampImportController::onPipelineProgress(qsizetype completedWork, qsizetype totalWork)
{
    m_pipelineCompletedWork = completedWork;
    m_totalWork             = totalWork;
    publishImportProgress();
}

void BandcampImportController::onPipelineJobFinished(const ImportJob& job)
{
    if(m_finished) {
        return;
    }
    QString fatalReason;
    bool logFatalReason = true;
    switch(job.stage) {
        case ImportJobStage::Completed:
            ++m_added;
            break;
        case ImportJobStage::Failed:
            ++m_failed;
            if(job.admissionFailure) {
                fatalReason = *job.admissionFailure;
            }
            else if(!m_pendingFatalReason.isEmpty()) {
                fatalReason = std::exchange(m_pendingFatalReason, {});
            }
            else if(job.commitFailure && isFatalGatewayFailure(*job.commitFailure)) {
                fatalReason = tr("The library service is unavailable.");
            }
            logFatalReason = !job.commitFailure.has_value();
            break;
        case ImportJobStage::Cancelled:
            ++m_cancelled;
            break;
        case ImportJobStage::Queued:
        case ImportJobStage::Probing:
        case ImportJobStage::Probed:
        case ImportJobStage::Committing:
        case ImportJobStage::Committed:
            return;
    }

    m_committedCount = m_added + m_failed + m_cancelled;
    const QPointer<BandcampImportController> guard{this};
    publishImportProgress();
    if(!guard || fatalReason.isEmpty()) {
        return;
    }
    failRun(std::move(fatalReason), logFatalReason);
}

void BandcampImportController::publishImportProgress()
{
    BandcampImportProgress progress;
    progress.operationId = m_operationId;
    progress.requested   = m_progress.requested;
    progress.delivered   = m_progress.delivered;
    progress.active      = m_progress.active;
    progress.totalWork   = m_totalWork;
    if(m_progress.phase == BandcampOperationPhase::Discovering) {
        progress.phase         = BandcampImportPhase::Discovering;
        progress.completedWork = m_progress.completedWork;
    }
    else if(!m_resolutionFinished) {
        progress.phase         = BandcampImportPhase::Resolving;
        progress.completedWork = m_progress.completedWork;
    }
    else if(m_probeStreams && m_pipelineCompletedWork < m_totalWork) {
        progress.phase         = BandcampImportPhase::Probing;
        progress.completedWork = m_pipelineCompletedWork;
    }
    else {
        progress.phase         = BandcampImportPhase::Committing;
        progress.completedWork = qMin(m_committedCount, m_totalWork);
    }
    emit progressChanged(progress);
}

void BandcampImportController::failRun(QString reason, bool logFailure)
{
    if(m_finished || m_stopping) {
        return;
    }
    const QPointer<BandcampImportController> guard{this};
    m_stopping   = true;
    m_stopReason = std::move(reason);
    m_pipeline.cancel();
    if(!guard) {
        return;
    }
    while(!m_pendingScopes.isEmpty()) {
        enqueueScope(m_pendingScopes.dequeue());
        if(!guard) {
            return;
        }
    }
    const QString stopReason = m_stopReason;
    if(logFailure) {
        qCWarning(BANDCAMP_UI).noquote() << formatTraceEvent(
            CorrelationContext{}, QStringLiteral("import_stopped"),
            {DiagnosticField::text(QStringLiteral("reason"), stopReason)});
    }
    emit integrationError(stopReason);
    if(!guard) {
        return;
    }
    requestResolutionCancellation();
    if(guard) {
        finishIfReady();
    }
}

void BandcampImportController::requestResolutionCancellation()
{
    if(m_resolutionCancellationIssued) {
        return;
    }
    m_resolutionCancellationIssued        = true;
    const CancelOperation cancelOperation = m_cancelOperation;
    if(cancelOperation) {
        cancelOperation(m_operationId);
    }
}

void BandcampImportController::cancelProbe(quint64 jobId)
{
    if(m_pendingProbeRetries.remove(jobId) > 0) {
        return;
    }
    const quint64 token = m_probeTokens.take(jobId);
    if(m_streamProbe && token != 0) {
        m_streamProbe->cancel(token);
    }
}

void BandcampImportController::cancelGatewayRequest(quint64 jobId)
{
    const quint64 token = m_gatewayTokens.take(jobId);
    if(!m_gateway) {
        return;
    }
    if(m_ownsGateway) {
        m_gateway->cancel();
    }
    else if(token != 0) {
        m_gateway->cancelRequest(token);
    }
}

void BandcampImportController::finishIfReady()
{
    if(m_finished || (m_stopping && !m_resolutionCancellationIssued) || !m_resolutionFinished || !m_pipeline.isIdle()
       || !m_pendingScopes.isEmpty()) {
        return;
    }
    m_finished = true;
    const BandcampImportSummary summary{m_operationId, m_added,      m_failed,
                                        m_cancelled,   m_stopReason, m_createdPlaylistId};
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
        m_pendingFatalReason = tr("The playlist service is unavailable.");
        return false;
    }
    if(m_pendingNewPlaylist) {
        const auto createNew = m_playlistApi.createNew;
        const QString name   = newPlaylistName(std::get<ResolutionSuccess<BandcampResolvedData>>(scope.outcome).value);
        const std::optional<Fooyin::UId> created = createNew(name);
        if(!guard || m_stopping || m_finished) {
            return false;
        }
        if(!created || !created->isValid()) {
            m_pendingFatalReason = tr("A writable playlist could not be created.");
            return false;
        }
        m_destinationId      = *created;
        m_createdPlaylistId  = *created;
        m_pendingNewPlaylist = false;
    }
    const bool writable = destinationWritable(m_destinationId);
    if(!guard) {
        return false;
    }
    if(!writable) {
        m_pendingFatalReason = tr("The selected playlist is no longer available or writable.");
        return false;
    }
    id = m_destinationId;
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
