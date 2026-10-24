#pragma once

#include "bandcampdiagnostics.h"
#include "bandcampresolution.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Fooyin::Bandcamp {
struct BandcampFetchResponse
{
    QUrl url;
    // 0 when no HTTP response was received.
    int status{0};
    QByteArray body;
};

using BandcampFetchOutcome = ResolutionOutcome<BandcampFetchResponse>;

class BandcampClient : public QObject
{
    Q_OBJECT

public:
    using Callback  = std::function<void(BandcampFetchOutcome)>;
    using RequestId = quint64;

    explicit BandcampClient(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~BandcampClient() override;

    // Returns 0 when the request is refused. The callback runs on the thread that
    // owns this object, exactly once, unless the request is cancelled or this
    // object is destroyed first. The context's request ID is replaced with the ID
    // assigned to this request.
    RequestId fetch(const QUrl& url, Callback callback, CorrelationContext context = {});

    // Aborts a pending request. Its callback is not called.
    void cancel(RequestId id);

private:
    struct PendingRequest
    {
        QNetworkReply* reply{nullptr};
        CorrelationContext context;
    };

    QNetworkAccessManager* m_network{nullptr};
    // A request is pending until its reply finishes or is cancelled. Whoever
    // removes it owns the callback.
    QHash<RequestId, PendingRequest> m_pending;
    RequestId m_nextRequestId{1};
};
} // namespace Fooyin::Bandcamp
