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
#include <stdexcept>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
const QUrl AlbumUrl{u"https://artist.bandcamp.com/album/record?token=SECRET#fragment"_s};

struct Fetched
{
    BandcampClient::RequestId id{0};
    std::optional<BandcampFetchOutcome> response;
};

// Runs a fetch and spins the event loop until it calls back. The response stays
// empty on timeout.
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
        [&fetched, &loop](BandcampFetchOutcome response) {
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
        network.setResponder(
            [](const QNetworkRequest&) { return FakeResponse{.status = 200, .body = "<html>ok</html>"}; });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampFetchResponse>>(*fetched.response));
        const auto& response = std::get<ResolutionSuccess<BandcampFetchResponse>>(*fetched.response).value;
        QCOMPARE(response.status, 200);
        QCOMPARE(response.body, QByteArray("<html>ok</html>"));
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
        const auto* failure = std::get_if<BandcampFetchFailure>(&*fetched.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::FetchFailed);
        QCOMPARE(failure->error.field, u"httpStatus"_s);
        QCOMPARE(failure->error.reason, u"HTTP 404"_s);
    }

    void transportErrorIsReported()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{
                .status = 0, .error = QNetworkReply::ConnectionRefusedError, .errorText = u"connection refused"_s};
        });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        const auto* failure = std::get_if<BandcampFetchFailure>(&*fetched.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::FetchFailed);
        QCOMPARE(failure->error.reason, u"no HTTP response received"_s);
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
        const auto* failure = std::get_if<BandcampFetchFailure>(&*fetched.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::PageTooLarge);
    }

    void completionOnlyReadsEnforceExactByteBoundary()
    {
        FakeNetworkManager exactNetwork;
        exactNetwork.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 200, .body = QByteArray(MaxPageBytes, 'x'), .finalOnly = true};
        });
        BandcampClient exactClient{&exactNetwork};
        const Fetched exact = fetchAndWait(exactClient, AlbumUrl);
        QVERIFY(exact.response.has_value());
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampFetchResponse>>(*exact.response));
        QCOMPARE(std::get<ResolutionSuccess<BandcampFetchResponse>>(*exact.response).value.body.size(), MaxPageBytes);
        QVERIFY(exactNetwork.largestReadRequest() <= 64 * 1024);

        FakeNetworkManager overNetwork;
        overNetwork.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 200, .body = QByteArray(MaxPageBytes + 1, 'x'), .finalOnly = true};
        });
        BandcampClient overClient{&overNetwork};
        const Fetched over = fetchAndWait(overClient, AlbumUrl);
        QVERIFY(over.response.has_value());
        const auto* failure = std::get_if<BandcampFetchFailure>(&*over.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::PageTooLarge);
        QVERIFY(overNetwork.largestReadRequest() <= 64 * 1024);
    }

    void chunkedResponseIsDrainedIncrementally()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status          = 200,
                                .chunks          = {QByteArray("first"), QByteArray("-"), QByteArray("last")},
                                .chunkIntervalMs = 2};
        });
        BandcampClient client{&network};
        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampFetchResponse>>(*fetched.response));
        QCOMPARE(std::get<ResolutionSuccess<BandcampFetchResponse>>(*fetched.response).value.body,
                 QByteArray("first-last"));
    }

    void redirectsUseEffectiveUrlAndRejectHttpsDowngrade()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().path() == u"/album/record"_s) {
                return FakeResponse{.status = 302, .redirectTarget = QUrl(u"/moved/record"_s)};
            }
            return FakeResponse{.status = 200, .body = "ok"};
        });
        BandcampClient client{&network};
        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        const auto* success = std::get_if<ResolutionSuccess<BandcampFetchResponse>>(&*fetched.response);
        QVERIFY(success);
        QCOMPARE(success->value.url, QUrl(u"https://artist.bandcamp.com/moved/record"_s));
        QCOMPARE(network.requested.size(), 2);

        FakeNetworkManager downgradeNetwork;
        downgradeNetwork.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 302, .redirectTarget = QUrl(u"http://artist.bandcamp.com/plain"_s)};
        });
        BandcampClient downgradeClient{&downgradeNetwork};
        const Fetched rejected = fetchAndWait(downgradeClient, AlbumUrl);
        QVERIFY(rejected.response.has_value());
        const auto* failure = std::get_if<BandcampFetchFailure>(&*rejected.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::UnsupportedRequest);
        QCOMPARE(downgradeNetwork.requested.size(), 1);
    }

    void cancelledRedirectDoesNotStartAnotherRequest()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{
                .status = 302, .redirectTarget = QUrl(u"/redirected"_s), .holdDelivery = true, .holdAfterAbort = true};
        });
        BandcampClient client{&network};
        int calls      = 0;
        bool cancelled = false;
        const auto id  = client.fetch(AlbumUrl, [&calls, &cancelled](BandcampFetchOutcome outcome) {
            ++calls;
            cancelled = std::holds_alternative<ResolutionCancelled>(outcome);
        });
        QTRY_COMPARE(network.liveReplyCount(), 1);

        client.cancel(id);
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
        network.finishAbortedReplies();
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);

        QVERIFY(cancelled);
        QCOMPARE(network.requested.size(), 1);
    }

    void oversizedRedirectDoesNotStartAnotherRequest()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status         = 302,
                                .body           = QByteArray(MaxPageBytes + 1, 'x'),
                                .redirectTarget = QUrl(u"/redirected"_s),
                                .finalOnly      = true};
        });
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, AlbumUrl);
        QVERIFY(fetched.response.has_value());
        const auto* failure = std::get_if<BandcampFetchFailure>(&*fetched.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::PageTooLarge);
        QCOMPARE(network.requested.size(), 1);
    }

    void startFailureCompletesAsynchronously()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) -> FakeResponse { throw std::runtime_error("start failed"); });
        BandcampClient client{&network};
        int calls   = 0;
        bool failed = false;
        client.fetch(AlbumUrl, [&calls, &failed](BandcampFetchOutcome outcome) {
            ++calls;
            failed = std::holds_alternative<BandcampFetchFailure>(outcome);
        });

        QCOMPARE(calls, 0);
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
        QVERIFY(failed);
    }

    void unsupportedSchemeIsRefusedWithoutRequest()
    {
        FakeNetworkManager network;
        BandcampClient client{&network};

        const Fetched fetched = fetchAndWait(client, QUrl(u"ftp://artist.bandcamp.com/album"_s));
        QVERIFY(fetched.id != 0);
        QVERIFY(fetched.response.has_value());
        const auto* failure = std::get_if<BandcampFetchFailure>(&*fetched.response);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::UnsupportedRequest);
        QVERIFY(network.requested.isEmpty());
    }

    void cancellationSettlesOnlyAfterPhysicalReplyFinishes()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            return FakeResponse{.status = 200, .body = request.url().path().toUtf8()};
        });
        BandcampClient client{&network};

        int cancelledCalls        = 0;
        bool cancellationReported = false;
        const auto id = client.fetch(AlbumUrl, [&cancelledCalls, &cancellationReported](BandcampFetchOutcome outcome) {
            ++cancelledCalls;
            cancellationReported = std::holds_alternative<ResolutionCancelled>(outcome);
        });
        QVERIFY(id != 0);
        client.cancel(id);
        client.cancel(id); // Cancelling twice is a no-op.

        // The next fetch finishes after the cancelled reply, so its callback proves
        // the cancelled one was drained.
        const Fetched later = fetchAndWait(client, u"https://artist.bandcamp.com/track/later"_s);
        QVERIFY(later.response.has_value());
        QCOMPARE(cancelledCalls, 1);
        QVERIFY(cancellationReported);
    }

    void asynchronousAbortRetainsReplyUntilPhysicalSettlement()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 200, .body = "never delivered", .holdAfterAbort = true};
        });
        BandcampClient client{&network};
        int calls      = 0;
        bool cancelled = false;
        const auto id  = client.fetch(AlbumUrl, [&calls, &cancelled](BandcampFetchOutcome outcome) {
            ++calls;
            cancelled = std::holds_alternative<ResolutionCancelled>(outcome);
        });
        QTRY_COMPARE(network.liveReplyCount(), 1);
        client.cancel(id);
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
        QCOMPARE(network.liveReplyCount(), 1);
        network.finishAbortedReplies();
        QTRY_COMPARE(calls, 1);
        QVERIFY(cancelled);
        QTRY_COMPARE(network.liveReplyCount(), 0);
    }

    void replyReadBufferIsBounded()
    {
        FakeNetworkManager network;
        network.setResponder(
            [](const QNetworkRequest&) { return FakeResponse{.status = 200, .body = "held", .holdDelivery = true}; });
        BandcampClient client{&network};
        bool finished = false;
        client.fetch(AlbumUrl, [&finished](BandcampFetchOutcome) { finished = true; });
        QTRY_COMPARE(network.liveReplyCount(), 1);
        const auto replies = network.replies();
        QVERIFY(replies.first());
        QCOMPARE(replies.first()->readBufferSize(), qint64{64 * 1024});
        network.releaseHeldResponses();
        QTRY_VERIFY(finished);
    }

    void destroyedReplySettlesWithoutFinished()
    {
        FakeNetworkManager network;
        network.setResponder(
            [](const QNetworkRequest&) { return FakeResponse{.status = 200, .body = "pending", .delayMs = 1000}; });
        BandcampClient client{&network};
        int calls   = 0;
        bool failed = false;
        client.fetch(AlbumUrl, [&calls, &failed](BandcampFetchOutcome outcome) {
            ++calls;
            failed = std::holds_alternative<BandcampFetchFailure>(outcome);
        });
        QTRY_COMPARE(network.liveReplyCount(), 1);
        const auto replies = network.replies();
        QVERIFY(replies.first());
        delete replies.first();
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
        QVERIFY(failed);
        QCOMPARE(network.liveReplyCount(), 0);
    }

    void destroyedManagerSettlesPendingReplyExactlyOnce()
    {
        auto* network = new FakeNetworkManager;
        network->setResponder(
            [](const QNetworkRequest&) { return FakeResponse{.status = 200, .body = "pending", .delayMs = 1000}; });
        BandcampClient client{network};
        int calls   = 0;
        bool failed = false;
        client.fetch(AlbumUrl, [&calls, &failed](BandcampFetchOutcome outcome) {
            ++calls;
            failed = std::holds_alternative<BandcampFetchFailure>(outcome);
        });
        QTRY_COMPARE(network->liveReplyCount(), 1);
        delete network;
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
        QVERIFY(failed);
        QCoreApplication::processEvents();
        QCOMPARE(calls, 1);
    }

    void destroyingClientDropsPendingCallbacks()
    {
        FakeNetworkManager network;
        int calls = 0;
        {
            BandcampClient client{&network};
            client.fetch(AlbumUrl, [&calls](BandcampFetchOutcome) { ++calls; });
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
        context.jobId         = 7;
        context.resourceUrl   = AlbumUrl;
        const Fetched fetched = fetchAndWait(client, AlbumUrl, context);
        QVERIFY(fetched.response.has_value());
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampFetchResponse>>(*fetched.response));
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
        context.jobId = 9;
        const auto id = client.fetch(AlbumUrl, [](BandcampFetchOutcome) { }, context);
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
