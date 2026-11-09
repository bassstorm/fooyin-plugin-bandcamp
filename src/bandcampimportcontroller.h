#pragma once

#include "bandcampimportpipeline.h"

#include <utils/id.h>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>

#include <functional>
#include <optional>

namespace Fooyin::Bandcamp {
enum class BandcampPlaylistAvailability : quint8
{
    Writable,
    Missing,
    Temporary,
    Automatic,
    Locked,
    OwnerUnavailable,
};

struct BandcampPlaylistApi
{
    QPointer<QObject> owner;
    std::function<bool()> ownersAvailable;
    std::function<BandcampPlaylistAvailability(const Fooyin::UId&)> inspect;
    std::function<std::optional<Fooyin::UId>(const QString&)> createNew;
    std::function<void(const Fooyin::UId&, const Fooyin::TrackList&)> insert;
};

enum class BandcampImportPhase : quint8
{
    Discovering,
    Resolving,
    Probing,
    Committing,
    Finished,
};

struct BandcampImportProgress
{
    BandcampOperationId operationId{0};
    qsizetype requested{0};
    qsizetype delivered{0};
    qsizetype active{0};
    BandcampImportPhase phase{BandcampImportPhase::Resolving};
    qsizetype completedWork{0};
    qsizetype totalWork{0};
};

struct BandcampImportSummary
{
    BandcampOperationId operationId{0};
    qsizetype added{0};
    qsizetype failed{0};
    qsizetype cancelled{0};
    QString stopReason;
    Fooyin::UId createdPlaylistId;
};

class BandcampImportController final : public QObject
{
    Q_OBJECT

public:
    using CancelOperation = std::function<void(BandcampOperationId)>;

    BandcampImportController(BandcampOperationId operationId, QList<BandcampPageUrl> submittedPages,
                             bool forceNewPlaylist, Fooyin::UId selectedPlaylistId, FooyinLibraryApi libraryApi,
                             BandcampPlaylistApi playlistApi, CancelOperation cancelOperation,
                             BandcampStreamProbe* streamProbe = nullptr, bool probeStreams = false,
                             QObject* parent = nullptr, BandcampLibraryGateway* sharedGateway = nullptr);
    ~BandcampImportController() override;

    void start();
    void cancel();
    [[nodiscard]] bool isFinished() const
    {
        return m_finished;
    }
    void acceptScopeResult(const BandcampScopeResult& result);
    void acceptProgress(const BandcampOperationProgress& progress);
    void finishResolution(const BandcampOperationSummary& summary);

signals:
    void progressChanged(const Fooyin::Bandcamp::BandcampImportProgress& progress);
    void integrationError(const QString& reason);
    void finished(const Fooyin::Bandcamp::BandcampImportSummary& summary);

private:
    struct PendingProbeRetry
    {
        ImportJob job;
        BandcampImportPipeline::ProbeCompletion completion;
    };

    void configurePipeline();
    void submitProbe(quint64 jobId);
    void enqueueScope(const BandcampScopeResult& scope);
    void onPipelineProgress(qsizetype completedWork, qsizetype totalWork);
    void onPipelineJobFinished(const ImportJob& job);
    void publishImportProgress();
    void failRun(QString reason, bool logFailure = true);
    void requestResolutionCancellation();
    void cancelProbe(quint64 jobId);
    void cancelGatewayRequest(quint64 jobId);
    void finishIfReady();
    [[nodiscard]] bool playlistOwnersAvailable() const;
    [[nodiscard]] bool destinationWritable(const Fooyin::UId& id) const;
    [[nodiscard]] bool prepareDestination(const BandcampScopeResult& scope, Fooyin::UId& id);
    [[nodiscard]] QString newPlaylistName(const BandcampResolvedData& data) const;
    [[nodiscard]] bool isFatalGatewayFailure(const FooyinTrackCommitFailure& failure) const;

    BandcampOperationId m_operationId{0};
    QList<BandcampPageUrl> m_submittedPages;
    Fooyin::UId m_destinationId;
    Fooyin::UId m_createdPlaylistId;
    QPointer<QObject> m_libraryOwner;
    BandcampPlaylistApi m_playlistApi;
    CancelOperation m_cancelOperation;
    QPointer<BandcampLibraryGateway> m_gateway;
    bool m_ownsGateway{false};
    QPointer<BandcampStreamProbe> m_streamProbe;
    QHash<quint64, quint64> m_probeTokens;
    QHash<quint64, PendingProbeRetry> m_pendingProbeRetries;
    QHash<quint64, quint64> m_gatewayTokens;
    BandcampImportPipeline m_pipeline;
    QQueue<BandcampScopeResult> m_pendingScopes;
    BandcampOperationProgress m_progress;
    qsizetype m_pipelineCompletedWork{0};
    qsizetype m_totalWork{0};
    qsizetype m_added{0};
    qsizetype m_failed{0};
    qsizetype m_cancelled{0};
    qsizetype m_committedCount{0};
    QString m_stopReason;
    QString m_pendingFatalReason;
    bool m_probeStreams{false};
    bool m_pendingNewPlaylist{false};
    bool m_initialFailure{false};
    bool m_started{false};
    bool m_resolutionFinished{false};
    bool m_resolutionCancellationIssued{false};
    bool m_stopping{false};
    bool m_finished{false};
};
} // namespace Fooyin::Bandcamp
