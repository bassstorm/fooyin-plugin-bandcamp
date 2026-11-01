#include "bandcamporchestrator.h"
#include "bandcampurl.h"

#include <QtTest>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
BandcampPageUrl page(const QString& url)
{
    return parseBandcampPageUrl(url);
}

BandcampResolutionOutcome failure(const QString& reason)
{
    return ResolutionFailure{ResolutionError{
        ResolutionErrorCode::FetchFailed, ResolutionStage::Fetch, {}, 0, QStringLiteral("request"), reason}};
}

BandcampListingReference reference(const QString& url, qint64 id, qsizetype ordinal)
{
    return {ordinal, id, page(url)};
}

BandcampResolutionOutcome albumOutcome(qint64 id, QDate date)
{
    BandcampAlbum album;
    album.metadata.id          = id;
    album.metadata.releaseDate = date;
    return ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(album)}};
}

BandcampResolutionOutcome memberOutcome(qint64 id, QDate trackDate, QDate albumDate)
{
    BandcampResolvedTrack track;
    track.track.id          = id;
    track.track.releaseDate = trackDate;
    BandcampAlbumMetadata album;
    album.id          = id + 1000;
    album.releaseDate = albumDate;
    track.album       = album;
    return ResolutionSuccess<BandcampResolvedData>{BandcampResolvedData{std::move(track)}};
}
} // namespace

class BandcampOrchestratorTests : public QObject
{
    Q_OBJECT

private slots:
    void deduplicatesWorkAndRestoresOccurrencesInInputOrder()
    {
        const BandcampPageUrl album = page(u"https://artist.bandcamp.com/album/release"_s);
        const BandcampPageUrl track = page(u"https://artist.bandcamp.com/track/song"_s);
        BandcampOrchestrator orchestrator{{album, track, album}};

        QCOMPARE(orchestrator.requests().size(), 2);
        QCOMPARE(orchestrator.occurrenceCount(), 3);
        QVERIFY(!orchestrator.isComplete());

        const QString trackKey = orchestrator.requests().at(1).key;
        QVERIFY(orchestrator.associate(trackKey, failure(u"track failed"_s)).isEmpty());
        QCOMPARE(orchestrator.deliveredCount(), 0);

        const QString albumKey = orchestrator.requests().at(0).key;
        const auto ready       = orchestrator.associate(albumKey, failure(u"album failed"_s));
        QCOMPARE(ready.size(), 3);
        QCOMPARE(ready.at(0).ordinal, 0);
        QCOMPARE(ready.at(0).page.url, album.url);
        QCOMPARE(ready.at(1).ordinal, 1);
        QCOMPARE(ready.at(1).page.url, track.url);
        QCOMPARE(ready.at(2).ordinal, 2);
        QCOMPARE(ready.at(2).page.url, album.url);
        QCOMPARE(orchestrator.deliveredCount(), 3);
        QVERIFY(orchestrator.isComplete());
    }

    void acceptsCompleteListsLargerThanLegacyScopeCaps()
    {
        QList<BandcampPageUrl> pages;
        for(int i = 0; i < 300; ++i) {
            pages.append(page(QStringLiteral("https://artist.bandcamp.com/album/release-%1").arg(i)));
        }

        const BandcampOrchestrator orchestrator{pages};

        QCOMPARE(orchestrator.requests().size(), 300);
        QCOMPARE(orchestrator.occurrenceCount(), 300);
    }

    void progressCountsFollowDiscoveryFailureAndCancellation()
    {
        const BandcampPageUrl artist = page(u"https://artist.bandcamp.com/music"_s);
        const BandcampPageUrl label  = page(u"https://label.bandcamp.com/music"_s);
        const BandcampPageUrl direct = page(u"https://artist.bandcamp.com/album/direct"_s);
        BandcampOrchestrator orchestrator{{artist, direct, label}};
        QCOMPARE(orchestrator.occurrenceCount(), 3);
        QVERIFY(!orchestrator.isDiscoveryComplete());

        BandcampListingBatch batch;
        batch.references = {reference(u"https://artist.bandcamp.com/album/one"_s, 1, 0),
                            reference(u"https://artist.bandcamp.com/album/two"_s, 2, 1)};
        orchestrator.appendListingBatch(0, batch);
        QCOMPARE(orchestrator.occurrenceCount(), 4);
        orchestrator.failDiscography(0, failure(u"continuation failed"_s));
        QCOMPARE(orchestrator.occurrenceCount(), 3);
        QVERIFY(!orchestrator.isDiscoveryComplete());

        orchestrator.appendListingBatch(2, batch);
        QCOMPARE(orchestrator.occurrenceCount(), 4);
        orchestrator.cancelPending();
        QCOMPARE(orchestrator.occurrenceCount(), 3);
        QCOMPARE(orchestrator.deliveredCount(), 3);
        QVERIFY(orchestrator.isDiscoveryComplete());
        QVERIFY(orchestrator.isComplete());
        QVERIFY(orchestrator.cancelPending().isEmpty());
        QCOMPARE(orchestrator.occurrenceCount(), 3);
    }

    void ordersLargeChronologicalCollectionAfterSingleReadinessPass()
    {
        constexpr qsizetype Count         = 10000;
        const BandcampPageUrl discography = page(u"https://artist.bandcamp.com/music"_s);
        BandcampListingBatch batch;
        batch.references.reserve(Count);
        for(qsizetype i = 0; i < Count; ++i) {
            const BandcampPageUrl release = page(u"https://artist.bandcamp.com/album/large-%1"_s.arg(i));
            batch.references.append({i, i + 1, release});
        }

        BandcampOrchestrator orchestrator{{discography}, BandcampDiscographyOrder::Ascending};
        orchestrator.appendListingBatch(0, batch);
        orchestrator.completeDiscography(0);
        QCOMPARE(orchestrator.requests().size(), Count);

        QVector<BandcampAssociatedOutcome> ready;
        for(qsizetype i = 0; i < Count; ++i) {
            const auto newlyReady = orchestrator.associate(orchestrator.requests().at(i).key,
                                                           albumOutcome(i + 1, QDate(2020, 1, 1).addDays(Count - i)));
            if(!newlyReady.isEmpty()) {
                ready = newlyReady;
            }
        }

        QCOMPARE(ready.size(), Count);
        QCOMPARE(ready.first().page.url, batch.references.last().page.url);
        QCOMPARE(ready.last().page.url, batch.references.first().page.url);
        QCOMPARE(orchestrator.deliveredCount(), Count);
        QVERIFY(orchestrator.isComplete());
    }

    void chronologicalCollectionsSortKnownDatesAndKeepUnknownsAndFailuresLast()
    {
        const BandcampPageUrl discography = page(u"https://artist.bandcamp.com"_s);
        const BandcampPageUrl direct      = page(u"https://artist.bandcamp.com/album/direct"_s);
        const BandcampPageUrl member      = page(u"https://artist.bandcamp.com/track/member"_s);
        const BandcampPageUrl album2022   = page(u"https://artist.bandcamp.com/album/2022"_s);
        const BandcampPageUrl albumTie    = page(u"https://artist.bandcamp.com/album/tie"_s);
        const BandcampPageUrl unknown     = page(u"https://artist.bandcamp.com/album/unknown"_s);
        const BandcampPageUrl failed      = page(u"https://artist.bandcamp.com/album/failed"_s);

        BandcampListingBatch batch;
        batch.references = {reference(member.url, 2, 0), reference(album2022.url, 3, 1), reference(albumTie.url, 4, 2),
                            reference(unknown.url, 5, 3), reference(failed.url, 6, 4)};
        BandcampOrchestrator orchestrator{{discography, direct}, BandcampDiscographyOrder::Ascending};
        orchestrator.appendListingBatch(0, batch);
        orchestrator.completeDiscography(0);
        QCOMPARE(orchestrator.requests().size(), 6);

        const QString directKey = orchestrator.requests().at(0).key;
        QVERIFY(orchestrator.associate(directKey, albumOutcome(90, QDate(2000, 1, 1))).isEmpty());
        for(const BandcampReleaseRequest& request : orchestrator.requests()) {
            if(request.page.url == member.url) {
                QVERIFY(orchestrator.associate(request.key, memberOutcome(2, QDate(2010, 1, 1), QDate(2020, 1, 1)))
                            .isEmpty());
            }
            else if(request.page.url == album2022.url) {
                QVERIFY(orchestrator.associate(request.key, albumOutcome(3, QDate(2022, 1, 1))).isEmpty());
            }
            else if(request.page.url == albumTie.url) {
                QVERIFY(orchestrator.associate(request.key, albumOutcome(4, QDate(2020, 1, 1))).isEmpty());
            }
            else if(request.page.url == unknown.url) {
                QVERIFY(orchestrator.associate(request.key, albumOutcome(5, {})).isEmpty());
            }
            else if(request.page.url == failed.url) {
                const auto ready = orchestrator.associate(request.key, failure(u"one child failed"_s));
                QCOMPARE(ready.size(), 6);
                QCOMPARE(ready.at(0).page.url, member.url);
                QCOMPARE(ready.at(1).page.url, albumTie.url);
                QCOMPARE(ready.at(2).page.url, album2022.url);
                QCOMPARE(ready.at(3).page.url, unknown.url);
                QCOMPARE(ready.at(4).page.url, failed.url);
                QCOMPARE(ready.at(5).page.url, direct.url);
            }
        }
        QVERIFY(orchestrator.isComplete());
    }

    void descendingOrderRetainsListingTiesAndUnknownsLast()
    {
        const BandcampPageUrl discography = page(u"https://artist.bandcamp.com/music"_s);
        const BandcampPageUrl newer       = page(u"https://artist.bandcamp.com/album/newer"_s);
        const BandcampPageUrl tie         = page(u"https://artist.bandcamp.com/album/tie"_s);
        const BandcampPageUrl unknown     = page(u"https://artist.bandcamp.com/album/unknown"_s);
        BandcampListingBatch batch;
        batch.references = {reference(tie.url, 1, 0), reference(unknown.url, 2, 1), reference(newer.url, 3, 2)};
        BandcampOrchestrator orchestrator{{discography}, BandcampDiscographyOrder::Descending};
        orchestrator.appendListingBatch(0, batch);
        orchestrator.completeDiscography(0);

        const auto requests = orchestrator.requests();
        QVERIFY(orchestrator.associate(requests.at(0).key, albumOutcome(1, QDate(2024, 1, 1))).isEmpty());
        QVERIFY(orchestrator.associate(requests.at(1).key, albumOutcome(2, {})).isEmpty());
        const auto ready = orchestrator.associate(requests.at(2).key, albumOutcome(3, QDate(2025, 1, 1)));
        QCOMPARE(ready.size(), 3);
        QCOMPARE(ready.at(0).page.url, newer.url);
        QCOMPARE(ready.at(1).page.url, tie.url);
        QCOMPARE(ready.at(2).page.url, unknown.url);
    }

    void repeatedListingOccurrencesShareWorkButKeepIdentityChecksLocal()
    {
        const BandcampPageUrl discography = page(u"https://artist.bandcamp.com/music"_s);
        const BandcampPageUrl one         = page(u"https://artist.bandcamp.com/album/one"_s);
        const BandcampPageUrl two         = page(u"https://artist.bandcamp.com/album/two"_s);
        BandcampListingBatch batch;
        batch.references = {reference(one.url, 1, 0), reference(one.url, 999, 1), reference(two.url, 2, 2)};
        BandcampOrchestrator orchestrator{{discography}, BandcampDiscographyOrder::Unsorted};
        orchestrator.appendListingBatch(0, batch);
        orchestrator.completeDiscography(0);
        QCOMPARE(orchestrator.requests().size(), 2);

        const QString oneKey        = orchestrator.requests().at(0).key;
        const auto duplicateResults = orchestrator.associate(oneKey, albumOutcome(1, {}));
        QCOMPARE(duplicateResults.size(), 2);
        QVERIFY(std::holds_alternative<ResolutionSuccess<BandcampResolvedData>>(duplicateResults.at(0).outcome));
        QVERIFY(std::holds_alternative<ResolutionFailure>(duplicateResults.at(1).outcome));
        const auto final = orchestrator.associate(orchestrator.requests().at(1).key, albumOutcome(2, {}));
        QCOMPARE(final.size(), 1);
        QCOMPARE(orchestrator.deliveredCount(), 3);
        QVERIFY(orchestrator.isComplete());
    }

    void sortsCollectionsIndependentlyAroundMixedInputs()
    {
        const BandcampPageUrl artist  = page(u"https://artist.bandcamp.com/music"_s);
        const BandcampPageUrl direct  = page(u"https://artist.bandcamp.com/album/direct"_s);
        const BandcampPageUrl label   = page(u"https://label.bandcamp.com/music"_s);
        const BandcampPageUrl artist1 = page(u"https://artist.bandcamp.com/album/artist-1"_s);
        const BandcampPageUrl artist2 = page(u"https://artist.bandcamp.com/album/artist-2"_s);
        const BandcampPageUrl label1  = page(u"https://label.bandcamp.com/album/label-1"_s);
        const BandcampPageUrl label2  = page(u"https://label.bandcamp.com/album/label-2"_s);
        BandcampListingBatch artistBatch;
        artistBatch.references = {reference(artist1.url, 1, 0), reference(artist2.url, 2, 1)};
        BandcampListingBatch labelBatch;
        labelBatch.references = {reference(label1.url, 3, 0), reference(label2.url, 4, 1)};

        BandcampOrchestrator orchestrator{{artist, direct, label}, BandcampDiscographyOrder::Ascending};
        orchestrator.appendListingBatch(0, artistBatch);
        orchestrator.completeDiscography(0);
        orchestrator.appendListingBatch(2, labelBatch);
        orchestrator.completeDiscography(2);
        QVERIFY(
            orchestrator.associate(orchestrator.requests().at(0).key, albumOutcome(50, QDate(2021, 1, 1))).isEmpty());
        QVERIFY(
            orchestrator.associate(orchestrator.requests().at(1).key, albumOutcome(1, QDate(2022, 1, 1))).isEmpty());
        const auto artistReady
            = orchestrator.associate(orchestrator.requests().at(2).key, albumOutcome(2, QDate(2021, 1, 1)));
        QCOMPARE(artistReady.size(), 3);
        QCOMPARE(artistReady.at(0).page.url, artist2.url);
        QCOMPARE(artistReady.at(1).page.url, artist1.url);
        QCOMPARE(artistReady.at(2).page.url, direct.url);
        QVERIFY(
            orchestrator.associate(orchestrator.requests().at(3).key, albumOutcome(3, QDate(1990, 1, 1))).isEmpty());
        const auto labelReady
            = orchestrator.associate(orchestrator.requests().at(4).key, albumOutcome(4, QDate(2000, 1, 1)));
        QCOMPARE(labelReady.size(), 2);
        QCOMPARE(labelReady.at(0).page.url, label1.url);
        QCOMPARE(labelReady.at(1).page.url, label2.url);
    }
};

int runOrchestratorTests(int argc, char** argv)
{
    BandcampOrchestratorTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcamporchestratortests.moc"
