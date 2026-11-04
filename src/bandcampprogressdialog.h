#pragma once

#include "bandcampimportcontroller.h"
#include "bandcamprefreshcontroller.h"

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QProgressBar;

namespace Fooyin::Bandcamp {
enum class BandcampDialogMode : quint8
{
    Import,
    Refresh,
};

class BandcampProgressDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BandcampProgressDialog(BandcampDialogMode mode, QWidget* parent = nullptr);

    void updateProgress(const BandcampOperationProgress& progress);
    void showImporting();
    void showIntegrationError(const QString& reason);
    void finish(const BandcampImportSummary& summary);
    void finishRefresh(const BandcampRefreshSummary& summary);
    void showRejection(const ResolutionError& error);

private:
    BandcampDialogMode m_mode;
    QLabel* m_status{nullptr};
    QProgressBar* m_progress{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};
} // namespace Fooyin::Bandcamp
