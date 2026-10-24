#pragma once

#include "bandcampurl.h"

#include <QDialog>
#include <QList>

class QDialogButtonBox;
class QLabel;
class QPlainTextEdit;

namespace Fooyin::Bandcamp {
class BandcampUrlDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BandcampUrlDialog(QWidget* parent = nullptr);

signals:
    void urlsSubmitted(const QList<BandcampPageUrl>& urls);

private:
    void updateStatus();
    void submit();

    QPlainTextEdit* m_input{nullptr};
    QLabel* m_status{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};
} // namespace Fooyin::Bandcamp
