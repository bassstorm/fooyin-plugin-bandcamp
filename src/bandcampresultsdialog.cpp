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
        const auto* success
            = result.outcome ? std::get_if<ResolutionSuccess<BandcampResolvedData>>(&*result.outcome) : nullptr;
        if(!success) {
            ++failed;
            text += tr("Failed: %1\n").arg(result.page.url);
            if(result.outcome) {
                if(const auto* failure = std::get_if<ResolutionFailure>(&*result.outcome)) {
                    text += tr("  Error: %1 (%2, %3)\n")
                                .arg(failure->error.reason, resolutionErrorCodeName(failure->error.code),
                                     failure->error.field);
                }
                else {
                    text += tr("  Resolution was cancelled.\n");
                }
            }
            else {
                text += tr("  No resolution outcome was reported.\n");
            }
            text += u"\n"_s;
            continue;
        }

        if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
            const auto& metadata = album->metadata;
            text += tr("Album: %1\n").arg(metadata.title);
            text += tr("  Artist: %1\n").arg(metadata.artist);
            text += tr("  Track total: %1, playable: %2\n").arg(metadata.trackTotal).arg(album->tracks.size());
            if(metadata.releaseDate.isValid()) {
                text += tr("  Released: %1\n").arg(metadata.releaseDate.toString(u"yyyy-MM-dd"_s));
            }

            QStringList firstTracks;
            for(qsizetype i = 0; i < album->tracks.size() && i < 5; ++i) {
                firstTracks.append(album->tracks.at(i).title);
            }
            if(!firstTracks.isEmpty()) {
                text += tr("  First tracks: %1\n").arg(firstTracks.join(u", "_s));
            }
        }
        else {
            const auto& resolved = std::get<BandcampResolvedTrack>(success->value);
            const auto& track    = resolved.track;
            text += tr("Track: %1\n").arg(track.title);
            text += tr("  Artist: %1\n").arg(track.artist);
            text += tr("  Album artist: %1\n")
                        .arg(resolved.album && !resolved.album->artist.isEmpty() ? resolved.album->artist
                                                                                 : tr("unknown"));
            if(track.durationMs > 0) {
                text += tr("  Duration: %1\n").arg(formatDuration(track.durationMs));
            }
            if(track.trackNumber > 0) {
                text += tr("  Track number: %1\n").arg(track.trackNumber);
            }
            text += tr("  Track total: %1\n")
                        .arg(resolved.album ? QString::number(resolved.album->trackTotal) : tr("unknown"));
            if(resolved.album) {
                text += tr("  Album: %1\n").arg(resolved.album->title);
            }
            text += tr("  Source: %1\n").arg(track.sourceUrl);
        }

        text += u"\n"_s;
    }

    text.prepend(
        tr("Resolved %1 of %2 URL(s), %3 failed.\n\n").arg(results.size() - failed).arg(results.size()).arg(failed));
    return text;
}
} // namespace Fooyin::Bandcamp
