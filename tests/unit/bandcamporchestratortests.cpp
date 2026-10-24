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
};

int runOrchestratorTests(int argc, char** argv)
{
    BandcampOrchestratorTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcamporchestratortests.moc"
