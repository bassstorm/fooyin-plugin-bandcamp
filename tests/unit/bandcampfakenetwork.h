#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

using namespace Qt::StringLiterals;

struct FakeResponse
{
    int status{200};
    QByteArray body;
    QNetworkReply::NetworkError error{QNetworkReply::NoError};
    QString errorText;
    QUrl redirectTarget;
    QList<QByteArray> chunks;
    int delayMs{0};
    int chunkIntervalMs{0};
    int abortDelayMs{0};
    bool finalOnly{false};
    bool holdDelivery{false};
    bool holdAfterAbort{false};
};

class FakeReply final : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest& request, FakeResponse response, QObject* parent = nullptr,
              std::function<void(qint64)> readObserver = {})
        : QNetworkReply{parent}
        , m_response{std::move(response)}
        , m_readObserver{std::move(readObserver)}
    {
        if(m_response.body.isEmpty()) {
            for(const QByteArray& part : std::as_const(m_response.chunks)) {
                m_response.body.append(part);
            }
        }
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_response.status);
        if(m_response.redirectTarget.isValid()) {
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, m_response.redirectTarget);
        }
        if(m_response.error != QNetworkReply::NoError) {
            setError(m_response.error, m_response.errorText);
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        m_deliveryHeld = m_response.holdDelivery;
        QTimer::singleShot(m_response.delayMs, this, [this]() { beginDelivery(); });
    }

    void abort() override
    {
        if(m_finished || m_aborted) {
            return;
        }
        m_aborted   = true;
        m_available = m_readOffset;
        if(error() == QNetworkReply::NoError) {
            setError(QNetworkReply::OperationCanceledError, u"cancelled"_s);
        }
        if(!m_response.holdAfterAbort) {
            QTimer::singleShot(m_response.abortDelayMs, this, [this]() { finish(); });
        }
    }

    void finishAbort()
    {
        if(m_aborted) {
            finish();
        }
    }

    void releaseDelivery()
    {
        if(m_deliveryHeld) {
            m_deliveryHeld = false;
            beginDelivery();
        }
    }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>(m_available) - m_readOffset + QNetworkReply::bytesAvailable();
    }

    qint64 largestReadRequest() const
    {
        return m_largestReadRequest;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        m_largestReadRequest = std::max(m_largestReadRequest, maxSize);
        if(m_readObserver) {
            m_readObserver(maxSize);
        }
        const qint64 available = bytesAvailable() - QNetworkReply::bytesAvailable();
        if(available <= 0) {
            return -1;
        }
        const qint64 count = std::min(available, maxSize);
        std::memcpy(data, m_response.body.constData() + m_readOffset, static_cast<size_t>(count));
        m_readOffset += static_cast<qsizetype>(count);
        return count;
    }

private:
    void beginDelivery()
    {
        if(m_finished || m_aborted || m_deliveryHeld) {
            return;
        }
        if(m_response.finalOnly) {
            m_available = m_aborted ? m_readOffset : m_response.body.size();
            finish();
            return;
        }
        if(m_response.chunks.isEmpty()) {
            m_available = m_aborted ? m_readOffset : m_response.body.size();
            emit readyRead();
            finish();
            return;
        }
        m_chunkIndex = 0;
        deliverChunk();
    }

    void deliverChunk()
    {
        if(m_finished || m_aborted) {
            return;
        }
        if(m_chunkIndex >= m_response.chunks.size()) {
            finish();
            return;
        }
        const QByteArray& chunk = m_response.chunks.at(m_chunkIndex++);
        m_available += chunk.size();
        emit readyRead();
        QTimer::singleShot(m_response.chunkIntervalMs, this, [this]() { deliverChunk(); });
    }

    void finish()
    {
        if(m_finished) {
            return;
        }
        m_available = m_aborted ? m_readOffset : m_response.body.size();
        m_finished  = true;
        emit finished();
    }

    FakeResponse m_response;
    std::function<void(qint64)> m_readObserver;
    qsizetype m_available{0};
    qsizetype m_readOffset{0};
    qsizetype m_chunkIndex{0};
    qint64 m_largestReadRequest{0};
    bool m_aborted{false};
    bool m_deliveryHeld{false};
    bool m_finished{false};
};

class FakeNetworkManager final : public QNetworkAccessManager
{
public:
    using Responder = std::function<FakeResponse(const QNetworkRequest&)>;

    explicit FakeNetworkManager(QObject* parent = nullptr)
        : QNetworkAccessManager{parent}
    { }

    void setResponder(Responder responder)
    {
        m_responder = std::move(responder);
    }

    qsizetype liveReplyCount() const
    {
        return std::count_if(m_replies.cbegin(), m_replies.cend(), [](const auto& reply) { return !reply.isNull(); });
    }

    QList<QPointer<FakeReply>> replies() const
    {
        return m_replies;
    }

    void destroyReplies()
    {
        const auto replies = m_replies;
        for(const QPointer<FakeReply>& reply : replies) {
            if(reply) {
                delete reply;
            }
        }
    }

    void finishAbortedReplies()
    {
        const auto current = m_replies;
        for(const QPointer<FakeReply>& reply : current) {
            if(reply) {
                reply->finishAbort();
            }
        }
    }

    void releaseHeldResponses()
    {
        const auto current = m_replies;
        for(const QPointer<FakeReply>& reply : current) {
            if(reply) {
                reply->releaseDelivery();
            }
        }
    }

    qint64 largestReadRequest() const
    {
        return m_largestReadRequest;
    }

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* outgoingData) override
    {
        Q_UNUSED(operation);
        Q_UNUSED(outgoingData);
        requested.append(request.url());
        const FakeResponse response = m_responder ? m_responder(request) : FakeResponse{};
        auto* reply                 = new FakeReply{request, response, this, [this](qint64 size) {
                                        m_largestReadRequest = std::max(m_largestReadRequest, size);
                                    }};
        m_replies.append(reply);
        return reply;
    }

public:
    QList<QUrl> requested;

private:
    Responder m_responder;
    QList<QPointer<FakeReply>> m_replies;
    qint64 m_largestReadRequest{0};
};
