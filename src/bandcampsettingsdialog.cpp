#include "bandcampsettingsdialog.h"

#include "bandcampsettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
BandcampSettingsDialog::BandcampSettingsDialog(QWidget* parent)
    : QDialog{parent}
    , m_parallelism{new QSpinBox(this)}
    , m_attemptTimeout{new QSpinBox(this)}
    , m_maxAttempts{new QSpinBox(this)}
    , m_attemptCooldown{new QSpinBox(this)}
    , m_discographyOrder{new QComboBox(this)}
    , m_refreshWindow{new QSpinBox(this)}
    , m_probeStreams{new QCheckBox(tr("Probe stream properties during import"), this)}
{
    setWindowTitle(tr("Bandcamp Settings"));

    auto* parallelismLabel = new QLabel(tr("Parallel fetches") + u":"_s, this);
    m_parallelism->setRange(MinParallelism, MaxParallelism);
    m_parallelism->setToolTip(tr("Number of pages fetched and parsed at the same time"));

    auto* orderLabel = new QLabel(tr("Discography order") + u":"_s, this);
    m_discographyOrder->addItems({tr("Default"), tr("Unsorted"), tr("Ascending"), tr("Descending")});
    m_discographyOrder->setToolTip(tr("Default currently uses the unsorted listing order"));

    auto* refreshLabel = new QLabel(tr("Stream refresh window") + u":"_s, this);
    m_refreshWindow->setRange(MinRefreshWindowMinutes, MaxRefreshWindowMinutes);
    m_refreshWindow->setSuffix(u" min"_s);
    m_refreshWindow->setToolTip(
        tr("Manual refresh updates streams with less than this lifetime remaining. Zero refreshes expired "
           "streams only; unknown or untrusted expiry is always refreshed."));

    auto* resilienceGroup = new QGroupBox(tr("Network resilience"), this);

    auto* attemptTimeoutLabel = new QLabel(tr("Attempt timeout") + u":"_s, resilienceGroup);
    m_attemptTimeout->setRange(MinAttemptTimeoutMs / 1000, MaxAttemptTimeoutMs / 1000);
    m_attemptTimeout->setSuffix(u" s"_s);
    m_attemptTimeout->setToolTip(
        tr("Time allowed for each fetch attempt. A timed-out attempt counts as a failed attempt and is retried "
           "if attempts remain."));

    auto* maxAttemptsLabel = new QLabel(tr("Attempts per page") + u":"_s, resilienceGroup);
    m_maxAttempts->setRange(MinAttemptsPerPage, MaxAttemptsPerPage);
    m_maxAttempts->setToolTip(
        tr("Total tries for each page, including the first. Transient failures are retried, such as network "
           "errors, timeouts, and HTTP 408, 429, or 5xx. Other failures, such as 404, fail immediately."));

    auto* cooldownLabel = new QLabel(tr("Pause between attempts") + u":"_s, resilienceGroup);
    m_attemptCooldown->setRange(MinAttemptCooldownMs / 1000, MaxAttemptCooldownMs / 1000);
    m_attemptCooldown->setSuffix(u" s"_s);
    m_attemptCooldown->setToolTip(tr("Wait before starting each retry"));

    auto* resilienceLayout = new QGridLayout(resilienceGroup);
    resilienceLayout->addWidget(attemptTimeoutLabel, 0, 0);
    resilienceLayout->addWidget(m_attemptTimeout, 0, 1);
    resilienceLayout->addWidget(maxAttemptsLabel, 1, 0);
    resilienceLayout->addWidget(m_maxAttempts, 1, 1);
    resilienceLayout->addWidget(cooldownLabel, 2, 0);
    resilienceLayout->addWidget(m_attemptCooldown, 2, 1);

    auto* note = new QLabel(tr("Changes apply to the next submission."), this);
    m_probeStreams->setToolTip(tr("Read stream headers with Fooyin decoders to discover codec and audio properties. "
                                  "Probe failures do not prevent import."));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &BandcampSettingsDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QGridLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    layout->addWidget(parallelismLabel, 0, 0);
    layout->addWidget(m_parallelism, 0, 1);
    layout->addWidget(orderLabel, 1, 0);
    layout->addWidget(m_discographyOrder, 1, 1);
    layout->addWidget(refreshLabel, 2, 0);
    layout->addWidget(m_refreshWindow, 2, 1);
    layout->addWidget(m_probeStreams, 3, 0, 1, 2);
    layout->addWidget(resilienceGroup, 4, 0, 1, 2);
    layout->addWidget(note, 5, 0, 1, 2);
    layout->addWidget(buttons, 6, 0, 1, 2);

    const BandcampSettings current = loadBandcampSettings(m_settings);
    m_parallelism->setValue(current.parallelism);
    m_attemptTimeout->setValue(current.attemptTimeoutMs / 1000);
    m_maxAttempts->setValue(current.maxAttempts);
    m_attemptCooldown->setValue(current.attemptCooldownMs / 1000);
    m_discographyOrder->setCurrentIndex(static_cast<int>(current.discographyOrder));
    m_refreshWindow->setValue(current.prePlaybackRefreshWindowMinutes);
    m_probeStreams->setChecked(current.probeStreamsOnImport);
}

void BandcampSettingsDialog::accept()
{
    BandcampSettings values;
    values.parallelism                     = m_parallelism->value();
    values.attemptTimeoutMs                = m_attemptTimeout->value() * 1000;
    values.maxAttempts                     = m_maxAttempts->value();
    values.attemptCooldownMs               = m_attemptCooldown->value() * 1000;
    values.discographyOrder                = static_cast<BandcampDiscographyOrder>(m_discographyOrder->currentIndex());
    values.prePlaybackRefreshWindowMinutes = m_refreshWindow->value();
    values.probeStreamsOnImport            = m_probeStreams->isChecked();
    saveBandcampSettings(m_settings, values);

    QDialog::accept();
}
} // namespace Fooyin::Bandcamp
