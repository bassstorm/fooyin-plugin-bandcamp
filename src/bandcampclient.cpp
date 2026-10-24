#include "bandcampclient.h"

#include "bandcamplogging.h"
#include "bandcampparser.h"

#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace Fooyin::Bandcamp {
namespace {
constexpr int MaxRedirects = 5;

bool isHttpUrl(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == QStringLiteral("https") || scheme == QStringLiteral("http"))
        && !url.host().isEmpty() && url.userInfo().isEmpty();
}

bool isSafeRedirect(const QUrl& from, const QUrl& to)
{
    if(!isHttpUrl(to)) {
        return false;
    }
    return from.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || to.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
}

ResolutionError clientError(ResolutionErrorCode code, const QUrl& url, const QString& field, const QString& reason)
{
    return {code, ResolutionStage::Fetch, url, 0, field, reason};
}
} // namespace

BandcampClient::BandcampClient(QNetworkAccessManager* network, QObject* parent)
    : QObject{parent}
    , m_network{network}
{
    if(network) {
        connect(network, &QObject::destroyed, this, [this]() {
            m_network = nullptr;
            const QPointer<BandcampClient> guard{this};
            const QList<RequestId> ids = m_pending.keys();
            for(const RequestId id : ids) {
                if(!guard) {
                    return;
                }
                auto it = m_pending.find(id);
                if(it == m_pending.end()) {
                    continue;
                }
                if(it->reply) {
                    it->reply->abort();
                    continue;
                }
                const QUrl url = it->currentUrl;
                settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, url,
                                                            QStringLiteral("request"),
                                                            QStringLiteral("network manager was destroyed"))});
            }
            QTimer::singleShot(0, this, [guard, ids]() {
                if(!guard) {
                    return;
                }
                for(const RequestId id : ids) {
                    auto it = guard->m_pending.find(id);
                    if(it == guard->m_pending.end() || !it->reply) {
                        continue;
                    }
                    QNetworkReply* reply = it->reply.data();
                    delete reply;
                    if(!guard) {
                        return;
                    }
                }
            });
        });
    }
}

BandcampClient::~BandcampClient()
{
    m_destroying       = true;
    const auto pending = std::exchange(m_pending, {});
    for(const PendingRequest& request : pending) {
        if(request.reply) {
            disconnect(request.reply, nullptr, this, nullptr);
            request.reply->abort();
            request.reply->deleteLater();
        }
    }
}

BandcampClient::RequestId BandcampClient::fetch(const QUrl& url, Callback callback, CorrelationContext context)
{
    if(!m_network || !isHttpUrl(url)) {
        const RequestId id = m_nextRequestId++;
        const QPointer<BandcampClient> self{this};
        QMetaObject::invokeMethod(
            this,
            [self, callback = std::move(callback), url]() mutable {
                if(self) {
                    try {
                        callback(BandcampFetchFailure{clientError(ResolutionErrorCode::UnsupportedRequest, url,
                                                                  QStringLiteral("resourceUrl"),
                                                                  QStringLiteral("unsupported request URL"))});
                    }
                    catch(...) {
                        qCWarning(BANDCAMP_FETCH) << "A fetch completion handler threw";
                    }
                }
            },
            Qt::QueuedConnection);
        return id;
    }

    const RequestId id = m_nextRequestId++;
    context            = context.withRequest(static_cast<qint64>(id));
    PendingRequest pending;
    pending.initialUrl = url;
    pending.currentUrl = url;
    pending.context    = context;
    pending.callback   = std::move(callback);
    pending.timer.start();
    m_pending.insert(id, std::move(pending));

    qCDebug(BANDCAMP_TRACE_FETCH).noquote() << formatTraceEvent(context, QStringLiteral("request.started"),
                                                                {DiagnosticField::url(QStringLiteral("url"), url)});
    startHop(id, url);
    return id;
}

void BandcampClient::startHop(RequestId id, const QUrl& url)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end()) {
        return;
    }
    if(!m_network) {
        settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, url, QStringLiteral("request"),
                                                    QStringLiteral("network manager is unavailable"))});
        return;
    }

    QNetworkRequest request{url};
    request.setMaximumRedirectsAllowed(0);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30'000);

    try {
        QNetworkReply* reply = m_network->get(request);
        if(!reply) {
            settle(id,
                   BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, url, QStringLiteral("request"),
                                                    QStringLiteral("network manager returned no reply"))});
            return;
        }
        it = m_pending.find(id);
        if(it == m_pending.end()) {
            reply->abort();
            reply->deleteLater();
            return;
        }
        reply->setReadBufferSize(MaxClientReadChunkBytes);
        it->currentUrl           = url;
        it->reply                = reply;
        const quint64 generation = ++it->replyGeneration;
        connect(reply, &QNetworkReply::readyRead, this, [this, id, generation]() { drainReply(id, generation); });
        connect(reply, &QNetworkReply::finished, this, [this, id, generation]() { onFinished(id, generation); });
        connect(reply, &QObject::destroyed, this, [this, id, generation]() { onReplyDestroyed(id, generation); });
    }
    catch(...) {
        settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, url, QStringLiteral("request"),
                                                    QStringLiteral("network request could not be started"))});
    }
}

void BandcampClient::drainReply(RequestId id, quint64 generation)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end() || it->replyGeneration != generation || !it->reply || it->tooLarge) {
        return;
    }
    QNetworkReply* reply = it->reply;
    while(reply->bytesAvailable() > 0 && !it->tooLarge) {
        const qint64 remaining = static_cast<qint64>(MaxPageBytes) - it->body.size();
        const qint64 readSize  = std::min({reply->bytesAvailable(), MaxClientReadChunkBytes, remaining + 1});
        QByteArray chunk       = reply->read(readSize);
        if(chunk.isEmpty()) {
            break;
        }
        if(chunk.size() > remaining) {
            it->tooLarge = true;
            it->body.clear();
            reply->abort();
            return;
        }
        it->body.append(chunk);
    }
}

void BandcampClient::onFinished(RequestId id, quint64 generation)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end() || it->replyGeneration != generation || !it->reply) {
        return;
    }
    drainReply(id, generation);
    it = m_pending.find(id);
    if(it == m_pending.end() || it->replyGeneration != generation || !it->reply) {
        return;
    }

    QNetworkReply* reply    = it->reply;
    const QUrl effectiveUrl = reply->url();
    const bool tooLarge     = it->tooLarge;
    const bool cancelled    = it->cancelled;
    if(cancelled || tooLarge) {
        it->reply = nullptr;
        it->body.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->deleteLater();
        if(cancelled) {
            settle(id, ResolutionCancelled{});
        }
        else {
            settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::PageTooLarge, effectiveUrl,
                                                        QStringLiteral("response"),
                                                        QStringLiteral("response byte limit exceeded"))});
        }
        return;
    }

    const int status          = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if(status >= 300 && status < 400 && redirectTarget.isValid()) {
        const QUrl target = effectiveUrl.resolved(redirectTarget);
        if(it->redirects >= MaxRedirects || !isSafeRedirect(it->currentUrl, target)) {
            reply->deleteLater();
            settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::UnsupportedRequest, target,
                                                        QStringLiteral("redirect"),
                                                        QStringLiteral("unsafe or excessive redirect"))});
            return;
        }
        ++it->redirects;
        it->body.clear();
        it->reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->deleteLater();
        startHop(id, target);
        return;
    }

    const QNetworkReply::NetworkError networkError = reply->error();
    const QUrl initialUrl                          = it->initialUrl;
    QByteArray body                                = std::move(it->body);
    const CorrelationContext context               = it->context;
    const qint64 elapsed                           = it->timer.elapsed();
    it->reply                                      = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();

    if(!isSafeRedirect(initialUrl, effectiveUrl)) {
        settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::UnsupportedRequest, effectiveUrl,
                                                    QStringLiteral("redirect"),
                                                    QStringLiteral("unsafe effective response URL"))});
        return;
    }
    if(status == 0) {
        settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, effectiveUrl,
                                                    QStringLiteral("httpStatus"),
                                                    QStringLiteral("no HTTP response received"))});
        return;
    }
    if(networkError != QNetworkReply::NoError) {
        settle(id,
               BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, effectiveUrl,
                                                QStringLiteral("request"), QStringLiteral("network request failed"))});
        return;
    }
    if(status >= 200 && status <= 299) {
        qCInfo(BANDCAMP_FETCH) << "HTTP" << status << "in" << elapsed << "ms," << body.size() << "bytes";
        qCDebug(BANDCAMP_TRACE_FETCH).noquote()
            << formatTraceEvent(context, QStringLiteral("request.finished"),
                                {DiagnosticField::number(QStringLiteral("status"), status),
                                 DiagnosticField::number(QStringLiteral("bytes"), body.size()),
                                 DiagnosticField::number(QStringLiteral("elapsedMs"), elapsed)});
        settle(id,
               ResolutionSuccess<BandcampFetchResponse>{BandcampFetchResponse{effectiveUrl, status, std::move(body)}});
        return;
    }

    settle(id, BandcampFetchFailure{clientError(ResolutionErrorCode::FetchFailed, effectiveUrl,
                                                QStringLiteral("httpStatus"), QStringLiteral("HTTP %1").arg(status))});
}

void BandcampClient::onReplyDestroyed(RequestId id, quint64 generation)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end() || it->replyGeneration != generation || m_destroying) {
        return;
    }
    const bool cancelled = it->cancelled;
    const QUrl url       = it->currentUrl;
    it->reply            = nullptr;
    settle(id, cancelled ? BandcampFetchOutcome{ResolutionCancelled{}}
                         : BandcampFetchOutcome{BandcampFetchFailure{
                               clientError(ResolutionErrorCode::FetchFailed, url, QStringLiteral("request"),
                                           QStringLiteral("network reply was destroyed before completion"))}});
}

void BandcampClient::settle(RequestId id, BandcampFetchOutcome outcome)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end()) {
        return;
    }
    PendingRequest pending = std::move(it.value());
    m_pending.erase(it);
    if(pending.reply) {
        disconnect(pending.reply, nullptr, this, nullptr);
        pending.reply->deleteLater();
    }
    if(const auto* failure = std::get_if<BandcampFetchFailure>(&outcome)) {
        qCDebug(BANDCAMP_TRACE_FETCH).noquote() << formatTraceEvent(
            pending.context, QStringLiteral("request.failed"),
            {DiagnosticField::text(QStringLiteral("code"), resolutionErrorCodeName(failure->error.code)),
             DiagnosticField::number(QStringLiteral("elapsedMs"), pending.timer.elapsed())});
    }
    Callback callback = std::move(pending.callback);
    const QPointer<BandcampClient> guard{this};
    QMetaObject::invokeMethod(
        this,
        [guard, callback = std::move(callback), outcome = std::move(outcome)]() mutable {
            if(!guard) {
                return;
            }
            try {
                callback(std::move(outcome));
            }
            catch(...) {
                qCWarning(BANDCAMP_FETCH) << "A fetch completion handler threw";
            }
        },
        Qt::QueuedConnection);
}

void BandcampClient::cancel(RequestId id)
{
    auto it = m_pending.find(id);
    if(it == m_pending.end() || it->cancelled) {
        return;
    }
    it->cancelled = true;
    qCDebug(BANDCAMP_TRACE_FETCH).noquote() << formatTraceEvent(it->context, QStringLiteral("request.cancelled"), {});
    if(it->reply) {
        it->reply->abort();
    }
}
} // namespace Fooyin::Bandcamp
