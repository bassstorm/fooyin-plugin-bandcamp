#pragma once

#include "bandcampurl.h"

#include <QDialog>
#include <QList>

class QDialogButtonBox;
class QCheckBox;
class QLabel;
class QPlainTextEdit;

namespace Fooyin::Bandcamp {
class BandcampUrlDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BandcampUrlDialog(QWidget* parent = nullptr);

signals:
    void urlsSubmitted(const QList<BandcampPageUrl>& urls, bool forceNewPlaylist);

private:
    void updateStatus();
    void submit();

    QPlainTextEdit* m_input{nullptr};
    QCheckBox* m_forceNewPlaylist{nullptr};
    QLabel* m_status{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};
} // namespace Fooyin::Bandcamp
