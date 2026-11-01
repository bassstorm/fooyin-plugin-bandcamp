#pragma once

#include "bandcampdiagnostics.h"
#include "bandcampmodel.h"

#include <core/library/musiclibrary.h>
#include <core/track.h>
#include <core/trackmetadatastore.h>

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQueue>

#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace Fooyin::Bandcamp {
inline constexpr auto StreamProviderProperty   = "_STREAM_PROVIDER";
inline constexpr auto BandcampSourceUrlProperty = "_BANDCAMP_SOURCE_URL";
inline constexpr auto BandcampStreamUrlProperty = "_BANDCAMP_STREAM_URL";
inline constexpr auto BandcampExpiresAtProperty = "_BANDCAMP_EXPIRES_AT";
inline constexpr auto BandcampTrackIdProperty   = "_BANDCAMP_TRACK_ID";

[[nodiscard]] Fooyin::Track makeFooyinTrack(
    const BandcampResolvedTrack& resolved,
    std::shared_ptr<Fooyin::TrackMetadataStore> metadataStore = {});
[[nodiscard]] std::optional<qint64> bandcampStreamExpiryMs(const QString& streamUrl);

struct FooyinLibraryApi
{
    QPointer<QObject> owner;
    std::function<Fooyin::TrackList()> tracks;
    std::function<std::shared_ptr<Fooyin::TrackMetadataStore>()> metadataStore;
    std::function<Fooyin::WriteRequest(const Fooyin::TrackList&)> writeTrackMetadata;
};

[[nodiscard]] FooyinLibraryApi fooyinLibraryApi(Fooyin::MusicLibrary* library);

struct FooyinTrackCommitSuccess
{
    // Occurrence count/order is identical to the incoming list. New tracks remain ID-less
    // for the normal playlist insertion path.
    Fooyin::TrackList tracks;
    qsizetype updatedResidentCount{0};
    qsizetype newIdentityCount{0};
};

enum class FooyinTrackCommitErrorCode : quint8
{
    Cancelled,
    InvalidIdentity,
    InvalidTimeout,
    LibraryUnavailable,
    WrongThread,
    PersistenceFailed,
    HostOperationFailed,
};

struct FooyinTrackCommitFailure
{
    FooyinTrackCommitErrorCode code{FooyinTrackCommitErrorCode::HostOperationFailed};
    qint64 trackId{0};
    QString reason;
};

using FooyinTrackCommitOutcome = std::variant<FooyinTrackCommitSuccess, FooyinTrackCommitFailure>;

class BandcampLibraryGateway final : public QObject
{
public:
    using Completion = std::function<void(FooyinTrackCommitOutcome)>;

    explicit BandcampLibraryGateway(FooyinLibraryApi api, QObject* parent = nullptr);
    ~BandcampLibraryGateway() override;

    void cancel();
    void commit(QList<BandcampResolvedTrack> incoming, int activeTimeoutMs, QObject* callbackContext,
                CorrelationContext correlation, Completion completion);

private:
    struct CommitRequest
    {
        quint64 token{0};
        QList<BandcampResolvedTrack> incoming;
        int activeTimeoutMs{0};
        QPointer<QObject> callbackContext;
        CorrelationContext correlation;
        Completion completion;
        QMetaObject::Connection receiverDestroyed;
    };

    struct ActiveCommit
    {
        CommitRequest request;
        Fooyin::TrackList output;
        Fooyin::TrackList writeTargets;
        qsizetype newIdentityCount{0};
        QPointer<QFutureWatcher<Fooyin::WriteResult>> watcher;
        std::function<void()> cancelWrite;
        QElapsedTimer deadline;
        bool timedOut{false};
        bool callbackDelivered{false};
        bool withdrawn{false};
    };

    void scheduleNext();
    void startNext();
    void start(const CommitRequest& request);
    void onWriteFinished(quint64 token);
    void onTimeout(quint64 token);
    void withdraw(quint64 token);
    void failActive(FooyinTrackCommitFailure error);
    void finishActive(FooyinTrackCommitOutcome outcome);
    void abandonActive();
    void deliver(QObject* context, Completion completion, FooyinTrackCommitOutcome outcome,
                 const CorrelationContext& correlation) const;
    void traceTransportChange(const CorrelationContext& context, const Fooyin::Track& previous,
                              const Fooyin::Track& updated) const;
    void logFailure(const CorrelationContext& context, const FooyinTrackCommitFailure& failure) const;

    FooyinLibraryApi m_api;
    QQueue<CommitRequest> m_pending;
    std::optional<ActiveCommit> m_active;
    quint64 m_nextToken{1};
    bool m_nextScheduled{false};
    bool m_cancelled{false};
};
} // namespace Fooyin::Bandcamp
