#include "bandcampfakenetwork.h"
#include "bandcampresolver.h"
#include "bandcampservice.h"
#include "bandcamptestpages.h"
#include "bandcampurl.h"

#include <QEventLoop>
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

    void moreThan256UniqueImmediateFailuresComplete()
    {
        FakeNetworkManager network;
        BandcampService service{&network};
        QList<BandcampPageUrl> pages;
        for(int i = 0; i < 300; ++i) {
            pages.append(page(QStringLiteral("https://artist-%1.bandcamp.com/music").arg(i)));
        }

        const ServiceRun run = resolve(service, pages, OperationOptions{8, 10000});

        QVERIFY(run.finished);
        QCOMPARE(run.summary.requested, 300);
        QCOMPARE(run.summary.failed, 300);
        QCOMPARE(run.results.size(), 300);
        QCOMPARE(network.requested.size(), 0);
        for(const BandcampScopeResult& result : run.results) {
            const auto* failure = std::get_if<ResolutionFailure>(&result.outcome);
            QVERIFY(failure);
            QCOMPARE(failure->error.code, ResolutionErrorCode::UnsupportedRequest);
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
};

int runServiceTests(int argc, char** argv)
{
    BandcampServiceTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampservicetests.moc"
