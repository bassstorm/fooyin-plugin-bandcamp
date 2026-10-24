#pragma once

#include "bandcampclient.h"
#include "bandcampmodel.h"
#include "bandcampurl.h"

#include <QFuture>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;
class QTimer;

namespace Fooyin::Bandcamp {
struct BandcampResolvedPage
{
    BandcampPageUrl page;
    // Empty when the page was parsed.
    QString error;
    QStringList warnings;
    // Only the field matching page.kind is meaningful.
    BandcampAlbum album;
    BandcampTrack track;

    [[nodiscard]] bool isValid() const
    {
        return error.isEmpty();
    }
};

class BandcampService : public QObject
{
    Q_OBJECT

public:
    explicit BandcampService(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~BandcampService() override;

    // Number of items fetched or parsed at once. Clamped to a supported range.
    void setMaxItemsInProgress(int count);
    // Time budget for one item in milliseconds, covering fetch and parse. Clamped to 1 ms-120 s.
    // Applies to items started afterwards.
    void setItemTimeoutMs(int timeoutMs);

    // Results arrive in input order once every URL in the batch has finished.
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
        // A track page with an album is parsed first, then its album page is fetched for artist and total.
        bool awaitingAlbum{false};
        BandcampPageUrl albumPage;
        BandcampResolvedPage partial;
    };

    struct Batch
    {
        int remaining{0};
        QList<BandcampResolvedPage> results;
    };

    void startNextJobs();
    void startJob(const Job& job);
    void startFetch(quint64 jobId, const QString& url);
    void onFetched(quint64 jobId, BandcampFetchResponse response);
    void startParse(quint64 jobId, const BandcampPageUrl& page, QByteArray body, CorrelationContext context);
    void onParsed(quint64 jobId, BandcampResolvedPage result);
    void onTimedOut(quint64 jobId);
    void finishJob(quint64 jobId, BandcampResolvedPage result);
    void recordResult(const Job& job, BandcampResolvedPage result);

    BandcampClient* m_client{nullptr};
    int m_maxItemsInProgress{0};
    int m_itemTimeoutMs{0};
    QList<Job> m_queued;
    QHash<quint64, ActiveJob> m_active;
    // Keyed by job id. Kept until the parse task ends, even after its job has timed out.
    QHash<quint64, QFuture<BandcampResolvedPage>> m_parses;
    quint64 m_nextJobId{1};
    quint64 m_nextBatchId{1};
    QHash<quint64, Batch> m_batches;
};
} // namespace Fooyin::Bandcamp
