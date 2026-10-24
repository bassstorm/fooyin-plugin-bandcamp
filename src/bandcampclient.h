#pragma once

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
    // Empty on success.
    QString error;

    [[nodiscard]] bool isValid() const
    {
        return error.isEmpty();
    }
};

class BandcampClient : public QObject
{
    Q_OBJECT

public:
    using Callback  = std::function<void(BandcampFetchResponse)>;
    using RequestId = quint64;

    explicit BandcampClient(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~BandcampClient() override;

    // Returns 0 when the request is refused. The callback runs on the thread that owns this object,
    // exactly once, unless the request is cancelled or this object is destroyed first.
    RequestId fetch(const QUrl& url, Callback callback);

    // Aborts a pending request. Its callback is not called.
    void cancel(RequestId id);

private:
    QNetworkAccessManager* m_network{nullptr};
    // A request is pending until its reply finishes or is cancelled. Whoever removes it owns the callback.
    QHash<RequestId, QNetworkReply*> m_pending;
    RequestId m_nextRequestId{1};
};
} // namespace Fooyin::Bandcamp
