#pragma once

#include "bandcamplibrarygateway.h"
#include "bandcampresolutionservice.h"
#include "bandcampstreamprobe.h"

#include <utils/id.h>

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
    void progressChanged(const Fooyin::Bandcamp::BandcampOperationProgress& progress);
    void importing();
    void integrationError(const QString& reason);
    void finished(const Fooyin::Bandcamp::BandcampImportSummary& summary);

private:
    enum class HandoffPhase : quint8
    {
        None,
        Probing,
        Gateway,
        PreparingDestination,
        Inserting,
    };

    void pump();
    void onProbe(const BandcampScopeResult& scope, QList<BandcampResolvedTrack> tracks,
                 QList<BandcampStreamProbeResult> results);
    void commitScope(const BandcampScopeResult& scope, QList<BandcampImportTrack> tracks);
    void onCommit(const BandcampScopeResult& scope, FooyinTrackCommitOutcome outcome);
    void failRun(QString reason, bool countCurrentScope, bool logFailure = true);
    void requestResolutionCancellation();
    void cancelGatewayRequest();
    void clearPendingAsCancelled();
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
    quint64 m_gatewayRequest{0};
    bool m_ownsGateway{false};
    QPointer<BandcampStreamProbe> m_streamProbe;
    quint64 m_probeToken{0};
    bool m_probeStreams{false};
    QQueue<BandcampScopeResult> m_pendingScopes;
    qsizetype m_added{0};
    qsizetype m_failed{0};
    qsizetype m_cancelled{0};
    QString m_stopReason;
    HandoffPhase m_handoffPhase{HandoffPhase::None};
    bool m_pendingNewPlaylist{false};
    bool m_initialFailure{false};
    bool m_started{false};
    bool m_resolutionFinished{false};
    bool m_resolutionCancellationIssued{false};
    bool m_stopping{false};
    bool m_importingEmitted{false};
    bool m_finished{false};
};
} // namespace Fooyin::Bandcamp
