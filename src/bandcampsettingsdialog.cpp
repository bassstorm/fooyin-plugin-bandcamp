#include "bandcampsettingsdialog.h"

#include "bandcampsettings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
BandcampSettingsDialog::BandcampSettingsDialog(QWidget* parent)
    : QDialog{parent}
    , m_parallelism{new QSpinBox(this)}
    , m_itemTimeout{new QSpinBox(this)}
    , m_discographyOrder{new QComboBox(this)}
    , m_refreshWindow{new QSpinBox(this)}
{
    setWindowTitle(tr("Bandcamp Settings"));

    auto* parallelismLabel = new QLabel(tr("Parallel fetches") + u":"_s, this);
    m_parallelism->setRange(MinParallelism, MaxParallelism);
    m_parallelism->setToolTip(tr("Number of pages fetched and parsed at the same time"));

    auto* timeoutLabel = new QLabel(tr("Item timeout") + u":"_s, this);
    m_itemTimeout->setRange(MinItemTimeoutMs / 1000, MaxItemTimeoutMs / 1000);
    m_itemTimeout->setSuffix(u" s"_s);
    m_itemTimeout->setToolTip(tr(
        "Time allowed for one release workflow, including any required album parent; also applies per listing page"));

    auto* orderLabel = new QLabel(tr("Discography order") + u":"_s, this);
    m_discographyOrder->addItems({tr("Default"), tr("Unsorted"), tr("Ascending"), tr("Descending")});
    m_discographyOrder->setToolTip(tr("Default currently uses the unsorted listing order"));

    auto* refreshLabel = new QLabel(tr("Pre-playback refresh window") + u":"_s, this);
    m_refreshWindow->setRange(MinRefreshWindowMinutes, MaxRefreshWindowMinutes);
    m_refreshWindow->setSuffix(u" min"_s);
    m_refreshWindow->setToolTip(
        tr("Refresh streams with less than this lifetime remaining; zero refreshes expired streams only"));

    auto* note = new QLabel(tr("Changes apply to the next submission."), this);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &BandcampSettingsDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QGridLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    layout->addWidget(parallelismLabel, 0, 0);
    layout->addWidget(m_parallelism, 0, 1);
    layout->addWidget(timeoutLabel, 1, 0);
    layout->addWidget(m_itemTimeout, 1, 1);
    layout->addWidget(orderLabel, 2, 0);
    layout->addWidget(m_discographyOrder, 2, 1);
    layout->addWidget(refreshLabel, 3, 0);
    layout->addWidget(m_refreshWindow, 3, 1);
    layout->addWidget(note, 4, 0, 1, 2);
    layout->addWidget(buttons, 5, 0, 1, 2);

    const BandcampSettings current = loadBandcampSettings(m_settings);
    m_parallelism->setValue(current.parallelism);
    m_itemTimeout->setValue(current.itemTimeoutMs / 1000);
    m_discographyOrder->setCurrentIndex(static_cast<int>(current.discographyOrder));
    m_refreshWindow->setValue(current.prePlaybackRefreshWindowMinutes);
}

void BandcampSettingsDialog::accept()
{
    BandcampSettings values;
    values.parallelism                     = m_parallelism->value();
    values.itemTimeoutMs                   = m_itemTimeout->value() * 1000;
    values.discographyOrder                = static_cast<BandcampDiscographyOrder>(m_discographyOrder->currentIndex());
    values.prePlaybackRefreshWindowMinutes = m_refreshWindow->value();
    saveBandcampSettings(m_settings, values);

    QDialog::accept();
}
} // namespace Fooyin::Bandcamp
