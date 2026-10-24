#include "bandcampclient.h"

#include "bandcamplogging.h"
#include "bandcampparser.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <memory>

namespace Fooyin::Bandcamp {
namespace {
constexpr int MaxRedirects = 5;

struct FetchState
{
    QByteArray body;
    bool tooLarge{false};
};

bool isHttpUrl(const QUrl& url)
{
    return url.isValid() && (url.scheme() == u"https" || url.scheme() == u"http") && !url.host().isEmpty();
}
} // namespace

BandcampClient::BandcampClient(QNetworkAccessManager* network, QObject* parent)
    : QObject(parent)
    , m_network{network}
{ }

BandcampClient::~BandcampClient()
{
    // Taking the map first means the finished handlers find nothing pending and skip their callbacks.
    const auto pending = std::exchange(m_pending, {});
    for(const PendingRequest& request : pending) {
        request.reply->abort();
    }
}

BandcampClient::RequestId BandcampClient::fetch(const QUrl& url, Callback callback, CorrelationContext context)
{
    if(!m_network || !isHttpUrl(url)) {
        BandcampFetchResponse response;
        response.url   = url;
        response.error = QStringLiteral("unsupported request");
        qCWarning(BANDCAMP_FETCH) << "Refusing to fetch" << redactUrlForLog(url) << response.error;
        QMetaObject::invokeMethod(
            this, [callback, response = std::move(response)]() mutable { callback(std::move(response)); },
            Qt::QueuedConnection);
        return 0;
    }

    QNetworkRequest request(url);
    request.setMaximumRedirectsAllowed(MaxRedirects);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    const RequestId id = m_nextRequestId++;
    context            = context.withRequest(static_cast<qint64>(id));

    qCInfo(BANDCAMP_FETCH) << "GET" << redactUrlForLog(url);
    qCDebug(BANDCAMP_TRACE_FETCH).noquote()
        << formatTraceEvent(context, QStringLiteral("request.started"),
                            {DiagnosticField::url(QStringLiteral("url"), url)});

    QNetworkReply* reply = m_network->get(request);
    m_pending.insert(id, {reply, context});
    auto state = std::make_shared<FetchState>();
    QElapsedTimer timer;
    timer.start();

    // The size check runs while data arrives, so an oversized page never fills memory.
    connect(reply, &QNetworkReply::readyRead, this, [reply, state]() {
        if(state->tooLarge) {
            return;
        }
        state->body.append(reply->readAll());
        if(state->body.size() > MaxPageBytes) {
            state->tooLarge = true;
            state->body.clear();
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, id, reply, state, url, context, callback = std::move(callback), timer]() mutable {
                // Not pending means the request was cancelled or this client is shutting down.
                if(m_pending.remove(id) == 0) {
                    reply->deleteLater();
                    return;
                }

                BandcampFetchResponse response;
                response.url    = url;
                response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

                if(!state->tooLarge) {
                    state->body.append(reply->readAll());
                }

                if(state->tooLarge) {
                    response.error = QStringLiteral("page exceeds the %1 MB limit").arg(MaxPageBytes / (1024 * 1024));
                }
                else if(response.status != 0 && (response.status < 200 || response.status > 299)) {
                    response.error = QStringLiteral("HTTP %1").arg(response.status);
                }
                else if(reply->error() != QNetworkReply::NoError) {
                    response.error = reply->errorString();
                }
                else {
                    response.body = std::move(state->body);
                }

                if(response.isValid()) {
                    qCInfo(BANDCAMP_FETCH) << "HTTP" << response.status << "in" << timer.elapsed() << "ms,"
                                           << response.body.size() << "bytes from" << redactUrlForLog(url);
                }
                else {
                    qCWarning(BANDCAMP_FETCH) << "Fetch of" << redactUrlForLog(url) << "failed after" << timer.elapsed()
                                              << "ms:" << response.error;
                }
                qCDebug(BANDCAMP_TRACE_FETCH).noquote()
                    << formatTraceEvent(context, QStringLiteral("request.finished"),
                                        {DiagnosticField::number(QStringLiteral("status"), response.status),
                                         DiagnosticField::number(QStringLiteral("bytes"),
                                                                 static_cast<qint64>(response.body.size())),
                                         DiagnosticField::number(QStringLiteral("elapsedMs"), timer.elapsed()),
                                         DiagnosticField::text(QStringLiteral("error"), response.error)});

                reply->deleteLater();
                callback(std::move(response));
            });
    return id;
}

void BandcampClient::cancel(RequestId id)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end()) {
        return;
    }

    // Copied before erase, since erasing invalidates the iterator.
    const PendingRequest request = it.value();
    m_pending.erase(it);
    qCDebug(BANDCAMP_TRACE_FETCH).noquote()
        << formatTraceEvent(request.context, QStringLiteral("request.cancelled"), {});
    // The finished handler sees the request is no longer pending and only cleans up the reply.
    request.reply->abort();
}
} // namespace Fooyin::Bandcamp
