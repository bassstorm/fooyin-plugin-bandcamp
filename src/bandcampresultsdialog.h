#pragma once

#include "bandcampservice.h"

#include <QDialog>

class QPlainTextEdit;

namespace Fooyin::Bandcamp {
class BandcampResultsDialog : public QDialog
{
    Q_OBJECT

public:
    BandcampResultsDialog(const QList<BandcampResolvedPage>& results, QWidget* parent = nullptr);

private:
    [[nodiscard]] QString formatResults(const QList<BandcampResolvedPage>& results) const;

    QPlainTextEdit* m_text{nullptr};
};
} // namespace Fooyin::Bandcamp
