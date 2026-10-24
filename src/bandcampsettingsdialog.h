#pragma once

#include <core/coresettings.h>

#include <QDialog>

class QComboBox;
class QSpinBox;

namespace Fooyin::Bandcamp {
class BandcampSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BandcampSettingsDialog(QWidget* parent = nullptr);

    void accept() override;

private:
    FySettings m_settings;
    QSpinBox* m_parallelism;
    QSpinBox* m_itemTimeout;
    QComboBox* m_discographyOrder;
    QSpinBox* m_refreshWindow;
};
} // namespace Fooyin::Bandcamp
