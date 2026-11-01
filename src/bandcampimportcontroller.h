#pragma once

#include "bandcampfooyintrack.h"
#include "bandcampservice.h"

#include <utils/id.h>

#include <QQueue>
#include <QObject>
#include <QPointer>

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
};

class BandcampImportController final : public QObject
{
    Q_OBJECT

public:
    using CancelOperation = std::function<void(BandcampOperationId)>;

    BandcampImportController(BandcampOperationId operationId, QList<BandcampPageUrl> submittedPages,
                             bool forceNewPlaylist, Fooyin::UId selectedPlaylistId, int activeTimeoutMs,
                             FooyinLibraryApi libraryApi, BandcampPlaylistApi playlistApi,
                             CancelOperation cancelOperation, QObject* parent = nullptr);
    ~BandcampImportController() override;

    void start();
    void cancel();
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
        Gateway,
        PreparingDestination,
        Inserting,
    };

    void pump();
    void onCommit(const BandcampScopeResult& scope, FooyinTrackCommitOutcome outcome);
    void failRun(QString reason, bool countCurrentScope, bool logFailure = true);
    void requestResolutionCancellation();
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
    int m_activeTimeoutMs{0};
    QPointer<QObject> m_libraryOwner;
    BandcampPlaylistApi m_playlistApi;
    CancelOperation m_cancelOperation;
    BandcampLibraryGateway* m_gateway{nullptr};
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
