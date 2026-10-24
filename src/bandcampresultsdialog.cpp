#include "bandcampresultsdialog.h"

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
constexpr qsizetype MaxDisplayChars   = 1024 * 1024;
constexpr int MaxDisplayCharsPerScope = 8192;

QString formatDuration(qint64 durationMs)
{
    const qint64 totalSeconds = durationMs / 1000;
    return QStringLiteral("%1:%2").arg(totalSeconds / 60).arg(totalSeconds % 60, 2, 10, QChar(u'0'));
}
} // namespace

BandcampResultsDialog::BandcampResultsDialog(QWidget* parent)
    : QDialog{parent}
    , m_text{new QPlainTextEdit(this)}
{
    setWindowTitle(tr("Bandcamp Results"));
    resize(560, 420);

    m_text->setReadOnly(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_text);
    layout->addWidget(buttons);
}

void BandcampResultsDialog::appendResult(const BandcampScopeResult& result)
{
    appendBounded(formatResult(result));
}

void BandcampResultsDialog::updateProgress(const BandcampOperationProgress& progress)
{
    setWindowTitle(tr("Bandcamp Results — %1 of %2").arg(progress.delivered).arg(progress.requested));
}

void BandcampResultsDialog::finish(const BandcampOperationSummary& summary)
{
    appendBounded(tr("Finished: %1 succeeded, %2 failed, %3 cancelled.\n")
                      .arg(summary.succeeded)
                      .arg(summary.failed)
                      .arg(summary.cancelled));
}

void BandcampResultsDialog::showRejection(const ResolutionError& error)
{
    appendBounded(tr("Submission rejected: %1 (%2)\n").arg(error.reason, resolutionErrorCodeName(error.code)));
}

void BandcampResultsDialog::appendBounded(QString text)
{
    if(m_displayTruncated) {
        return;
    }
    if(text.size() > MaxDisplayCharsPerScope) {
        text.truncate(MaxDisplayCharsPerScope - 32);
        text += tr("\n  [scope display truncated]\n");
    }
    if(m_displayedChars + text.size() > MaxDisplayChars) {
        const qsizetype available = MaxDisplayChars - m_displayedChars;
        if(available > 0) {
            QString notice
                = tr("[Further diagnostic display omitted at the 1 MiB UI limit; service outcomes continue.]")
                      .left(available);
            m_text->appendPlainText(notice);
            m_displayedChars += notice.size();
        }
        m_displayTruncated = true;
        return;
    }
    m_displayedChars += text.size();
    m_text->appendPlainText(text);
}

QString BandcampResultsDialog::formatResult(const BandcampScopeResult& result) const
{
    QString text = tr("[%1] %2\n").arg(result.ordinal + 1).arg(result.page.url);
    if(const auto* failure = std::get_if<ResolutionFailure>(&result.outcome)) {
        text += tr("  Failed: %1 (%2, %3)\n\n")
                    .arg(failure->error.reason, resolutionErrorCodeName(failure->error.code), failure->error.field);
        return text;
    }
    if(std::holds_alternative<ResolutionCancelled>(result.outcome)) {
        text += tr("  Cancelled.\n\n");
        return text;
    }

    const auto& data = std::get<ResolutionSuccess<BandcampResolvedData>>(result.outcome).value;
    if(const auto* album = std::get_if<BandcampAlbum>(&data)) {
        text += tr("  Album: %1\n  Artist: %2\n  Track total: %3, playable: %4\n")
                    .arg(album->metadata.title, album->metadata.artist)
                    .arg(album->metadata.trackTotal)
                    .arg(album->tracks.size());
        if(album->tracks.size() > 5) {
            text += tr("  Showing the first five playable tracks in diagnostics.\n");
        }
        if(album->metadata.releaseDate.isValid()) {
            text += tr("  Released: %1\n").arg(album->metadata.releaseDate.toString(u"yyyy-MM-dd"_s));
        }
        for(qsizetype i = 0; i < album->tracks.size() && i < 5; ++i) {
            text += tr("  Track %1: %2\n").arg(i + 1).arg(album->tracks.at(i).title);
        }
    }
    else {
        const auto& resolved = std::get<BandcampResolvedTrack>(data);
        const auto& track    = resolved.track;
        text += tr("  Track: %1\n  Artist: %2\n").arg(track.title, track.artist);
        if(track.durationMs > 0) {
            text += tr("  Duration: %1\n").arg(formatDuration(track.durationMs));
        }
        if(track.trackNumber > 0) {
            text += tr("  Track number: %1\n").arg(track.trackNumber);
        }
        text += tr("  Album: %1\n\n").arg(resolved.album ? resolved.album->title : tr("unknown"));
    }
    text += u"\n"_s;
    return text;
}
} // namespace Fooyin::Bandcamp
