#include "bandcampurldialog.h"

#include "bandcamplogging.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
constexpr int MaxReportedRejections = 10;

struct InputLine
{
    int number{0};
    BandcampPageUrl url;
};

QList<InputLine> parseInput(const QString& text)
{
    QList<InputLine> lines;
    int number                 = 0;
    const QStringList rawLines = text.split(u'\n');
    for(const QString& raw : rawLines) {
        ++number;
        if(raw.trimmed().isEmpty()) {
            continue;
        }
        lines.append({.number = number, .url = parseBandcampPageUrl(raw)});
    }
    return lines;
}
} // namespace

BandcampUrlDialog::BandcampUrlDialog(QWidget* parent)
    : QDialog{parent}
{
    setWindowTitle(tr("Add Bandcamp URLs"));
    setMinimumWidth(560);
    qCInfo(BANDCAMP_UI) << "Add Bandcamp URL dialog opened";

    auto* hint = new QLabel(
        tr("Paste one Bandcamp URL per line. Album, track, and discography pages are accepted on any domain."), this);
    hint->setWordWrap(true);

    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(u"https://artist.bandcamp.com/album/name\n"
                                "https://artist.bandcamp.com/track/name\n"
                                "https://artist.bandcamp.com/music"_s);
    m_forceNewPlaylist = new QCheckBox(tr("Create as a new playlist"), this);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addWidget(m_input);
    layout->addWidget(m_forceNewPlaylist);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);

    QObject::connect(m_input, &QPlainTextEdit::textChanged, this, &BandcampUrlDialog::updateStatus);
    QObject::connect(m_buttons, &QDialogButtonBox::accepted, this, &BandcampUrlDialog::submit);
    QObject::connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateStatus();
}

void BandcampUrlDialog::updateStatus()
{
    const QList<InputLine> lines = parseInput(m_input->toPlainText());

    int accepted = 0;
    int rejected = 0;
    QStringList problems;
    for(const InputLine& line : lines) {
        if(line.url.isValid()) {
            ++accepted;
            continue;
        }
        ++rejected;
        if(rejected <= MaxReportedRejections) {
            problems.append(tr("Line %1: %2").arg(line.number).arg(line.url.error));
        }
    }

    QString text;
    if(lines.isEmpty()) {
        text = tr("Paste one Bandcamp URL per line. Album, track, and discography pages are accepted on any domain.");
    }
    else {
        text = tr("%1 valid, %2 rejected").arg(accepted).arg(rejected);
        if(!problems.isEmpty()) {
            text += u'\n' + problems.join(u'\n');
        }
        if(rejected > MaxReportedRejections) {
            text += u'\n' + tr("…and %1 more").arg(rejected - MaxReportedRejections);
        }
    }
    m_status->setText(text);

    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(accepted > 0);
}

void BandcampUrlDialog::submit()
{
    QList<BandcampPageUrl> urls;
    for(const InputLine& line : parseInput(m_input->toPlainText())) {
        if(line.url.isValid()) {
            qCDebug(BANDCAMP_UI).noquote()
                << "line" << line.number << "accepted:" << pageKindName(line.url.kind) << line.url.url;
            urls.append(line.url);
        }
        else {
            qCDebug(BANDCAMP_UI).noquote() << "line" << line.number << "rejected:" << line.url.error;
        }
    }

    qCInfo(BANDCAMP_UI).noquote() << "Submitted" << urls.size() << "URL(s)";
    if(urls.isEmpty()) {
        return;
    }

    emit urlsSubmitted(urls, m_forceNewPlaylist->isChecked());
    accept();
}
} // namespace Fooyin::Bandcamp
