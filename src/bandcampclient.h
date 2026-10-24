#pragma once

#include "bandcampdiagnostics.h"
#include "bandcampresolution.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Fooyin::Bandcamp {
constexpr qsizetype MaxClientReadChunkBytes = 64 * 1024;

struct BandcampFetchResponse
{
    QUrl url;
    int status{0};
    QByteArray body;
};

struct BandcampFetchFailure
{
    ResolutionError error;
};

using BandcampFetchOutcome
    = std::variant<ResolutionSuccess<BandcampFetchResponse>, BandcampFetchFailure, ResolutionCancelled>;

class BandcampClient : public QObject
{
    Q_OBJECT

public:
    using Callback  = std::function<void(BandcampFetchOutcome)>;
    using RequestId = quint64;

    explicit BandcampClient(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~BandcampClient() override;

    // Completion is always queued and delivered exactly once after the request settles or fails to start.
    // Cancellation marks the result cancelled but retains the physical record until settlement.
    RequestId fetch(const QUrl& url, Callback callback, CorrelationContext context = {});
    void cancel(RequestId id);

private:
    struct PendingRequest
    {
        QPointer<QNetworkReply> reply;
        QUrl initialUrl;
        QUrl currentUrl;
        CorrelationContext context;
        Callback callback;
        QByteArray body;
        quint64 replyGeneration{0};
        int redirects{0};
        bool cancelled{false};
        bool tooLarge{false};
        QElapsedTimer timer;
    };

    void startHop(RequestId id, const QUrl& url);
    void drainReply(RequestId id, quint64 generation);
    void onFinished(RequestId id, quint64 generation);
    void onReplyDestroyed(RequestId id, quint64 generation);
    void settle(RequestId id, BandcampFetchOutcome outcome);

    QPointer<QNetworkAccessManager> m_network;
    QHash<RequestId, PendingRequest> m_pending;
    RequestId m_nextRequestId{1};
    bool m_destroying{false};
};
} // namespace Fooyin::Bandcamp
