#include "bandcampresultsdialog.h"

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {
namespace {
QString formatDuration(qint64 durationMs)
{
    const qint64 totalSeconds = durationMs / 1000;
    return QStringLiteral("%1:%2").arg(totalSeconds / 60).arg(totalSeconds % 60, 2, 10, QChar(u'0'));
}

QString formatWarnings(const QStringList& warnings)
{
    if(warnings.isEmpty()) {
        return {};
    }
    return QStringLiteral("  Warnings: %1\n").arg(warnings.join(u"; "_s));
}
} // namespace

BandcampResultsDialog::BandcampResultsDialog(const QList<BandcampResolvedPage>& results, QWidget* parent)
    : QDialog(parent)
    , m_text{new QPlainTextEdit(this)}
{
    setWindowTitle(tr("Bandcamp Results"));
    resize(560, 420);

    m_text->setReadOnly(true);
    m_text->setPlainText(formatResults(results));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_text);
    layout->addWidget(buttons);
}

QString BandcampResultsDialog::formatResults(const QList<BandcampResolvedPage>& results) const
{
    QString text;
    int failed = 0;

    for(const auto& result : results) {
        if(!result.isValid()) {
            ++failed;
            text += tr("Failed: %1\n  Error: %2\n").arg(result.page.url, result.error);
            text += formatWarnings(result.warnings);
            text += u"\n"_s;
            continue;
        }

        if(result.page.kind == BandcampPageKind::Album) {
            const auto& album = result.album;
            text += tr("Album: %1\n").arg(album.title);
            text += tr("  Artist: %1\n").arg(album.artist);
            text += tr("  Track total: %1, playable: %2\n").arg(album.trackTotal).arg(album.tracks.size());
            if(album.releaseDate.isValid()) {
                text += tr("  Released: %1\n").arg(album.releaseDate.toString(u"yyyy-MM-dd"_s));
            }

            QStringList firstTracks;
            for(qsizetype i = 0; i < album.tracks.size() && i < 5; ++i) {
                firstTracks.append(album.tracks.at(i).title);
            }
            if(!firstTracks.isEmpty()) {
                text += tr("  First tracks: %1\n").arg(firstTracks.join(u", "_s));
            }
        }
        else {
            const auto& track = result.track;
            text += tr("Track: %1\n").arg(track.title);
            text += tr("  Artist: %1\n").arg(track.artist);
            text += tr("  Album artist: %1\n").arg(track.albumArtist.isEmpty() ? tr("unknown") : track.albumArtist);
            if(track.durationMs > 0) {
                text += tr("  Duration: %1\n").arg(formatDuration(track.durationMs));
            }
            if(track.trackNumber > 0) {
                text += tr("  Track number: %1\n").arg(track.trackNumber);
            }
            text += tr("  Track total: %1\n")
                        .arg(track.trackTotal > 0 ? QString::number(track.trackTotal) : tr("unknown"));
            text += tr("  Source: %1\n").arg(track.sourceUrl);
        }

        text += formatWarnings(result.warnings);
        text += u"\n"_s;
    }

    text.prepend(
        tr("Resolved %1 of %2 URL(s), %3 failed.\n\n").arg(results.size() - failed).arg(results.size()).arg(failed));
    return text;
}
} // namespace Fooyin::Bandcamp
