#include "bandcampresultsdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace Fooyin::Bandcamp {
BandcampResultsDialog::BandcampResultsDialog(QWidget* parent)
    : QDialog{parent}
    , m_status{new QLabel(tr("Starting Bandcamp import…"), this)}
    , m_progress{new QProgressBar(this)}
    , m_buttons{new QDialogButtonBox(QDialogButtonBox::Cancel, this)}
{
    setWindowTitle(tr("Bandcamp Import"));
    setMinimumWidth(420);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_progress->setRange(0, 0);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_buttons);

    QObject::connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void BandcampResultsDialog::updateProgress(const BandcampOperationProgress& progress)
{
    switch(progress.phase) {
        case BandcampOperationPhase::Discovering:
            m_status->setText(tr("Discovering releases…"));
            m_progress->setRange(0, 0);
            break;
        case BandcampOperationPhase::Resolving: {
            if(progress.totalWork == 0) {
                m_status->setText(tr("No releases to resolve."));
                m_progress->setRange(0, 0);
                m_progress->hide();
                break;
            }
            m_progress->show();
            m_progress->setRange(0, 100);
            const int percent = progress.completedWork >= progress.totalWork
                                    ? 100
                                    : qMin(99, static_cast<int>(100.0L * progress.completedWork / progress.totalWork));
            m_progress->setValue(percent);
            m_status->setText(tr("Resolving releases… %1 of %2")
                                  .arg(progress.completedWork)
                                  .arg(progress.totalWork));
            break;
        }
        case BandcampOperationPhase::Finished:
            m_status->setText(tr("Finishing import…"));
            m_progress->show();
            m_progress->setRange(0, 0);
            break;
    }
}

void BandcampResultsDialog::showImporting()
{
    m_status->setText(tr("Adding resolved releases to the playlist…"));
    m_progress->show();
    m_progress->setRange(0, 0);
}

void BandcampResultsDialog::showIntegrationError(const QString& reason)
{
    m_status->setText(tr("Import stopped: %1").arg(reason));
}

void BandcampResultsDialog::finish(const BandcampImportSummary& summary)
{
    QString text = tr("Import finished: %1 added, %2 failed, %3 cancelled.")
                       .arg(summary.added)
                       .arg(summary.failed)
                       .arg(summary.cancelled);
    if(!summary.stopReason.isEmpty()) {
        text += u'\n' + tr("Stopped: %1").arg(summary.stopReason);
    }
    m_status->setText(text);
    m_progress->hide();
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Close"));
}

void BandcampResultsDialog::showRejection(const ResolutionError& error)
{
    m_status->setText(tr("Import could not start: %1").arg(error.reason));
    m_progress->hide();
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Close"));
}
} // namespace Fooyin::Bandcamp
