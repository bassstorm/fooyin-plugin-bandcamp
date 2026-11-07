#pragma once

#include "bandcamplibrarygateway.h"
#include "bandcampresolutionservice.h"

#include <QHash>
#include <QQueue>
#include <QSet>

namespace Fooyin::Bandcamp {
[[nodiscard]] bool hasSelectedBandcampTracks(const Fooyin::TrackList& selection);

enum class BandcampRefreshError : quint8
{
    InvalidPersistedState,
    ResolutionFailed,
    MissingSelectedTrack,
    UnusableRefreshedExpiry,
    WriteFailed,
    SourceUnavailable,
    SubmissionRejected,
    OperationFailed,
};

struct BandcampRefreshFailure
{
    BandcampRefreshError code{BandcampRefreshError::InvalidPersistedState};
    QString reason;
    std::optional<ResolutionError> cause;
};

enum class BandcampRefreshSuccess : quint8
{
    Updated,
    Unchanged,
};
struct BandcampRefreshCancelled
{ };
using BandcampRefreshOutcome = std::variant<BandcampRefreshSuccess, BandcampRefreshFailure, BandcampRefreshCancelled>;

struct BandcampRefreshSummary
{
    BandcampOperationId operationId{0};
    // Counts unique selected Bandcamp identities; repeated occurrences are one consumer.
    qsizetype updated{0};
    qsizetype unchanged{0};
    qsizetype failed{0};
    qsizetype cancelled{0};
};

// One submission owns batch eligibility, consumer association, resolution and resident writes.
// Selection/options are copied; no playlist or UI objects are consulted afterwards.
class BandcampRefreshController final : public QObject
{
    Q_OBJECT
public:
    BandcampRefreshController(BandcampResolutionService* service, Fooyin::TrackList selected, OperationOptions options,
                              FooyinLibraryApi libraryApi, QObject* parent = nullptr,
                              BandcampLibraryGateway* sharedGateway = nullptr);
    ~BandcampRefreshController() override;
    void start(qint64 nowMs);
    void cancel();

signals:
    void progressChanged(const Fooyin::Bandcamp::BandcampOperationProgress& progress);
    void consumerFinished(qint64 trackId, const Fooyin::Bandcamp::BandcampRefreshOutcome& outcome);
    void finished(const Fooyin::Bandcamp::BandcampRefreshSummary& summary);

private:
    void acceptScope(const BandcampScopeResult& result);
    void pump();
    void settle(qint64 id, BandcampRefreshOutcome outcome, bool alreadyLogged = false);
    void finishIfReady();
    void withdrawSource();
    void cancelGatewayRequest();
    void failPending(BandcampRefreshFailure error);
    void stopWithFailure(BandcampRefreshFailure error);

    QPointer<BandcampResolutionService> m_service;
    Fooyin::TrackList m_selected;
    OperationOptions m_options;
    FooyinLibraryApi m_libraryApi;
    QPointer<BandcampLibraryGateway> m_gateway;
    quint64 m_gatewayRequest{0};
    bool m_ownsGateway{false};
    QHash<QString, QList<qint64>> m_consumers;
    QSet<qint64> m_unsettled;
    qsizetype m_anonymousUnsettled{0};
    QQueue<BandcampResolvedTrack> m_pendingWrites;
    BandcampRefreshSummary m_summary;
    bool m_started{false};
    bool m_stopping{false};
    bool m_sourceFinished{false};
    bool m_withdrawalIssued{false};
    qint64 m_writingTrackId{0};
    bool m_finished{false};
};
} // namespace Fooyin::Bandcamp
