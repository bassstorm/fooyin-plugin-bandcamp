#include "bandcampfakenetwork.h"
#include "bandcampresolver.h"
#include "bandcampservice.h"
#include "bandcamptestpages.h"
#include "bandcampurl.h"

#include <QEventLoop>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QString AlbumUrl{u"https://artist.bandcamp.com/album/parent"_s};
const QString TrackUrl{u"https://artist.bandcamp.com/track/selected"_s};
const QString StandaloneUrl{u"https://artist.bandcamp.com/track/solo"_s};
const QString SecondAlbumUrl{u"https://artist.bandcamp.com/album/second"_s};

const QByteArray AlbumDocument = albumPage(
    R"({"type":"album","id":40,"title":"Parent album","artist":"Parent artist","release_date":"2020-01-01","art_id":20})",
    {streamEntry(R"("id":7,"title":"Parent child title","artist":"Child artist","track_num":1,"duration":90)")});

const QByteArray MemberTrackDocument = tralbumPage(
    R"({"current":{"type":"track","id":7,"title":"Page title","artist":"Page artist","release_date":"2024-05-01","art_id":91,"album_id":40},"artist":"Fallback","album_url":"https://artist.bandcamp.com/album/parent","trackinfo":[{"id":7,"title":"Page title","artist":"Page artist","track_num":3,"duration":150,"file":{"mp3-128":"https://cdn.example.com/old-page.mp3?sig=old"}}]})");

const QByteArray StandaloneTrackDocument = tralbumPage(
    R"({"current":{"type":"track","id":9,"title":"Solo title","artist":"Solo artist","album_id":null,"art_id":92},"trackinfo":[{"id":9,"title":"Solo title","artist":"Solo artist","file":{"mp3-128":"https://cdn.example.com/solo.mp3?sig=solo"}}]})");

const QByteArray SecondAlbumDocument
    = albumPage(R"({"type":"album","id":41,"title":"Second album","artist":"Second artist"})",
                {streamEntry(R"("id":8,"title":"Second track","artist":"Second artist")")});

QByteArray discographyPage(const QByteArray& items, const QByteArray& navigation = {})
{
    QByteArray body = tralbumPage(R"({"url":"https://artist.bandcamp.com/music"})");
    body.replace("</html>", navigation + "<ol id=\"music-grid\">" + items + "</ol></html>");
    return body;
}

struct ServiceRun
{
    bool finished{false};
    BandcampOperationSummary summary;
    QList<BandcampScopeResult> results;
};

struct ResolverRun
{
    bool finished{false};
    QHash<QString, BandcampResolutionOutcome> outcomes;
};

ServiceRun resolve(BandcampService& service, const QList<BandcampPageUrl>& pages, OperationOptions options = {})
{
    ServiceRun run;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(7000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&service, &BandcampService::scopeResolved, &loop,
                     [&run](const BandcampScopeResult& result) { run.results.append(result); });
    QObject::connect(&service, &BandcampService::operationFinished, &loop,
                     [&run, &loop](const BandcampOperationSummary& summary) {
                         run.finished = true;
                         run.summary  = summary;
                         loop.quit();
                     });
    const BandcampSubmission submitted = service.submit(pages, options);
    if(std::holds_alternative<BandcampSubmissionRejected>(submitted)) {
        return run;
    }
    loop.exec();
    return run;
}

ResolverRun resolveRequests(FakeNetworkManager& network, QVector<BandcampReleaseRequest> requests,
                            OperationOptions options = {})
{
    ResolverRun run;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(7000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    BandcampPageParser parser
        = [](const QByteArray& body, const QUrl& url, CorrelationContext context, const std::atomic_bool* stop) {
              return parseReleasePage(body, url, std::move(context), stop);
          };
    auto workflowCapacity = std::make_shared<BandcampWorkflowSlots>();
    BandcampResolver resolver{
        &network, 1, std::move(requests), options, std::move(parser), std::move(workflowCapacity)};
    QObject::connect(
        &resolver, &BandcampResolver::keyedOutcomeReady, &loop,
        [&run](const QString& key, const BandcampResolutionOutcome& outcome) { run.outcomes.insert(key, outcome); });
    QObject::connect(&resolver, &BandcampResolver::completed, &loop,
                     [&run, &loop](const QHash<QString, BandcampResolutionOutcome>& outcomes) {
                         run.finished = true;
                         run.outcomes = outcomes;
                         loop.quit();
                     });

    if(resolver.start()) {
        return run;
    }
    if(!run.finished) {
        loop.exec();
    }
    return run;
}

BandcampPageUrl page(const QString& url)
{
    return parseBandcampPageUrl(url);
}
} // namespace

class BandcampServiceTests : public QObject
{
    Q_OBJECT

private slots:
    void rejectsNoncanonicalAndOversizedPageValues()
    {
        FakeNetworkManager network;
        BandcampService service{&network};

        const BandcampPageUrl noncanonical{
            BandcampPageKind::Album, u"https://artist.bandcamp.com/album/parent?token=secret"_s, {}};
        const BandcampPageUrl oversized{BandcampPageKind::Album, QString(9000, u'a'), {}};
        for(const BandcampPageUrl& invalid : {noncanonical, oversized}) {
            const BandcampSubmission submission = service.submit({invalid});
            QVERIFY(std::holds_alternative<BandcampSubmissionRejected>(submission));
            QCOMPARE(std::get<BandcampSubmissionRejected>(submission).error.code,
                     ResolutionErrorCode::UnsupportedRequest);
        }
        QVERIFY(network.requested.isEmpty());
    }

    void resolvesAlbumMemberAndStandaloneThroughOnePipeline()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            const QString path = request.url().path();
            if(path == u"/album/parent"_s) {
                return FakeResponse{.body = AlbumDocument};
            }
            if(path == u"/track/selected"_s) {
                return FakeResponse{.body = MemberTrackDocument};
            }
            if(path == u"/track/solo"_s) {
                return FakeResponse{.body = StandaloneTrackDocument};
            }
            return FakeResponse{.status = 404};
        });
        BandcampService service{&network};

        const ServiceRun run = resolve(service, {page(AlbumUrl), page(TrackUrl), page(AlbumUrl), page(StandaloneUrl)},
                                       OperationOptions{3, 10000});

        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 4);
        QCOMPARE(run.summary.succeeded, 4);
        QCOMPARE(run.summary.failed, 0);
        QCOMPARE(run.results.size(), 4);
        QCOMPARE(run.results.at(0).page.url, AlbumUrl);
        QCOMPARE(run.results.at(1).page.url, TrackUrl);
        QCOMPARE(run.results.at(2).page.url, AlbumUrl);
        QCOMPARE(run.results.at(3).page.url, StandaloneUrl);

        const auto& album = std::get<BandcampAlbum>(
            std::get<ResolutionSuccess<BandcampResolvedData>>(run.results.at(0).outcome).value);
        QCOMPARE(album.metadata.title, u"Parent album"_s);
        QCOMPARE(album.tracks.size(), 1);

        const auto& member = std::get<BandcampResolvedTrack>(
            std::get<ResolutionSuccess<BandcampResolvedData>>(run.results.at(1).outcome).value);
        QCOMPARE(member.track.title, u"Parent child title"_s);
        QCOMPARE(member.track.streamUrl, u"https://cdn.example.com/track.mp3"_s);
        QVERIFY(member.album.has_value());
        QCOMPARE(member.album->title, u"Parent album"_s);
        QCOMPARE(member.track.sourceUrl, AlbumUrl);

        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(run.results.at(2).outcome));
        const auto& standalone = std::get<BandcampResolvedTrack>(
            std::get<ResolutionSuccess<BandcampResolvedData>>(run.results.at(3).outcome).value);
        QCOMPARE(standalone.track.title, u"Solo title"_s);
        QVERIFY(!standalone.album.has_value());
        QCOMPARE(standalone.track.sourceUrl, StandaloneUrl);
        QCOMPARE(network.requested.size(), 4);
    }

    void serverErrorProducesOneOutcome()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.status = 503}; });
        BandcampService service{&network};

        const ServiceRun run = resolve(service, {page(AlbumUrl)});

        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 1);
        QCOMPARE(run.summary.failed, 1);
        QCOMPARE(run.results.size(), 1);
        const auto* failure = std::get_if<ResolutionFailure>(&run.results.first().outcome);
        QVERIFY(failure);
        QCOMPARE(failure->error.code, ResolutionErrorCode::FetchFailed);
        QCOMPARE(network.requested.size(), 1);
    }

    void cancellingFromResultDoesNotFinishBeforeOrderedResults()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().path() == u"/album/parent"_s) {
                return FakeResponse{.body = AlbumDocument, .delayMs = 100};
            }
            return FakeResponse{.body = SecondAlbumDocument};
        });
        BandcampService service{&network};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(5000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        BandcampOperationId operationId = 0;
        int delivered                   = 0;
        int deliveredAtCompletion       = -1;
        BandcampOperationSummary summary;
        QObject::connect(&service, &BandcampService::scopeResolved, &loop,
                         [&service, &operationId, &delivered](const BandcampScopeResult&) {
                             if(++delivered == 1) {
                                 service.cancel(operationId);
                             }
                         });
        QObject::connect(
            &service, &BandcampService::operationFinished, &loop,
            [&loop, &summary, &delivered, &deliveredAtCompletion](const BandcampOperationSummary& completed) {
                summary               = completed;
                deliveredAtCompletion = delivered;
                loop.quit();
            });

        const auto submission = service.submit({page(AlbumUrl), page(SecondAlbumUrl)}, OperationOptions{2, 10000});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(submission));
        operationId = std::get<BandcampOperationAccepted>(submission).id;
        loop.exec();

        QCOMPARE(deliveredAtCompletion, 2);
        QCOMPARE(summary.succeeded, 2);
        QCOMPARE(summary.failed, 0);
        QCOMPARE(summary.cancelled, 0);
    }

    void resolverCachesAdmittedResultsByPageIdentity()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.body = AlbumDocument}; });

        const ResolverRun run = resolveRequests(
            network, {{u"first-correlation"_s, page(AlbumUrl)}, {u"second-correlation"_s, page(AlbumUrl)}},
            OperationOptions{1, 10000});

        QVERIFY(run.finished);
        QCOMPARE(run.outcomes.size(), 2);
        const auto first  = run.outcomes.constFind(u"first-correlation"_s);
        const auto second = run.outcomes.constFind(u"second-correlation"_s);
        QVERIFY(first != run.outcomes.cend());
        QVERIFY(second != run.outcomes.cend());
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(first.value()));
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(second.value()));
        QCOMPARE(network.requested.size(), 1);
    }

    void resolverDoesNotCacheFailedResolutions()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.status = 503}; });

        const ResolverRun run
            = resolveRequests(network, {{u"first-failure"_s, page(AlbumUrl)}, {u"second-failure"_s, page(AlbumUrl)}},
                              OperationOptions{1, 10000});

        QVERIFY(run.finished);
        QCOMPARE(run.outcomes.size(), 2);
        const auto first  = run.outcomes.constFind(u"first-failure"_s);
        const auto second = run.outcomes.constFind(u"second-failure"_s);
        QVERIFY(first != run.outcomes.cend());
        QVERIFY(second != run.outcomes.cend());
        QVERIFY(std::holds_alternative<ResolutionFailure>(first.value()));
        QVERIFY(std::holds_alternative<ResolutionFailure>(second.value()));
        QCOMPARE(network.requested.size(), 2);
    }

    void resolverRejectsEmptyOrDuplicateKeysBeforeFetching()
    {
        const QVector<BandcampReleaseRequest> emptyKey{{QString{}, page(AlbumUrl)}};
        const QVector<BandcampReleaseRequest> duplicateKeys{{u"same-key"_s, page(AlbumUrl)},
                                                            {u"same-key"_s, page(SecondAlbumUrl)}};
        for(const QVector<BandcampReleaseRequest>& requests : {emptyKey, duplicateKeys}) {
            FakeNetworkManager network;
            auto workflowCapacity = std::make_shared<BandcampWorkflowSlots>();
            BandcampResolver resolver{
                &network,
                1,
                requests,
                OperationOptions{},
                [](const QByteArray& body, const QUrl& url, CorrelationContext context, const std::atomic_bool* stop) {
                    return parseReleasePage(body, url, std::move(context), stop);
                },
                std::move(workflowCapacity)};

            const std::optional<ResolutionError> rejection = resolver.start();
            QVERIFY(rejection.has_value());
            QCOMPARE(rejection->code, ResolutionErrorCode::UnsupportedRequest);
            QCOMPARE(rejection->field, u"requestKey"_s);
            QVERIFY(network.requested.isEmpty());
        }
    }

    void separateOperationsDoNotSharePageRequests()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.body = AlbumDocument}; });
        BandcampService service{&network};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(5000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        int finished = 0;
        QObject::connect(&service, &BandcampService::operationFinished, &loop, [&finished, &loop]() {
            ++finished;
            if(finished == 2) {
                loop.quit();
            }
        });

        const auto first  = service.submit({page(AlbumUrl)});
        const auto second = service.submit({page(AlbumUrl)});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(first));
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(second));
        loop.exec();

        QCOMPARE(finished, 2);
        QCOMPARE(network.requested.size(), 2);
    }

    void pendingScopesStartTheirDeadlineOnlyWhenAdmitted()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().path() == u"/album/parent"_s) {
                return FakeResponse{.body = AlbumDocument, .delayMs = 1400};
            }
            return FakeResponse{.body = SecondAlbumDocument};
        });
        BandcampService service{&network};

        const ServiceRun run = resolve(service, {page(AlbumUrl), page(SecondAlbumUrl)}, OperationOptions{1, 1000});

        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 2);
        QCOMPARE(run.summary.succeeded, 1);
        QCOMPARE(run.summary.failed, 1);
        QCOMPARE(run.results.size(), 2);
        QVERIFY(std::holds_alternative<ResolutionFailure>(run.results.at(0).outcome));
        QCOMPARE(std::get<ResolutionFailure>(run.results.at(0).outcome).error.code, ResolutionErrorCode::Timeout);
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(run.results.at(1).outcome));
        QCOMPARE(network.requested.size(), 2);
    }

    void moreThan256UniqueImmediateFetchFailuresComplete()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.status = 503}; });
        BandcampService service{&network};
        QList<BandcampPageUrl> pages;
        for(int i = 0; i < 300; ++i) {
            pages.append(page(QStringLiteral("https://artist-%1.bandcamp.com/album/failure-%1").arg(i)));
        }

        const ServiceRun run = resolve(service, pages, OperationOptions{8, 10000});

        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 300);
        QCOMPARE(run.summary.failed, 300);
        QCOMPARE(run.results.size(), 300);
        QCOMPARE(network.requested.size(), 300);
        for(const BandcampScopeResult& result : run.results) {
            const auto* failure = std::get_if<ResolutionFailure>(&result.outcome);
            QVERIFY(failure);
            QCOMPARE(failure->error.code, ResolutionErrorCode::FetchFailed);
        }
    }

    void deletingServiceFromFirstSuccessIsSafe()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.body = AlbumDocument}; });
        auto* service = new BandcampService{&network};
        QPointer<BandcampService> guard{service};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(7000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        int resolved = 0;
        bool success = false;
        QObject::connect(service, &BandcampService::scopeResolved, &loop,
                         [&service, &loop, &resolved, &success](const BandcampScopeResult& result) {
                             ++resolved;
                             success = std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(result.outcome);
                             delete service;
                             service = nullptr;
                             loop.quit();
                         });

        const BandcampSubmission submission = service->submit({page(AlbumUrl)});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(submission));
        loop.exec();

        if(service) {
            delete service;
            service = nullptr;
        }
        QVERIFY(guard.isNull());
        QCOMPARE(resolved, 1);
        QVERIFY(success);
    }

    void deletingServiceFromDiscoveryFailureIsSafe()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().query().isEmpty()) {
                return FakeResponse{
                    .body = discographyPage(R"(<li data-item-id="album-40"><a href="/album/parent"></a></li>)",
                                            R"(<link rel="next" href="?page=2">)")};
            }
            return FakeResponse{.status = 404};
        });
        auto* service = new BandcampService{&network};
        QPointer<BandcampService> guard{service};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(7000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        int resolved = 0;
        bool failed  = false;
        QObject::connect(service, &BandcampService::scopeResolved, &loop,
                         [&service, &loop, &resolved, &failed](const BandcampScopeResult& result) {
                             ++resolved;
                             failed = result.page.kind == BandcampPageKind::Discography
                                   && std::holds_alternative<ResolutionFailure>(result.outcome);
                             delete service;
                             service = nullptr;
                             loop.quit();
                         });

        const BandcampSubmission submission = service->submit({page(u"https://artist.bandcamp.com/music"_s)});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(submission));
        loop.exec();

        if(service) {
            delete service;
            service = nullptr;
        }
        QVERIFY(guard.isNull());
        QCOMPARE(resolved, 1);
        QVERIFY(failed);
        QCOMPARE(network.requested.size(), 2);
    }

    void deletingServiceFromActiveCancellationIsSafe()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) {
            return FakeResponse{.status = 200, .body = "pending", .holdDelivery = true, .holdAfterAbort = true};
        });
        auto* service = new BandcampService{&network};
        QPointer<BandcampService> guard{service};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(7000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        BandcampOperationId operationId = 0;
        bool cancelScheduled            = false;
        int resolved                    = 0;
        bool cancelled                  = false;
        QObject::connect(service, &BandcampService::scopeResolved, &loop,
                         [&service, &loop, &resolved, &cancelled](const BandcampScopeResult& result) {
                             ++resolved;
                             cancelled = std::holds_alternative<ResolutionCancelled>(result.outcome);
                             delete service;
                             service = nullptr;
                             loop.quit();
                         });
        QObject::connect(service, &BandcampService::progressChanged, &loop,
                         [&loop, &service, &operationId, &cancelScheduled](const BandcampOperationProgress& state) {
                             if(state.active == 0 || cancelScheduled) {
                                 return;
                             }
                             cancelScheduled = true;
                             QTimer::singleShot(0, &loop, [&service, &operationId]() {
                                 if(service) {
                                     service->cancel(operationId);
                                 }
                             });
                         });

        const BandcampSubmission submission = service->submit({page(AlbumUrl)}, OperationOptions{1, 10000});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(submission));
        operationId = std::get<BandcampOperationAccepted>(submission).id;
        loop.exec();

        if(service) {
            delete service;
            service = nullptr;
        }
        QVERIFY(guard.isNull());
        QVERIFY(cancelScheduled);
        QCOMPARE(resolved, 1);
        QVERIFY(cancelled);
        QCOMPARE(network.requested.size(), 1);
    }

    void longListsAreAcceptedAndCancellationKeepsPhysicalRepliesOwned()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().path() == u"/album/second"_s) {
                return FakeResponse{.body = SecondAlbumDocument};
            }
            return FakeResponse{.holdDelivery = true, .holdAfterAbort = true};
        });
        BandcampService service{&network};
        QList<BandcampPageUrl> pages;
        for(int i = 0; i < 300; ++i) {
            pages.append(page(QStringLiteral("https://artist.bandcamp.com/album/release-%1").arg(i)));
        }

        ServiceRun run;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(5000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&service, &BandcampService::scopeResolved, &loop,
                         [&run](const BandcampScopeResult& result) { run.results.append(result); });
        QObject::connect(&service, &BandcampService::operationFinished, &loop,
                         [&run, &loop](const BandcampOperationSummary& summary) {
                             run.finished = true;
                             run.summary  = summary;
                             loop.quit();
                         });
        const auto submission = service.submit(pages, OperationOptions{8, 10000});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(submission));
        const BandcampOperationId operationId = std::get<BandcampOperationAccepted>(submission).id;
        QPointer<BandcampResolver> resolverGuard{service.findChild<BandcampResolver*>()};
        QVERIFY(resolverGuard);
        QTimer::singleShot(1, &service, [&service, operationId]() { service.cancel(operationId); });
        loop.exec();

        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 300);
        QCOMPARE(run.summary.cancelled, 300);
        QCOMPARE(run.results.size(), 300);
        QCOMPARE(network.requested.size(), 8);
        QVERIFY(network.liveReplyCount() > 0);

        const auto waiting = service.submit({page(SecondAlbumUrl)}, OperationOptions{1, 10000});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(waiting));
        const BandcampOperationId waitingId = std::get<BandcampOperationAccepted>(waiting).id;
        bool waitingFinished                = false;
        BandcampOperationSummary waitingSummary;
        QEventLoop waitingLoop;
        QTimer waitingTimeout;
        waitingTimeout.setSingleShot(true);
        waitingTimeout.start(7000);
        QObject::connect(&waitingTimeout, &QTimer::timeout, &waitingLoop, &QEventLoop::quit);
        QObject::connect(
            &service, &BandcampService::operationFinished, &waitingLoop,
            [&waitingLoop, &waitingFinished, &waitingSummary, waitingId](const BandcampOperationSummary& summary) {
                if(summary.operationId == waitingId) {
                    waitingFinished = true;
                    waitingSummary  = summary;
                    waitingLoop.quit();
                }
            });
        QTest::qWait(20);
        QCOMPARE(network.requested.size(), 8);
        network.finishAbortedReplies();
        waitingLoop.exec();

        QVERIFY(waitingFinished);
        QCOMPARE(waitingSummary.succeeded, 1);
        QCOMPARE(network.requested.size(), 9);
        QTRY_VERIFY_WITH_TIMEOUT(resolverGuard.isNull(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(network.liveReplyCount(), 0, 2000);
    }

    void discoversArtistAndLabelListingsAndSortsEachCollectionIndependently()
    {
        const QByteArray firstListing = discographyPage(
            R"(<li data-item-id="album-40"><a href="/album/parent"></a></li>)", R"(<link rel="next" href="?page=2">)");
        const QByteArray secondListing
            = discographyPage(R"(<li data-item-id="album-41"><a href="/album/second"></a></li>)");
        const QByteArray labelListing
            = discographyPage(R"(<li data-item-id="album-42"><a href="/album/label"></a></li>)");
        const QByteArray datedParent = albumPage(
            R"({"type":"album","id":40,"title":"Parent album","artist":"Parent artist","release_date":"2020-01-01"})",
            {streamEntry(
                R"("id":7,"title":"Parent child title","artist":"Child artist","track_num":1,"duration":90)")});
        const QByteArray datedSecond = albumPage(
            R"({"type":"album","id":41,"title":"Second album","artist":"Second artist","release_date":"2018-01-01"})",
            {streamEntry(R"("id":8,"title":"Second track","artist":"Second artist")")});
        const QByteArray datedLabel = albumPage(
            R"({"type":"album","id":42,"title":"Label album","artist":"Label artist","release_date":"1990-01-01"})",
            {streamEntry(R"("id":10,"title":"Label track","artist":"Label artist")")});

        FakeNetworkManager network;
        network.setResponder([&](const QNetworkRequest& request) {
            if(request.url().host() == u"label.bandcamp.com"_s && request.url().path() == u"/music"_s) {
                return FakeResponse{.body = labelListing};
            }
            if(request.url().host() == u"artist.bandcamp.com"_s && request.url().path() == u"/music"_s) {
                return request.url().query() == u"page=2"_s ? FakeResponse{.body = secondListing}
                                                            : FakeResponse{.body = firstListing};
            }
            if(request.url().path() == u"/album/parent"_s) {
                return FakeResponse{.body = datedParent};
            }
            if(request.url().path() == u"/album/second"_s) {
                return FakeResponse{.body = datedSecond};
            }
            if(request.url().path() == u"/album/label"_s) {
                return FakeResponse{.body = datedLabel};
            }
            return FakeResponse{.status = 404};
        });
        BandcampService service{&network};
        OperationOptions options;
        options.parallelism      = 2;
        options.discographyOrder = BandcampDiscographyOrder::Ascending;

        const ServiceRun run = resolve(
            service, {page(u"https://artist.bandcamp.com"_s), page(u"https://label.bandcamp.com"_s)}, options);
        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 3);
        QCOMPARE(run.summary.succeeded, 3);
        QCOMPARE(run.summary.failed, 0);
        QCOMPARE(run.results.size(), 3);
        QCOMPARE(run.results.at(0).page.url, u"https://artist.bandcamp.com/album/second"_s);
        QCOMPARE(run.results.at(1).page.url, u"https://artist.bandcamp.com/album/parent"_s);
        QCOMPARE(run.results.at(2).page.url, u"https://label.bandcamp.com/album/label"_s);
        QCOMPARE(network.requested.size(), 6);
        QCOMPARE(network.requested.at(0).path(), u"/music"_s);
        QCOMPARE(network.requested.at(1).query(), u"page=2"_s);
    }

    void accumulatesMultipleListingPagesBeyondThePerPageValueLimit()
    {
        const BandcampPageUrl release = page(AlbumUrl);
        BandcampServiceDependencies dependencies;
        dependencies.parseReleasePage = [](const QByteArray&, const QUrl& url, CorrelationContext,
                                           const std::atomic_bool*) -> ReleaseParseOutcome {
            return ResolutionFailure{ResolutionError{ResolutionErrorCode::MalformedPage, ResolutionStage::Parse, url, 0,
                                                     QStringLiteral("fixture"),
                                                     QStringLiteral("synthetic release parse failure")}};
        };
        dependencies.parseDiscographyPage = [release](const QByteArray&, const QUrl& url, CorrelationContext,
                                                      const std::atomic_bool*) -> BandcampListingParseOutcome {
            BandcampListingBatch batch;
            batch.effectiveUrl = url;
            if(url.query().isEmpty()) {
                batch.references.reserve(MaxSourceRecords);
                for(qsizetype i = 0; i < MaxSourceRecords; ++i) {
                    batch.references.append({i, 40, release});
                }
                batch.continuation = BandcampListingContinuation{url.resolved(QUrl(u"?page=2"_s, QUrl::StrictMode))};
            }
            else {
                batch.references.append({0, 40, release});
            }
            return ResolutionSuccess<BandcampListingBatch>{std::move(batch)};
        };

        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.body = "listing"}; });
        BandcampService service{&network, nullptr, std::move(dependencies)};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(20000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        qsizetype delivered = 0;
        bool allFailed      = true;
        BandcampOperationSummary summary;
        bool finished = false;
        QObject::connect(&service, &BandcampService::scopeResolved, &loop,
                         [&delivered, &allFailed](const auto& result) {
                             ++delivered;
                             allFailed = allFailed && std::holds_alternative<ResolutionFailure>(result.outcome);
                         });
        QObject::connect(&service, &BandcampService::operationFinished, &loop,
                         [&summary, &finished, &loop](const BandcampOperationSummary& value) {
                             summary  = value;
                             finished = true;
                             loop.quit();
                         });
        const BandcampSubmission submission = service.submit({page(u"https://artist.bandcamp.com/music"_s)});
        if(!std::holds_alternative<BandcampOperationAccepted>(submission)) {
            QFAIL("large discography submission was rejected");
        }

        QLoggingCategory::setFilterRules(u"fy.bandcamp.*=false\nfy.trace.bandcamp.*=false"_s);
        loop.exec();
        QLoggingCategory::setFilterRules({});

        QVERIFY(finished);
        QCOMPARE(summary.requested, MaxSourceRecords + 1);
        QCOMPARE(summary.failed, MaxSourceRecords + 1);
        QCOMPARE(delivered, MaxSourceRecords + 1);
        QVERIFY(allFailed);
        QCOMPARE(network.requested.size(), 3);
        QCOMPARE(network.requested.at(0).path(), u"/music"_s);
        QCOMPARE(network.requested.at(1).query(), u"page=2"_s);
        QCOMPARE(network.requested.at(2).path(), u"/album/parent"_s);
    }

    void repeatedListingContinuationFailsCollectionWithoutResolvingPartialEntries()
    {
        const QByteArray firstListing = discographyPage(
            R"(<li data-item-id="album-40"><a href="/album/parent"></a></li>)", R"(<link rel="next" href="?page=2">)");
        const QByteArray repeatedListing = discographyPage(
            R"(<li data-item-id="album-41"><a href="/album/second"></a></li>)", R"(<link rel="next" href="?page=2">)");
        FakeNetworkManager network;
        network.setResponder([&](const QNetworkRequest& request) {
            if(request.url().path() == u"/music"_s) {
                return request.url().query() == u"page=2"_s ? FakeResponse{.body = repeatedListing}
                                                            : FakeResponse{.body = firstListing};
            }
            return FakeResponse{.body = AlbumDocument};
        });
        BandcampService service{&network};

        const ServiceRun run = resolve(service, {page(u"https://artist.bandcamp.com/music"_s)});
        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 1);
        QCOMPARE(run.summary.succeeded, 0);
        QCOMPARE(run.summary.failed, 1);
        QCOMPARE(run.results.size(), 1);
        QCOMPARE(run.results.first().page.kind, BandcampPageKind::Discography);
        QVERIFY(std::holds_alternative<ResolutionFailure>(run.results.first().outcome));
        QCOMPARE(network.requested.size(), 2);
    }

    void malformedDiscoveryDoesNotSuppressIndependentInputs()
    {
        FakeNetworkManager network;
        network.setResponder([](const QNetworkRequest& request) {
            if(request.url().path() == u"/music"_s) {
                return FakeResponse{.body = discographyPage(R"(<li><a href="/album/parent"></a></li>)")};
            }
            if(request.url().path() == u"/album/parent"_s) {
                return FakeResponse{.body = AlbumDocument};
            }
            return FakeResponse{.status = 404};
        });
        BandcampService service{&network};

        const ServiceRun run = resolve(service, {page(u"https://artist.bandcamp.com/music"_s), page(AlbumUrl)});
        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 2);
        QCOMPARE(run.summary.succeeded, 1);
        QCOMPARE(run.summary.failed, 1);
        QCOMPARE(run.results.size(), 2);
        QCOMPARE(run.results.at(0).page.kind, BandcampPageKind::Discography);
        QCOMPARE(run.results.at(1).page.url, AlbumUrl);
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(run.results.at(1).outcome));
        QCOMPARE(network.requested.size(), 2);
    }

    void listingCancellationReportsPromptlyAndRetainsReplyUntilSettlement()
    {
        FakeNetworkManager network;
        network.setResponder(
            [](const QNetworkRequest&) { return FakeResponse{.holdDelivery = true, .holdAfterAbort = true}; });
        BandcampService service{&network};
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(5000);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        bool finished       = false;
        qsizetype cancelled = 0;
        QObject::connect(&service, &BandcampService::scopeResolved, &loop, [&cancelled](const auto& result) {
            if(std::holds_alternative<ResolutionCancelled>(result.outcome)) {
                ++cancelled;
            }
        });
        BandcampOperationSummary summary;
        QObject::connect(&service, &BandcampService::operationFinished, &loop,
                         [&finished, &summary, &loop](const BandcampOperationSummary& value) {
                             finished = true;
                             summary  = value;
                             loop.quit();
                         });
        const auto submission = service.submit({page(u"https://artist.bandcamp.com/music"_s)});
        QVERIFY(std::holds_alternative<BandcampOperationAccepted>(submission));
        const BandcampOperationId id = std::get<BandcampOperationAccepted>(submission).id;
        QTimer::singleShot(10, &service, [&service, id]() { service.cancel(id); });
        loop.exec();

        QVERIFY(finished);
        QCOMPARE(summary.cancelled, 1);
        QCOMPARE(cancelled, 1);
        QCOMPARE(network.requested.size(), 1);
        QCOMPARE(network.liveReplyCount(), 1);
        auto* listingResolver = service.findChild<BandcampListingResolver*>();
        QVERIFY(listingResolver);
        QPointer<BandcampListingResolver> listingGuard{listingResolver};
        QPointer<QObject> operationGuard{listingResolver->parent()};
        network.finishAbortedReplies();
        QTRY_COMPARE_WITH_TIMEOUT(network.liveReplyCount(), 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(listingGuard.isNull(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(operationGuard.isNull(), 2000);

        network.setResponder([](const QNetworkRequest&) { return FakeResponse{.body = AlbumDocument}; });
        const ServiceRun next = resolve(service, {page(AlbumUrl)});
        QVERIFY(next.finished);
        QCOMPARE(next.summary.succeeded, 1);
        QCOMPARE(next.summary.failed, 0);
        QCOMPARE(network.requested.size(), 2);
    }
};

int runServiceTests(int argc, char** argv)
{
    BandcampServiceTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampservicetests.moc"
