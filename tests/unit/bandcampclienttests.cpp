#include "bandcampclient.h"

#include "bandcampfakenetwork.h"
#include "bandcamplogcapture.h"
#include "bandcamplogging.h"
#include "bandcampparser.h"

#include <QEventLoop>
#include <QTimer>
#include <QtTest>

#include <memory>
#include <optional>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
const QUrl AlbumUrl{u"https://artist.bandcamp.com/album/record?token=SECRET#fragment"_s};

struct Fetched
{
    BandcampClient::RequestId id{0};
    std::optional<BandcampFetchResponse> response;
};

// Runs a fetch and spins the event loop until it calls back. The response stays empty on timeout.
Fetched fetchAndWait(BandcampClient& client, const QUrl& url, CorrelationContext context = {})
{
    Fetched fetched;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    guard.start(5000);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);

    fetched.id = client.fetch(
        url,
        [&fetched, &loop](BandcampFetchResponse response) {
            fetched.response = std::move(response);
            loop.quit();
        },
        std::move(context));
    loop.exec();
    return fetched;
}

QStringList traceLines(const QString& event)
{
    QStringList lines;
    for(const CapturedLog& log : capturedLogs()) {
        if(log.category == u"fy.trace.bandcamp.fetch"_s && log.message.contains(event)) {
            lines.append(log.message);
        }
    }
    return lines;
}
} // namespace

class ClientTests : public QObject
{
    Q_OBJECT

private slots:
    void successReturnsBody()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 200, .body = "<html>ok</html>"};
        });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        QVERIFY(fetched.response->isValid());
        QCOMPARE(fetched.response->status, 200);
        QCOMPARE(fetched.response->body, QByteArray("<html>ok</html>"));
        QCOMPARE(network.requested.size(), 1);
        QCOMPARE(network.requested.first(), AlbumUrl);
    }

    void httpErrorIsReported()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.status = 404}; });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        QVERIFY(!fetched.response->isValid());
        QCOMPARE(fetched.response->error, u"HTTP 404"_s);
        QCOMPARE(fetched.response->status, 404);
        QVERIFY(fetched.response->body.isEmpty());
    }

    void transportErrorIsReported()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 0,
                                .error     = QNetworkReply::ConnectionRefusedError,
                                .errorText = u"connection refused"_s};
        });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        QVERIFY(!fetched.response->isValid());
        QCOMPARE(fetched.response->error, u"connection refused"_s);
        QCOMPARE(fetched.response->status, 0);
        QVERIFY(fetched.response->body.isEmpty());
    }

    void oversizedBodyIsRejected()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 200, .body = QByteArray(MaxPageBytes + 1, 'x')};
        });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        QVERIFY(!fetched.response->isValid());
        QVERIFY(fetched.response->error.startsWith(u"page exceeds"_s));
        QVERIFY(fetched.response->body.isEmpty());
    }

    void unsupportedSchemeIsRefusedWithoutRequest()
    {
        FakeNetworkManager network;
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, QUrl(u"ftp://artist.bandcamp.com/album"_s));
        QCOMPARE(fetched.id, BandcampClient::RequestId{0});
        QVERIFY(fetched.response.has_value());
        QCOMPARE(fetched.response->error, u"unsupported request"_s);
        QVERIFY(network.requested.isEmpty());
    }

    void cancelSuppressesCallback()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            return FakeResponse{.status = 200, .body = request.url().path().toUtf8()};
        });
        BandcampClient client{&network};

        int cancelledCalls = 0;
        const auto id      = client.fetch(AlbumUrl, [&cancelledCalls](BandcampFetchResponse) { ++cancelledCalls; });
        QVERIFY(id != 0);
        client.cancel(id);
        client.cancel(id); // Cancelling twice is a no-op.

        // The next fetch finishes after the cancelled reply, so its callback proves the cancelled one was drained.
        const Fetched later = fetchAndWait(client, u"https://artist.bandcamp.com/track/later"_s);
        QVERIFY(later.response.has_value());
        QCOMPARE(cancelledCalls, 0);
    }

    void destroyingClientDropsPendingCallbacks()
    {
        FakeNetworkManager network;
        int calls = 0;
        {
            BandcampClient client{&network};
            client.fetch(AlbumUrl, [&calls](BandcampFetchResponse) { ++calls; });
        }

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(calls, 0);
    }

    void traceEventsShareCorrelationAndStayRedacted()
    {
        LogCapture capture(u"fy.trace.bandcamp.*.debug=true"_s);

        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.status = 200, .body = "ok"}; });
        BandcampClient client{&network};

        CorrelationContext context;
        context.jobId       = 7;
        context.resourceUrl = AlbumUrl;
        const Fetched fetched = fetchAndWait(client, AlbumUrl, context);
        QVERIFY(fetched.response.has_value());
        QCOMPARE(fetched.id, BandcampClient::RequestId{1});

        const QStringList started  = traceLines(u"request.started"_s);
        const QStringList finished = traceLines(u"request.finished"_s);
        QCOMPARE(started.size(), 1);
        QCOMPARE(finished.size(), 1);

        for(const QString& line : {started.first(), finished.first()}) {
            QVERIFY(line.contains(u"job=\"7\""_s));
            QVERIFY(line.contains(u"request=\"1\""_s));
            QVERIFY(!line.contains(u"SECRET"_s));
            QVERIFY(!line.contains(u"fragment"_s));
            QVERIFY(!line.contains(u'\n'));
        }
        QVERIFY(finished.first().contains(u"status=\"200\""_s));

        for(const CapturedLog& log : capturedLogs()) {
            QVERIFY(!log.message.contains(u"SECRET"_s));
        }
    }

    void traceRecordsCancellation()
    {
        LogCapture capture(u"fy.trace.bandcamp.*.debug=true"_s);

        FakeNetworkManager network;
        BandcampClient client{&network};

        CorrelationContext context;
        context.jobId       = 9;
        const auto id       = client.fetch(AlbumUrl, [](BandcampFetchResponse) {}, context);
        client.cancel(id);

        const QStringList cancelled = traceLines(u"request.cancelled"_s);
        QCOMPARE(cancelled.size(), 1);
        QVERIFY(cancelled.first().contains(u"job=\"9\""_s));
        QVERIFY(cancelled.first().contains(u"request=\"1\""_s));
    }
};

int runClientTests(int argc, char** argv)
{
    ClientTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampclienttests.moc"
