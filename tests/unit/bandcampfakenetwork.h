#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>

// Canned answer for one request. A status of 0 with an error models a transport failure.
struct FakeResponse
{
    int status{200};
    QByteArray body;
    QNetworkReply::NetworkError error{QNetworkReply::NoError};
    QString errorText;
};

// Delivers its data on the next event loop turn, so callers can connect handlers first, as they do with real replies.
class FakeReply : public QNetworkReply
{
public:
    FakeReply(const FakeResponse& response, const QNetworkRequest& request, QObject* parent)
        : QNetworkReply{parent}
        , m_body{response.body}
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        if(response.status != 0) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if(response.error != QNetworkReply::NoError) {
            setError(response.error, response.errorText);
        }
        QTimer::singleShot(0, this, [this]() { deliver(); });
    }

    void abort() override
    {
        // Like a real reply, the abort is reported through finished rather than synchronously.
        if(!m_done) {
            m_aborted = true;
            setError(QNetworkReply::OperationCanceledError, QStringLiteral("aborted"));
        }
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        const qint64 count = std::min<qint64>(maxlen, static_cast<qint64>(m_body.size()) - m_pos);
        if(count <= 0) {
            return 0;
        }
        std::copy_n(m_body.constData() + m_pos, count, data);
        m_pos += count;
        return count;
    }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>(m_body.size()) - m_pos + QNetworkReply::bytesAvailable();
    }

private:
    void deliver()
    {
        if(!m_aborted && !m_body.isEmpty()) {
            emit readyRead();
        }
        finish();
    }

    void finish()
    {
        if(m_done) {
            return;
        }
        m_done = true;
        emit finished();
    }

    QByteArray m_body;
    qint64 m_pos{0};
    bool m_aborted{false};
    bool m_done{false};
};

// Replaces the network with a responder. Every request it sees is recorded.
class FakeNetworkManager : public QNetworkAccessManager
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

    QList<QUrl> requested;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requested.append(request.url());
        const FakeResponse response = m_responder ? m_responder(request) : FakeResponse{};
        return new FakeReply(response, request, this);
    }

private:
    Responder m_responder;
};
