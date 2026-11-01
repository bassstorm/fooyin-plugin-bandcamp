#pragma once

#include "bandcampimportcontroller.h"

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QProgressBar;

namespace Fooyin::Bandcamp {
class BandcampResultsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BandcampResultsDialog(QWidget* parent = nullptr);

    void updateProgress(const BandcampOperationProgress& progress);
    void showImporting();
    void showIntegrationError(const QString& reason);
    void finish(const BandcampImportSummary& summary);
    void showRejection(const ResolutionError& error);

private:
    QLabel* m_status{nullptr};
    QProgressBar* m_progress{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};
} // namespace Fooyin::Bandcamp
