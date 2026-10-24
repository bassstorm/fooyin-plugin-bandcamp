#include "bandcampsettingsdialog.h"

#include "bandcampsettings.h"

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
{
    setWindowTitle(tr("Bandcamp Settings"));

    auto* parallelismLabel = new QLabel(tr("Parallel fetches") + u":"_s, this);
    m_parallelism->setRange(MinParallelism, MaxParallelism);
    m_parallelism->setToolTip(tr("Number of pages fetched and parsed at the same time"));

    auto* timeoutLabel = new QLabel(tr("Item timeout") + u":"_s, this);
    m_itemTimeout->setRange(MinItemTimeoutMs / 1000, MaxItemTimeoutMs / 1000);
    m_itemTimeout->setSuffix(u" s"_s);
    m_itemTimeout->setToolTip(tr("Time allowed for fetching and parsing one page"));

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
    layout->addWidget(note, 2, 0, 1, 2);
    layout->addWidget(buttons, 3, 0, 1, 2);

    const BandcampSettings current = loadBandcampSettings(m_settings);
    m_parallelism->setValue(current.parallelism);
    m_itemTimeout->setValue(current.itemTimeoutMs / 1000);
}

void BandcampSettingsDialog::accept()
{
    BandcampSettings values;
    values.parallelism   = m_parallelism->value();
    values.itemTimeoutMs = m_itemTimeout->value() * 1000;
    saveBandcampSettings(m_settings, values);

    QDialog::accept();
}
} // namespace Fooyin::Bandcamp
