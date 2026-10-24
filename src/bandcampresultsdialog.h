#pragma once

#include "bandcampservice.h"

#include <QDialog>

class QPlainTextEdit;

namespace Fooyin::Bandcamp {
class BandcampResultsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BandcampResultsDialog(QWidget* parent = nullptr);

    void appendResult(const BandcampScopeResult& result);
    void updateProgress(const BandcampOperationProgress& progress);
    void finish(const BandcampOperationSummary& summary);
    void showRejection(const ResolutionError& error);

private:
    void appendBounded(QString text);
    [[nodiscard]] QString formatResult(const BandcampScopeResult& result) const;

    QPlainTextEdit* m_text{nullptr};
    qsizetype m_displayedChars{0};
    bool m_displayTruncated{false};
};
} // namespace Fooyin::Bandcamp
