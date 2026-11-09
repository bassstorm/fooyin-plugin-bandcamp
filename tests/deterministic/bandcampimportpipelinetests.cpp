#include "bandcampimportpipeline.h"

#include <QSignalSpy>
#include <QtTest>

#include <utility>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
BandcampScopeResult successfulScope(qsizetype ordinal)
{
    const QString pageUrl = u"https://artist.bandcamp.com/track/track-%1"_s.arg(ordinal);
    BandcampTrack track;
    track.id        = ordinal + 1;
    track.title     = u"Track %1"_s.arg(ordinal);
    track.artist    = u"Artist"_s;
    track.sourceUrl = pageUrl;
    track.streamUrl = u"https://cdn.example.com/track.mp3"_s;
    BandcampResolvedData data{BandcampResolvedTrack{std::move(track), std::nullopt}};
    return {1,
            ordinal,
            {BandcampPageKind::Track, pageUrl, {}},
            BandcampResolutionOutcome{ResolutionSuccess<BandcampResolvedData>{std::move(data)}}};
}

BandcampStreamProbeResult successfulProbe(qint64 trackId)
{
    return {trackId, {}, std::nullopt};
}

FooyinTrackCommitOutcome successfulCommit()
{
    return FooyinTrackCommitSuccess{};
}
} // namespace

class BandcampImportPipelineTests final : public QObject
{
    Q_OBJECT

private slots:
    void probesTwoJobsConcurrently();
    void commitsInArrivalOrderAfterOutOfOrderProbes();
    void skipsProbeStageWhenDisabled();
    void failsJobsAndCancelsActiveWork();
    void emitsBecameIdleOnceWhenQueueDrains();
};

void BandcampImportPipelineTests::probesTwoJobsConcurrently()
{
    BandcampImportPipeline pipeline;
    QList<QPair<quint64, BandcampImportPipeline::ProbeCompletion>> probes;
    QList<ImportJob> finished;
    QList<quint64> cancelledProbes;
    pipeline.setProbeHandler(
        [&probes](const ImportJob& job, BandcampImportPipeline::ProbeCompletion completion) {
            probes.append({job.id, std::move(completion)});
        },
        [&cancelledProbes](const ImportJob& job) { cancelledProbes.append(job.id); });
    pipeline.setCommitHandler(
        [](const ImportJob&, BandcampImportPipeline::CommitCompletion completion) { completion(successfulCommit()); });
    QObject::connect(&pipeline, &BandcampImportPipeline::jobFinished, &pipeline,
                     [&finished](const ImportJob& job) { finished.append(job); });

    for(qsizetype index = 0; index < 3; ++index) {
        ImportJob job{successfulScope(index)};
        job.tracks.append(std::get<BandcampResolvedTrack>(
            std::get<ResolutionSuccess<BandcampResolvedData>>(job.scope.outcome).value));
        pipeline.enqueue(std::move(job));
    }

    QCOMPARE(probes.size(), 2);
    QVERIFY(!pipeline.isIdle());
    pipeline.cancel();
    QCOMPARE(cancelledProbes.size(), 2);
    QCOMPARE(finished.size(), 3);
    for(const ImportJob& job : finished) {
        QCOMPARE(job.stage, ImportJobStage::Cancelled);
    }
    QVERIFY(pipeline.isIdle());
}

void BandcampImportPipelineTests::commitsInArrivalOrderAfterOutOfOrderProbes()
{
    BandcampImportPipeline pipeline;
    QList<QPair<quint64, BandcampImportPipeline::ProbeCompletion>> probes;
    QList<QPair<quint64, BandcampImportPipeline::CommitCompletion>> commits;
    QList<quint64> inserted;
    QList<quint64> finished;
    QList<QPair<qsizetype, qsizetype>> progress;
    pipeline.setProbeHandler([&probes](const ImportJob& job, BandcampImportPipeline::ProbeCompletion completion) {
        probes.append({job.id, std::move(completion)});
    });
    pipeline.setCommitHandler([&commits](const ImportJob& job, BandcampImportPipeline::CommitCompletion completion) {
        commits.append({job.id, std::move(completion)});
    });
    pipeline.setInsertHandler([&inserted](const ImportJob& job) {
        inserted.append(job.id);
        return true;
    });
    QObject::connect(&pipeline, &BandcampImportPipeline::jobFinished, &pipeline,
                     [&finished](const ImportJob& job) { finished.append(job.id); });
    QObject::connect(&pipeline, &BandcampImportPipeline::progressChanged, &pipeline,
                     [&progress](qsizetype completed, qsizetype total) { progress.append({completed, total}); });
    pipeline.setTotalWork(3);

    for(qsizetype index = 0; index < 3; ++index) {
        ImportJob job{successfulScope(index)};
        job.tracks.append(std::get<BandcampResolvedTrack>(
            std::get<ResolutionSuccess<BandcampResolvedData>>(job.scope.outcome).value));
        pipeline.enqueue(std::move(job));
    }
    QCOMPARE(probes.size(), 2);

    auto completeProbe = [&probes](qsizetype index) {
        auto completion      = std::move(probes[index].second);
        const qint64 trackId = index + 1;
        completion({successfulProbe(trackId)});
    };
    completeProbe(1);
    QCOMPARE(probes.size(), 3);
    QCOMPARE(pipeline.completedWork(), qsizetype{1});
    QCOMPARE(progress.back(), (QPair<qsizetype, qsizetype>{1, 3}));
    QVERIFY(commits.isEmpty());
    completeProbe(2);
    QCOMPARE(pipeline.completedWork(), qsizetype{2});
    QCOMPARE(progress.back(), (QPair<qsizetype, qsizetype>{2, 3}));
    QVERIFY(commits.isEmpty());
    completeProbe(0);
    QCOMPARE(pipeline.completedWork(), qsizetype{3});
    QCOMPARE(progress.back(), (QPair<qsizetype, qsizetype>{3, 3}));

    QCOMPARE(commits.size(), 1);
    QCOMPARE(commits.front().first, quint64{1});
    auto firstCommit = std::move(commits.front().second);
    firstCommit(successfulCommit());
    QCOMPARE(inserted, QList<quint64>{1});
    QCOMPARE(commits.size(), 2);
    QCOMPARE(commits.at(1).first, quint64{2});
    auto secondCommit = std::move(commits[1].second);
    secondCommit(successfulCommit());
    QCOMPARE(inserted, (QList<quint64>{1, 2}));
    QCOMPARE(commits.size(), 3);
    QCOMPARE(commits.at(2).first, quint64{3});
    auto thirdCommit = std::move(commits[2].second);
    thirdCommit(successfulCommit());

    QCOMPARE(inserted, (QList<quint64>{1, 2, 3}));
    QCOMPARE(finished, (QList<quint64>{1, 2, 3}));
    QVERIFY(pipeline.isIdle());
}

void BandcampImportPipelineTests::skipsProbeStageWhenDisabled()
{
    BandcampImportPipeline pipeline;
    QList<ImportJobStage> stages;
    QList<quint64> inserted;
    QList<qsizetype> committedTrackCounts;
    pipeline.setCommitHandler(
        [&committedTrackCounts](const ImportJob& job, BandcampImportPipeline::CommitCompletion completion) {
            committedTrackCounts.append(job.importTracks.size());
            completion(successfulCommit());
        });
    pipeline.setInsertHandler([&inserted](const ImportJob& job) {
        inserted.append(job.id);
        return true;
    });
    QObject::connect(&pipeline, &BandcampImportPipeline::jobFinished, &pipeline,
                     [&stages](const ImportJob& job) { stages.append(job.stage); });
    pipeline.setTotalWork(2);

    for(qsizetype index = 0; index < 2; ++index) {
        ImportJob job{successfulScope(index)};
        job.tracks.append(std::get<BandcampResolvedTrack>(
            std::get<ResolutionSuccess<BandcampResolvedData>>(job.scope.outcome).value));
        pipeline.enqueue(std::move(job));
    }

    QCOMPARE(inserted, (QList<quint64>{1, 2}));
    QCOMPARE(committedTrackCounts, (QList<qsizetype>{1, 1}));
    QCOMPARE(stages, (QList<ImportJobStage>{ImportJobStage::Completed, ImportJobStage::Completed}));
    QCOMPARE(pipeline.completedWork(), qsizetype{2});
    QVERIFY(pipeline.isIdle());
}

void BandcampImportPipelineTests::failsJobsAndCancelsActiveWork()
{
    {
        BandcampImportPipeline pipeline;
        QList<ImportJob> finished;
        pipeline.setCommitHandler([](const ImportJob&, BandcampImportPipeline::CommitCompletion completion) {
            completion(FooyinTrackCommitFailure{FooyinTrackCommitErrorCode::PersistenceFailed, 1,
                                                QStringLiteral("write failed")});
        });
        QObject::connect(&pipeline, &BandcampImportPipeline::jobFinished, &pipeline,
                         [&finished](const ImportJob& job) { finished.append(job); });

        ImportJob job{successfulScope(0)};
        pipeline.enqueue(std::move(job));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.front().stage, ImportJobStage::Failed);
        QVERIFY(finished.front().commitFailure.has_value());
        QVERIFY(pipeline.isIdle());
    }

    {
        BandcampImportPipeline pipeline;
        QList<BandcampImportPipeline::ProbeCompletion> probes;
        QList<ImportJob> finished;
        QList<quint64> cancelled;
        pipeline.setProbeHandler(
            [&probes](const ImportJob&, BandcampImportPipeline::ProbeCompletion completion) {
                probes.append(std::move(completion));
            },
            [&cancelled](const ImportJob& job) { cancelled.append(job.id); });
        QObject::connect(&pipeline, &BandcampImportPipeline::jobFinished, &pipeline,
                         [&finished](const ImportJob& job) { finished.append(job); });

        for(qsizetype index = 0; index < 3; ++index) {
            ImportJob job{successfulScope(index)};
            pipeline.enqueue(std::move(job));
        }
        QCOMPARE(probes.size(), 2);
        pipeline.cancel();
        QCOMPARE(cancelled.size(), 2);
        QCOMPARE(finished.size(), 3);
        for(const ImportJob& job : finished) {
            QCOMPARE(job.stage, ImportJobStage::Cancelled);
        }
        QVERIFY(pipeline.isIdle());
        probes.front()({});
        QCOMPARE(finished.size(), 3);
    }

    {
        BandcampImportPipeline pipeline;
        QList<BandcampImportPipeline::CommitCompletion> commits;
        QList<quint64> cancelled;
        QList<ImportJob> finished;
        pipeline.setCommitHandler(
            [&commits](const ImportJob&, BandcampImportPipeline::CommitCompletion completion) {
                commits.append(std::move(completion));
            },
            [&cancelled](const ImportJob& job) { cancelled.append(job.id); });
        QObject::connect(&pipeline, &BandcampImportPipeline::jobFinished, &pipeline,
                         [&finished](const ImportJob& job) { finished.append(job); });
        pipeline.enqueue(ImportJob{successfulScope(0)});
        pipeline.enqueue(ImportJob{successfulScope(1)});
        QCOMPARE(commits.size(), 1);
        pipeline.cancel();
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(finished.size(), 2);
        for(const ImportJob& job : finished) {
            QCOMPARE(job.stage, ImportJobStage::Cancelled);
        }
        QVERIFY(pipeline.isIdle());
        commits.front()(successfulCommit());
        QCOMPARE(finished.size(), 2);
    }
}

void BandcampImportPipelineTests::emitsBecameIdleOnceWhenQueueDrains()
{
    {
        BandcampImportPipeline pipeline;
        QSignalSpy idleSpy(&pipeline, &BandcampImportPipeline::becameIdle);
        QVERIFY(idleSpy.isValid());

        QCOMPARE(idleSpy.count(), 0);
        QVERIFY(pipeline.isIdle());

        QList<BandcampImportPipeline::ProbeCompletion> probes;
        pipeline.setProbeHandler([&probes](const ImportJob&, BandcampImportPipeline::ProbeCompletion completion) {
            probes.append(std::move(completion));
        });
        pipeline.setCommitHandler([](const ImportJob&, BandcampImportPipeline::CommitCompletion completion) {
            completion(successfulCommit());
        });

        pipeline.enqueue(ImportJob{successfulScope(0)});
        pipeline.enqueue(ImportJob{successfulScope(1)});
        QVERIFY(!pipeline.isIdle());
        QCOMPARE(idleSpy.count(), 0);
        QCOMPARE(probes.size(), 2);

        probes[0]({successfulProbe(1)});
        QCOMPARE(idleSpy.count(), 0);
        QVERIFY(!pipeline.isIdle());

        probes[1]({successfulProbe(2)});
        QVERIFY(pipeline.isIdle());
        QCOMPARE(idleSpy.count(), 1);

        // Subsequent cancel calls while already idle must not re-emit becameIdle.
        pipeline.cancel();
        pipeline.cancel();
        QCOMPARE(idleSpy.count(), 1);
    }

    {
        // Cancellation of active in-flight jobs also emits becameIdle exactly once.
        BandcampImportPipeline pipeline;
        QSignalSpy idleSpy(&pipeline, &BandcampImportPipeline::becameIdle);
        QVERIFY(idleSpy.isValid());

        QList<BandcampImportPipeline::ProbeCompletion> probes;
        pipeline.setProbeHandler([&probes](const ImportJob&, BandcampImportPipeline::ProbeCompletion completion) {
            probes.append(std::move(completion));
        });

        pipeline.enqueue(ImportJob{successfulScope(0)});
        pipeline.enqueue(ImportJob{successfulScope(1)});
        QCOMPARE(idleSpy.count(), 0);
        QVERIFY(!pipeline.isIdle());
        QCOMPARE(probes.size(), 2);

        pipeline.cancel();
        QVERIFY(pipeline.isIdle());
        QCOMPARE(idleSpy.count(), 1);

        pipeline.cancel();
        QCOMPARE(idleSpy.count(), 1);
    }
}

int runImportPipelineTests(int argc, char** argv)
{
    BandcampImportPipelineTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampimportpipelinetests.moc"
