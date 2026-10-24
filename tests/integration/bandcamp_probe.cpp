// Live Bandcamp probe. Fetches real pages, runs them through the same service
// the plugin uses, and reports what it found. Run it on demand after Bandcamp
// changes its pages. It is not a unit test.

#include "bandcampdiagnostics.h"
#include "bandcampmodel.h"
#include "bandcampservice.h"
#include "bandcampurl.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QSet>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
constexpr int DefaultTimeoutMs = 10000;
// Covers the whole run, including a batch of slow items. A hang ends the probe
// instead of blocking it.
constexpr int WatchdogMs                = 600000;
constexpr qint64 MaxTrackMs             = 604800000;
constexpr qsizetype MaxProbeOutputChars = 1024 * 1024;
constexpr qsizetype MaxProbeFieldChars  = 512;
constexpr qsizetype MaxProbeTracks      = 5;

// The agreed probe set: artist and label discographies, an album, a track with
// album, and a standalone track.
const QStringList DefaultUrls{
    u"https://dither.bandcamp.com"_s,
    u"https://breakbeatrebelz.bandcamp.com/"_s,
    u"https://offland.bandcamp.com/album/parallels"_s,
    u"https://offland.bandcamp.com/track/caaa-baaa-aaa"_s,
    u"https://ravver.bandcamp.com/track/burial-shutta-ravver-remix"_s,
};

// Values observed on the live pages when the probe was written. An empty string
// or zero means "not checked". A mismatch means Bandcamp changed the page or
// the model, or the parser regressed.
struct Snapshot
{
    QString url;
    QString title;
    QString artist;
    int trackCount{0};
    qint64 firstTrackMs{0};
    qint64 trackMs{0};
    int trackNumber{0};
    bool hasAlbum{false};
    int trackTotal{0};
};

const QList<Snapshot> Snapshots{
    {u"https://offland.bandcamp.com/album/parallels"_s, u"Parallels"_s, u"Off Land"_s, 4, 1142620, 0, 0, false, 4},
    {u"https://offland.bandcamp.com/track/caaa-baaa-aaa"_s, u"Caaa Baaa Aaa"_s, QString(), 0, 0, 614043, 1, true, 0},
    {u"https://ravver.bandcamp.com/track/burial-shutta-ravver-remix"_s, u"Burial - Shutta - (Ravver Remix)"_s,
     QString(), 0, 0, 183429, 0, false, 0},
};

enum class Outcome : quint8
{
    Pass,
    Fail
};

struct Report
{
    Outcome outcome{Outcome::Pass};
    QStringList lines;
};

void check(Report& report, bool ok, const QString& message)
{
    if(ok) {
        return;
    }
    report.outcome = Outcome::Fail;
    report.lines << u"    FAIL  %1"_s.arg(message);
}

const Snapshot* snapshotFor(const QString& url)
{
    for(const Snapshot& snapshot : Snapshots) {
        if(snapshot.url == url) {
            return &snapshot;
        }
    }
    return nullptr;
}

void checkStream(Report& report, const QString& streamUrl, const QString& where)
{
    check(report, streamUrl.startsWith(u"https://"_s), where + u": stream URL is not https"_s);
}

void checkAlbum(Report& report, const BandcampAlbum& album, const Snapshot* snapshot)
{
    const auto& metadata = album.metadata;
    check(report, !metadata.title.isEmpty(), u"album has no title"_s);
    check(report, metadata.trackTotal >= album.tracks.size(), u"track total is below playable track count"_s);
    check(report, !album.tracks.isEmpty(), u"album has no playable tracks"_s);

    QSet<int> numbers;
    for(qsizetype i = 0; i < album.tracks.size(); ++i) {
        const BandcampTrack& track = album.tracks.at(i);
        const QString where        = u"track %1"_s.arg(i + 1);
        check(report, !track.title.isEmpty(), where + u": no title"_s);
        check(report, track.durationMs >= 0 && track.durationMs <= MaxTrackMs, where + u": duration out of range"_s);
        if(track.trackNumber > 0) {
            const bool unique = !numbers.contains(track.trackNumber);
            numbers.insert(track.trackNumber);
            check(report, unique, where + u": track number is duplicated"_s);
        }
        checkStream(report, track.streamUrl, where);
        if(track.durationMs == 0) {
            report.lines << u"    note  %1: duration unknown"_s.arg(where);
        }
    }

    if(!snapshot) {
        return;
    }
    check(report, metadata.title == snapshot->title,
          u"title is '%1', expected '%2'"_s.arg(metadata.title, snapshot->title));
    if(!snapshot->artist.isEmpty()) {
        check(report, metadata.artist == snapshot->artist,
              u"artist is '%1', expected '%2'"_s.arg(metadata.artist, snapshot->artist));
    }
    check(report, album.tracks.size() == snapshot->trackCount,
          u"%1 tracks, expected %2"_s.arg(album.tracks.size()).arg(snapshot->trackCount));
    if(snapshot->trackTotal > 0) {
        check(report, metadata.trackTotal == snapshot->trackTotal,
              u"track total is %1, expected %2"_s.arg(metadata.trackTotal).arg(snapshot->trackTotal));
    }
    if(!album.tracks.isEmpty()) {
        check(report, album.tracks.first().durationMs == snapshot->firstTrackMs,
              u"first track is %1 ms, expected %2"_s.arg(album.tracks.first().durationMs).arg(snapshot->firstTrackMs));
    }
}

void checkTrack(Report& report, const BandcampResolvedTrack& resolved, const Snapshot* snapshot)
{
    const auto& track = resolved.track;
    check(report, !track.title.isEmpty(), u"track has no title"_s);
    check(report, track.durationMs >= 0 && track.durationMs <= MaxTrackMs, u"track duration out of range"_s);
    checkStream(report, track.streamUrl, u"track"_s);

    if(!snapshot) {
        return;
    }
    check(report, track.title == snapshot->title, u"title is '%1', expected '%2'"_s.arg(track.title, snapshot->title));
    if(!snapshot->artist.isEmpty()) {
        check(report, track.artist == snapshot->artist,
              u"artist is '%1', expected '%2'"_s.arg(track.artist, snapshot->artist));
    }
    check(report, track.durationMs == snapshot->trackMs,
          u"duration is %1 ms, expected %2"_s.arg(track.durationMs).arg(snapshot->trackMs));
    check(report, track.trackNumber == snapshot->trackNumber,
          u"track number is %1, expected %2"_s.arg(track.trackNumber).arg(snapshot->trackNumber));
    const bool albumSource = resolved.album.has_value();
    if(albumSource) {
        check(report, resolved.album->trackTotal > 0, u"track has an album but no track total after album fetch"_s);
    }
    else {
        check(report, parseBandcampPageUrl(track.sourceUrl).kind == BandcampPageKind::Track,
              u"standalone track has a non-track source URL"_s);
    }
    check(report, albumSource == snapshot->hasAlbum,
          snapshot->hasAlbum ? u"album source URL missing"_s : u"unexpected album source URL"_s);
}

Report evaluate(const BandcampScopeResult& result)
{
    Report report;
    const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&result.outcome);
    if(!success) {
        report.outcome = Outcome::Fail;
        if(const auto* failure = std::get_if<ResolutionFailure>(&result.outcome)) {
            report.lines << u"    FAIL  %1 (%2, %3)"_s.arg(
                failure->error.reason, resolutionErrorCodeName(failure->error.code), failure->error.field);
        }
        else {
            report.lines << u"    FAIL  resolution cancelled"_s;
        }
        return report;
    }

    const Snapshot* snapshot = snapshotFor(result.page.url);
    if(const auto* album = std::get_if<BandcampAlbum>(&success->value)) {
        checkAlbum(report, *album, snapshot);
    }
    else {
        checkTrack(report, std::get<BandcampResolvedTrack>(success->value), snapshot);
    }
    if(!snapshot) {
        report.lines << u"    note  no snapshot for this URL, only generic checks ran"_s;
    }
    return report;
}

QString quoted(const QString& value)
{
    return u"\"%1\""_s.arg(value);
}

// Stream URLs carry a signed query string; keep host and path only.
QString redactSignedUrl(const QString& url)
{
    return redactUrlForLog(QUrl(url));
}

QString dateText(QDate date)
{
    return date.isValid() ? date.toString(Qt::ISODate) : QString();
}

// One "name: value" line. Empty values are printed as "" so every field is
// visible.
QString field(const QString& indent, const QString& name, const QString& value)
{
    QString shown = value;
    if(shown.size() > MaxProbeFieldChars) {
        shown.truncate(MaxProbeFieldChars - 16);
        shown += u"… [truncated]"_s;
    }
    return u"%1%2%3"_s.arg(indent, (name + u":"_s).leftJustified(20), shown);
}

QStringList trackFields(const BandcampTrack& track, const BandcampAlbumMetadata* album, const QString& indent)
{
    return {
        field(indent, u"id"_s, QString::number(track.id)),
        field(indent, u"title"_s, quoted(track.title)),
        field(indent, u"artist"_s, quoted(track.artist)),
        field(indent, u"albumArtist"_s, quoted(album ? album->artist : QString{})),
        field(indent, u"sourceUrl"_s, quoted(track.sourceUrl)),
        field(indent, u"streamUrl"_s, quoted(redactSignedUrl(track.streamUrl))),
        field(indent, u"releaseDate"_s, quoted(dateText(track.releaseDate))),
        field(indent, u"artId"_s, QString::number(track.artId)),
        field(indent, u"trackNumber"_s, QString::number(track.trackNumber)),
        field(indent, u"trackTotal"_s, QString::number(album ? album->trackTotal : 0)),
        field(indent, u"durationMs"_s, QString::number(track.durationMs)),
    };
}

QStringList albumFields(const BandcampAlbum& album, const QString& indent)
{
    const auto& metadata = album.metadata;
    return {
        field(indent, u"id"_s, QString::number(metadata.id)),
        field(indent, u"title"_s, quoted(metadata.title)),
        field(indent, u"artist"_s, quoted(metadata.artist)),
        field(indent, u"sourceUrl"_s, quoted(metadata.sourceUrl)),
        field(indent, u"releaseDate"_s, quoted(dateText(metadata.releaseDate))),
        field(indent, u"artId"_s, QString::number(metadata.artId)),
        field(indent, u"trackTotal"_s, QString::number(metadata.trackTotal)),
        field(indent, u"tracks"_s, QString::number(album.tracks.size())),
    };
}

void printResult(const BandcampScopeResult& result, int& passed, int& failed, qsizetype& diagnosticChars,
                 bool& diagnosticsOmitted)
{
    const Report report = evaluate(result);
    QString status;
    switch(report.outcome) {
        case Outcome::Pass:
            ++passed;
            status = u"PASS"_s;
            break;
        case Outcome::Fail:
            ++failed;
            status = u"FAIL"_s;
            break;
    }

    if(diagnosticsOmitted) {
        return;
    }
    QString diagnostics;
    QTextStream out(&diagnostics);
    out << status << u"  "_s << pageKindName(result.page.kind) << u"  "_s << result.page.url << u"\n"_s;
    const auto* success = std::get_if<ResolutionSuccess<BandcampResolvedData>>(&result.outcome);
    if(success && std::holds_alternative<BandcampAlbum>(success->value)) {
        const auto& album = std::get<BandcampAlbum>(success->value);
        out << u"  album:\n"_s;
        for(const QString& line : albumFields(album, u"      "_s)) {
            out << line << u"\n"_s;
        }
        out << u"  tracks:\n"_s;
        const qsizetype shownTracks = std::min(album.tracks.size(), MaxProbeTracks);
        for(qsizetype i = 0; i < shownTracks; ++i) {
            out << u"    [%1]\n"_s.arg(i + 1);
            for(const QString& line : trackFields(album.tracks.at(i), &album.metadata, u"      "_s)) {
                out << line << u"\n"_s;
            }
        }
        if(album.tracks.size() > shownTracks) {
            out << u"    … %1 playable track(s) omitted by the probe display limit\n"_s.arg(album.tracks.size()
                                                                                            - shownTracks);
        }
    }
    else if(success && std::holds_alternative<BandcampResolvedTrack>(success->value)) {
        const auto& track = std::get<BandcampResolvedTrack>(success->value);
        out << u"  track:\n"_s;
        for(const QString& line : trackFields(track.track, track.album ? &*track.album : nullptr, u"      "_s)) {
            out << line << u"\n"_s;
        }
    }
    for(const QString& line : report.lines) {
        out << line << u"\n"_s;
    }
    out.flush();
    if(diagnosticChars + diagnostics.size() > MaxProbeOutputChars) {
        QTextStream(stdout)
            << u"[Further probe diagnostics omitted at the 1 MiB display limit; outcome counts continue.]\n"_s;
        diagnosticsOmitted = true;
        return;
    }
    QTextStream(stdout) << diagnostics;
    diagnosticChars += diagnostics.size();
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"bandcamp-probe"_s);

    // Production parser TRACE provides the source snapshot without a second
    // extraction or fetch.
    const QString envRules = qEnvironmentVariable("QT_LOGGING_RULES").replace(u';', u'\n');
    QLoggingCategory::setFilterRules(envRules
                                     + u"\nfy.trace.bandcamp.parser.debug=true\n"
                                       "fy.trace.bandcamp.tracks.debug=true"_s);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"Fetches live Bandcamp pages and checks the parser against them. Not "
        u"part of the plugin build.\n"
        u"Provenance of each value is logged to stderr. More debug logs: QT_LOGGING_RULES=\"fy.bandcamp.*.debug=true\""_s);
    parser.addHelpOption();

    const QCommandLineOption parallelOption({u"parallel"_s, u"p"_s}, u"Items fetched or parsed at once (1-8)."_s,
                                            u"n"_s, QString::number(DefaultParallelism));
    const QCommandLineOption timeoutOption({u"timeout"_s, u"t"_s},
                                           u"Per-item time budget in milliseconds (1000-120000)."_s, u"ms"_s,
                                           QString::number(DefaultTimeoutMs));
    parser.addOptions({parallelOption, timeoutOption});
    parser.addPositionalArgument(u"url"_s, u"Bandcamp page URLs. Defaults to the agreed probe set."_s, u"[url...]"_s);
    parser.process(app);

    bool parallelOk       = false;
    bool timeoutOk        = false;
    const int parallelism = parser.value(parallelOption).toInt(&parallelOk);
    const int timeoutMs   = parser.value(timeoutOption).toInt(&timeoutOk);
    if(!parallelOk || !timeoutOk || parallelism < 1 || timeoutMs < 1) {
        QTextStream(stderr) << u"--parallel and --timeout must be positive integers\n"_s;
        return 2;
    }

    QStringList inputs = parser.positionalArguments();
    if(inputs.isEmpty()) {
        inputs = DefaultUrls;
    }

    QList<BandcampPageUrl> pages;
    for(const QString& input : std::as_const(inputs)) {
        const BandcampPageUrl page = parseBandcampPageUrl(input);
        if(!page.isValid()) {
            QTextStream(stderr) << u"Not a Bandcamp page URL: "_s << input << u" ("_s << page.error << u")\n"_s;
            return 2;
        }
        pages.append(page);
    }

    QNetworkAccessManager network;
    BandcampService service(&network);

    QElapsedTimer clock;
    clock.start();

    int passed                = 0;
    int failed                = 0;
    qsizetype diagnosticChars = 0;
    bool diagnosticsOmitted   = false;
    QObject::connect(&service, &BandcampService::scopeResolved, &app,
                     [&passed, &failed, &diagnosticChars, &diagnosticsOmitted](const BandcampScopeResult& result) {
                         printResult(result, passed, failed, diagnosticChars, diagnosticsOmitted);
                     });
    QObject::connect(&service, &BandcampService::operationFinished, &app,
                     [&app, &clock, parallelism, &passed, &failed](const BandcampOperationSummary& summary) {
                         QTextStream(stdout) << u"\n"_s << summary.requested << u" page(s) in "_s << clock.elapsed()
                                             << u" ms, parallelism "_s << parallelism << u": "_s << passed
                                             << u" passed, "_s << failed << u" failed\n"_s;
                         QTextStream(stdout).flush();
                         app.exit(failed > 0 ? 1 : 0);
                     });
    QTimer::singleShot(WatchdogMs, &app, [] {
        QTextStream(stderr) << u"Watchdog expired before the batch finished\n"_s;
        QCoreApplication::exit(1);
    });

    const auto submission = service.submit(pages, OperationOptions{parallelism, timeoutMs});
    if(const auto* rejected = std::get_if<BandcampSubmissionRejected>(&submission)) {
        QTextStream(stderr) << u"Submission rejected: "_s << rejected->error.reason << u"\n"_s;
        return 2;
    }
    return app.exec();
}
