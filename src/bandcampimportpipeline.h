#pragma once

#include "bandcamplibrarygateway.h"
#include "bandcampresolutionservice.h"
#include "bandcampstreamprobe.h"

#include <QObject>
#include <QSet>

#include <functional>
#include <optional>
#include <utility>

namespace Fooyin::Bandcamp {
inline constexpr qsizetype MaxImportProbeConcurrency = MaxProbeRequests;

enum class ImportJobStage : quint8
{
    Queued,
    Probing,
    Probed,
    Committing,
    Committed,
    Completed,
    Failed,
    Cancelled,
};

struct ImportJob
{
    explicit ImportJob(BandcampScopeResult result)
        : scope{std::move(result)}
    { }

    quint64 id{0};
    BandcampScopeResult scope;
    QList<BandcampResolvedTrack> tracks;
    QList<BandcampStreamProbeResult> probeResults;
    QList<BandcampImportTrack> importTracks;
    Fooyin::TrackList committedTracks;
    std::optional<FooyinTrackCommitFailure> commitFailure;
    std::optional<QString> admissionFailure;
    ImportJobStage stage{ImportJobStage::Queued};
};

class BandcampImportPipeline final : public QObject
{
    Q_OBJECT

public:
    using ProbeCompletion  = std::function<void(QList<BandcampStreamProbeResult>)>;
    using CommitCompletion = std::function<void(FooyinTrackCommitOutcome)>;
    using ProbeHandler     = std::function<void(const ImportJob&, ProbeCompletion)>;
    using CommitHandler    = std::function<void(const ImportJob&, CommitCompletion)>;
    using InsertHandler    = std::function<bool(const ImportJob&)>;
    using CancelHandler    = std::function<void(const ImportJob&)>;

    explicit BandcampImportPipeline(QObject* parent = nullptr);

    void setProbeHandler(ProbeHandler handler, CancelHandler cancelHandler = {});
    void setCommitHandler(CommitHandler handler, CancelHandler cancelHandler = {});
    void setInsertHandler(InsertHandler handler);
    void setTotalWork(qsizetype totalWork);
    void enqueue(ImportJob job);
    void cancel();

    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] bool hasActiveWork() const;
    [[nodiscard]] qsizetype completedWork() const;
    [[nodiscard]] qsizetype totalWork() const;

signals:
    void progressChanged(qsizetype completedWork, qsizetype totalWork);
    void jobFinished(const Fooyin::Bandcamp::ImportJob& job);
    void becameIdle();

private:
    ImportJob* findJob(quint64 id);
    const ImportJob* findJob(quint64 id) const;
    void pump();
    void finishProbe(quint64 id, QList<BandcampStreamProbeResult> results);
    void finishCommit(quint64 id, FooyinTrackCommitOutcome outcome);
    void finishJob(quint64 id, ImportJobStage stage);
    void completeRelease(const ImportJob& job);
    void publishProgress();
    void pruneFinishedFront();
    void checkBecameIdle();

    QList<ImportJob> m_jobs;
    QSet<QString> m_completedReleaseKeys;
    ProbeHandler m_probeHandler;
    CancelHandler m_cancelProbe;
    CommitHandler m_commitHandler;
    CancelHandler m_cancelCommit;
    InsertHandler m_insertHandler;
    quint64 m_nextJobId{1};
    quint64 m_insertingJobId{0};
    qsizetype m_completedWork{0};
    qsizetype m_totalWork{0};
    bool m_stopping{false};
    bool m_pumping{false};
    bool m_pumpAgain{false};
    bool m_idle{true};
};
} // namespace Fooyin::Bandcamp

Q_DECLARE_METATYPE(Fooyin::Bandcamp::ImportJob)
