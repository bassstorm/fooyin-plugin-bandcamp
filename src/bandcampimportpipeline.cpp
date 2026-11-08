#include "bandcampimportpipeline.h"

#include "bandcamplogging.h"
#include "bandcampurl.h"

#include <QHash>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace Fooyin::Bandcamp {
BandcampImportPipeline::BandcampImportPipeline(QObject* parent)
    : QObject{parent}
{ }

void BandcampImportPipeline::setProbeHandler(ProbeHandler handler, CancelHandler cancelHandler)
{
    m_probeHandler = std::move(handler);
    m_cancelProbe  = std::move(cancelHandler);
}

void BandcampImportPipeline::setCommitHandler(CommitHandler handler, CancelHandler cancelHandler)
{
    m_commitHandler = std::move(handler);
    m_cancelCommit  = std::move(cancelHandler);
}

void BandcampImportPipeline::setInsertHandler(InsertHandler handler)
{
    m_insertHandler = std::move(handler);
}

void BandcampImportPipeline::setTotalWork(qsizetype totalWork)
{
    const qsizetype boundedTotal = qMax<qsizetype>(0, totalWork);
    if(m_totalWork == boundedTotal) {
        return;
    }
    m_totalWork = boundedTotal;
    publishProgress();
}

void BandcampImportPipeline::enqueue(ImportJob job)
{
    job.id           = m_nextJobId++;
    job.stage        = ImportJobStage::Queued;
    const quint64 id = job.id;
    m_jobs.append(std::move(job));
    m_idle = false;
    if(m_stopping) {
        finishJob(id, ImportJobStage::Cancelled);
        return;
    }
    const ImportJob* queued = findJob(id);
    if(queued && queued->admissionFailure) {
        finishJob(id, ImportJobStage::Failed);
    }
    else if(queued && std::holds_alternative<ResolutionFailure>(queued->scope.outcome)) {
        finishJob(id, ImportJobStage::Failed);
    }
    else if(queued && std::holds_alternative<ResolutionCancelled>(queued->scope.outcome)) {
        finishJob(id, ImportJobStage::Cancelled);
    }
    else {
        pump();
    }
}

void BandcampImportPipeline::cancel()
{
    if(m_stopping) {
        return;
    }
    m_stopping = true;

    QList<ImportJob> cancelledJobs;
    QList<ImportJob> probingJobs;
    QList<ImportJob> committingJobs;
    QList<ImportJob> survivingJobs;
    for(ImportJob& job : m_jobs) {
        if(job.id == m_insertingJobId && job.stage == ImportJobStage::Committed) {
            survivingJobs.append(job);
            continue;
        }
        if(job.stage == ImportJobStage::Completed || job.stage == ImportJobStage::Failed
           || job.stage == ImportJobStage::Cancelled) {
            continue;
        }
        if(job.stage == ImportJobStage::Probing) {
            probingJobs.append(job);
        }
        else if(job.stage == ImportJobStage::Committing) {
            committingJobs.append(job);
        }
        job.stage = ImportJobStage::Cancelled;
        cancelledJobs.append(job);
    }
    m_jobs = std::move(survivingJobs);

    const QPointer<BandcampImportPipeline> guard{this};
    for(const ImportJob& job : std::as_const(probingJobs)) {
        if(m_cancelProbe) {
            m_cancelProbe(job);
            if(!guard) {
                return;
            }
        }
    }
    for(const ImportJob& job : std::as_const(committingJobs)) {
        if(m_cancelCommit) {
            m_cancelCommit(job);
            if(!guard) {
                return;
            }
        }
    }
    for(const ImportJob& job : std::as_const(cancelledJobs)) {
        completeRelease(job);
        if(!guard) {
            return;
        }
        emit jobFinished(job);
        if(!guard) {
            return;
        }
    }
    checkBecameIdle();
}

bool BandcampImportPipeline::isIdle() const
{
    return m_jobs.isEmpty();
}

bool BandcampImportPipeline::hasActiveWork() const
{
    return std::any_of(m_jobs.cbegin(), m_jobs.cend(), [](const ImportJob& job) {
        return job.stage == ImportJobStage::Probing || job.stage == ImportJobStage::Committing
            || job.stage == ImportJobStage::Committed;
    });
}

qsizetype BandcampImportPipeline::completedWork() const
{
    return m_completedWork;
}

qsizetype BandcampImportPipeline::totalWork() const
{
    return m_totalWork;
}

ImportJob* BandcampImportPipeline::findJob(quint64 id)
{
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const ImportJob& job) { return job.id == id; });
    return it == m_jobs.end() ? nullptr : &*it;
}

const ImportJob* BandcampImportPipeline::findJob(quint64 id) const
{
    const auto it = std::find_if(m_jobs.cbegin(), m_jobs.cend(), [id](const ImportJob& job) { return job.id == id; });
    return it == m_jobs.cend() ? nullptr : &*it;
}

// Invariant: Handlers (probe, commit) receive a by-value copy of ImportJob.
// Access to m_jobs uses index/front lookups exclusively, ensuring no internal
// pointers or references are held across handler invocations or pruning operations.
void BandcampImportPipeline::pump()
{
    if(m_stopping) {
        return;
    }
    if(m_pumping) {
        m_pumpAgain = true;
        return;
    }

    m_pumping = true;
    const QPointer<BandcampImportPipeline> guard{this};
    do {
        m_pumpAgain = false;
        pruneFinishedFront();
        if(!guard || m_stopping) {
            break;
        }

        qsizetype activeProbes = 0;
        for(const ImportJob& job : std::as_const(m_jobs)) {
            activeProbes += job.stage == ImportJobStage::Probing ? 1 : 0;
        }
        for(qsizetype index = 0; index < m_jobs.size() && activeProbes < MaxImportProbeConcurrency; ++index) {
            if(m_jobs.at(index).stage != ImportJobStage::Queued) {
                continue;
            }
            const quint64 id = m_jobs.at(index).id;
            if(!m_probeHandler) {
                m_jobs[index].importTracks.clear();
                m_jobs[index].importTracks.reserve(m_jobs.at(index).tracks.size());
                for(const BandcampResolvedTrack& track : std::as_const(m_jobs.at(index).tracks)) {
                    m_jobs[index].importTracks.append({track, {}});
                }
                m_jobs[index].stage  = ImportJobStage::Probed;
                const ImportJob copy = m_jobs.at(index);
                completeRelease(copy);
                if(!guard || m_stopping) {
                    break;
                }
                continue;
            }

            m_jobs[index].stage = ImportJobStage::Probing;
            ++activeProbes;
            const ImportJob copy       = m_jobs.at(index);
            const ProbeHandler handler = m_probeHandler;
            handler(copy, [guard, id](QList<BandcampStreamProbeResult> results) mutable {
                if(guard) {
                    guard->finishProbe(id, std::move(results));
                }
            });
            if(!guard || m_stopping) {
                break;
            }
        }
        if(!guard || m_stopping) {
            break;
        }

        pruneFinishedFront();
        if(!guard || m_stopping || m_jobs.isEmpty()) {
            continue;
        }
        if(m_jobs.front().stage == ImportJobStage::Probed) {
            const quint64 id = m_jobs.front().id;
            if(!m_commitHandler) {
                finishJob(id, ImportJobStage::Failed);
                if(!guard || m_stopping) {
                    break;
                }
                m_pumpAgain = true;
                continue;
            }
            m_jobs.front().stage = ImportJobStage::Committing;
            publishProgress();
            if(!guard || m_stopping) {
                break;
            }
            const ImportJob copy        = m_jobs.front();
            const CommitHandler handler = m_commitHandler;
            handler(copy, [guard, id](FooyinTrackCommitOutcome outcome) mutable {
                if(guard) {
                    guard->finishCommit(id, std::move(outcome));
                }
            });
            if(!guard || m_stopping) {
                break;
            }
        }
    } while(m_pumpAgain);
    if(guard) {
        guard->m_pumping = false;
        guard->checkBecameIdle();
    }
}

void BandcampImportPipeline::finishProbe(quint64 id, QList<BandcampStreamProbeResult> results)
{
    ImportJob* job = findJob(id);
    if(!job || job->stage != ImportJobStage::Probing || m_stopping) {
        return;
    }
    job->probeResults = std::move(results);
    QHash<qint64, BandcampStreamProbeResult> resultsByTrack;
    resultsByTrack.reserve(job->probeResults.size());
    for(const BandcampStreamProbeResult& result : std::as_const(job->probeResults)) {
        resultsByTrack.insert(result.trackId, result);
    }
    job->importTracks.clear();
    job->importTracks.reserve(job->tracks.size());
    for(const BandcampResolvedTrack& track : std::as_const(job->tracks)) {
        BandcampStreamProperties observed;
        const auto result = resultsByTrack.constFind(track.track.id);
        if(result == resultsByTrack.cend()) {
            qCWarning(BANDCAMP_TRACKS).noquote() << formatTraceEvent(CorrelationContext{.trackId = track.track.id},
                                                                     QStringLiteral("stream_probe_result_missing"), {});
        }
        else if(!result->failure) {
            observed = result->properties;
        }
        job->importTracks.append({track, std::move(observed)});
    }
    job->stage           = ImportJobStage::Probed;
    const ImportJob copy = *job;
    const QPointer<BandcampImportPipeline> guard{this};
    completeRelease(copy);
    if(guard) {
        pump();
    }
}

void BandcampImportPipeline::finishCommit(quint64 id, FooyinTrackCommitOutcome outcome)
{
    ImportJob* job = findJob(id);
    if(!job || job->stage != ImportJobStage::Committing || m_stopping) {
        return;
    }
    if(const auto* failure = std::get_if<FooyinTrackCommitFailure>(&outcome)) {
        job->commitFailure = *failure;
        finishJob(id, ImportJobStage::Failed);
        return;
    }

    job->committedTracks = std::get<FooyinTrackCommitSuccess>(outcome).tracks;
    job->stage           = ImportJobStage::Committed;
    const ImportJob copy = *job;
    m_insertingJobId     = id;
    const QPointer<BandcampImportPipeline> guard{this};
    const bool inserted = m_insertHandler && m_insertHandler(copy);
    if(!guard) {
        return;
    }
    m_insertingJobId = 0;
    job              = findJob(id);
    if(!job) {
        return;
    }
    if(m_stopping) {
        finishJob(id, inserted ? ImportJobStage::Completed : ImportJobStage::Cancelled);
        return;
    }
    finishJob(id, inserted ? ImportJobStage::Completed : ImportJobStage::Failed);
}

void BandcampImportPipeline::finishJob(quint64 id, ImportJobStage stage)
{
    ImportJob* job = findJob(id);
    if(!job || job->stage == ImportJobStage::Completed || job->stage == ImportJobStage::Failed
       || job->stage == ImportJobStage::Cancelled) {
        return;
    }
    job->stage           = stage;
    const ImportJob copy = *job;
    const QPointer<BandcampImportPipeline> guard{this};
    completeRelease(copy);
    if(!guard) {
        return;
    }
    emit jobFinished(copy);
    if(!guard) {
        return;
    }
    pruneFinishedFront();
    if(!m_stopping) {
        pump();
    }
    else {
        checkBecameIdle();
    }
}

void BandcampImportPipeline::completeRelease(const ImportJob& job)
{
    if(job.scope.page.kind == BandcampPageKind::Discography || job.scope.page.url.isEmpty()) {
        return;
    }
    const QString key = bandcampPageKey(job.scope.page);
    if(m_completedReleaseKeys.contains(key)) {
        return;
    }
    m_completedReleaseKeys.insert(key);
    ++m_completedWork;
    publishProgress();
}

void BandcampImportPipeline::publishProgress()
{
    emit progressChanged(qMin(m_completedWork, m_totalWork), m_totalWork);
}

void BandcampImportPipeline::pruneFinishedFront()
{
    while(!m_jobs.isEmpty()) {
        const ImportJobStage stage = m_jobs.front().stage;
        if(stage != ImportJobStage::Completed && stage != ImportJobStage::Failed
           && stage != ImportJobStage::Cancelled) {
            break;
        }
        m_jobs.removeFirst();
    }
}

void BandcampImportPipeline::checkBecameIdle()
{
    if(!m_idle && m_jobs.isEmpty() && !m_pumping) {
        m_idle = true;
        emit becameIdle();
    }
}
} // namespace Fooyin::Bandcamp
