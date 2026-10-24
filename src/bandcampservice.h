#pragma once

#include "bandcampassembly.h"
#include "bandcampclient.h"
#include "bandcampurl.h"

#include <QFuture>
#include <QHash>
#include <QList>
#include <QObject>

#include <optional>

class QNetworkAccessManager;
class QTimer;

namespace Fooyin::Bandcamp {
using BandcampResolutionOutcome = ResolutionOutcome<BandcampResolvedData>;

struct BandcampResolvedPage
{
    BandcampPageUrl page;
    // Empty only for service-internal, not-yet-completed batch slots.
    std::optional<BandcampResolutionOutcome> outcome;

    [[nodiscard]] bool isValid() const
    {
        return outcome && std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(*outcome);
    }
};

class BandcampService : public QObject
{
    Q_OBJECT

public:
    explicit BandcampService(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~BandcampService() override;

    void setMaxItemsInProgress(int count);
    void setItemTimeoutMs(int timeoutMs);
    void resolve(const QList<BandcampPageUrl>& urls);

signals:
    void batchResolved(const QList<Fooyin::Bandcamp::BandcampResolvedPage>& results);

private:
    struct Job
    {
        quint64 jobId{0};
        quint64 batchId{0};
        int index{0};
        BandcampPageUrl page;
    };

    struct ActiveJob
    {
        Job job;
        QTimer* timeout{nullptr};
        BandcampClient::RequestId request{0};
        bool awaitingAlbum{false};
        BandcampPageUrl albumPage;
        std::optional<TrackPageCandidate> trackCandidate;
        CorrelationContext correlation;
    };

    struct Batch
    {
        int remaining{0};
        QList<BandcampResolvedPage> results;
    };

    void startNextJobs();
    void startJob(const Job& job);
    void startFetch(quint64 jobId, const QString& url);
    void onFetched(quint64 jobId, BandcampFetchOutcome response);
    void startParse(quint64 jobId, const BandcampPageUrl& page, QByteArray body, CorrelationContext context);
    void onParsed(quint64 jobId, ReleaseParseOutcome result);
    void onTimedOut(quint64 jobId);
    void finishJob(quint64 jobId, BandcampResolutionOutcome result);
    void finishFailure(quint64 jobId, ResolutionError error);
    void recordResult(const Job& job, BandcampResolutionOutcome result);

    BandcampClient* m_client{nullptr};
    int m_maxItemsInProgress{0};
    int m_itemTimeoutMs{0};
    QList<Job> m_queued;
    QHash<quint64, ActiveJob> m_active;
    QHash<quint64, QFuture<ReleaseParseOutcome>> m_parses;
    quint64 m_nextJobId{1};
    quint64 m_nextBatchId{1};
    QHash<quint64, Batch> m_batches;
};
} // namespace Fooyin::Bandcamp
