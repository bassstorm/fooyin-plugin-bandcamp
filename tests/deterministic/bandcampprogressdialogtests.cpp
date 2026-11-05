#include "bandcampprogressdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

class ProgressDialogTests : public QObject
{
    Q_OBJECT

private slots:
    void importModeReportsProgressAndSummary()
    {
        BandcampProgressDialog dialog{BandcampDialogMode::Import};
        auto* status      = dialog.findChild<QLabel*>();
        auto* progressBar = dialog.findChild<QProgressBar*>();
        auto* buttons     = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(status);
        QVERIFY(progressBar);
        QVERIFY(buttons);
        QCOMPARE(dialog.windowTitle(), u"Bandcamp Import"_s);
        QCOMPARE(status->text(), u"Starting Bandcamp import…"_s);

        BandcampOperationProgress progress;
        progress.phase         = BandcampOperationPhase::Resolving;
        progress.completedWork = 2;
        progress.totalWork     = 4;
        dialog.updateProgress(progress);
        QCOMPARE(progressBar->value(), 50);
        QCOMPARE(status->text(), u"Resolving releases… 2 of 4"_s);
        progress.phase = BandcampOperationPhase::Finished;
        dialog.updateProgress(progress);
        QCOMPARE(status->text(), u"Finishing import…"_s);

        dialog.finish(BandcampImportSummary{.added = 3, .failed = 1, .cancelled = 2});
        QCOMPARE(status->text(), u"Import finished: 3 added, 1 failed, 2 cancelled."_s);
        QVERIFY(progressBar->isHidden());
        QCOMPARE(buttons->button(QDialogButtonBox::Cancel)->text(), u"Close"_s);
    }

    void refreshModeReportsProgressAndSummary()
    {
        BandcampProgressDialog dialog{BandcampDialogMode::Refresh};
        auto* status      = dialog.findChild<QLabel*>();
        auto* progressBar = dialog.findChild<QProgressBar*>();
        auto* buttons     = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(status);
        QVERIFY(progressBar);
        QVERIFY(buttons);
        QCOMPARE(dialog.windowTitle(), u"Bandcamp Stream Refresh"_s);
        QCOMPARE(status->text(), u"Starting Bandcamp stream refresh…"_s);

        BandcampOperationProgress progress;
        progress.phase = BandcampOperationPhase::Finished;
        dialog.updateProgress(progress);
        QCOMPARE(status->text(), u"Finishing stream updates…"_s);
        dialog.finishRefresh(BandcampRefreshSummary{.updated = 2, .unchanged = 3, .failed = 1, .cancelled = 4});
        QCOMPARE(
            status->text(),
            u"Refresh finished (unique selected Bandcamp tracks): 2 updated, 3 unchanged, 1 failed, 4 cancelled."_s);
        QVERIFY(progressBar->isHidden());
        QCOMPARE(buttons->button(QDialogButtonBox::Cancel)->text(), u"Close"_s);
    }
};

QTEST_MAIN(ProgressDialogTests)

#include "bandcampprogressdialogtests.moc"
