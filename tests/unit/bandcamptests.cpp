#include "bandcampparser.h"
#include "bandcampsettings.h"
#include "bandcamptestpages.h"
#include "bandcampurl.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <iterator>
#include <random>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QUrl AlbumUrl{u"https://offland.bandcamp.com/album/parallels"_s};
const QUrl TrackWithAlbumUrl{u"https://offland.bandcamp.com/track/caaa-baaa-aaa"_s};
const QUrl StandaloneTrackUrl{u"https://ravver.bandcamp.com/track/burial-shutta-ravver-remix"_s};

constexpr int DefaultMutationIterations = 10000;
constexpr std::uint32_t MutationSeed    = 0x5EEDB4C5U;
constexpr qint64 MaxTrackDurationMs     = 604800000;

// Synthetic pages shaped like Bandcamp's data-tralbum model. No real page is stored on disk;
// live checks against Bandcamp belong to the integration probe.
QByteArray sampleAlbumPage()
{
    return tralbumPage(
        R"({"current":{"type":"album","title":"Parallels","id":1370929774,"art_id":339985870,)"
        R"("release_date":"15 Jan 2016 00:00:00 GMT"},"artist":"Off Land","art_id":339985870,)"
        R"("album_release_date":"15 Jan 2016 00:00:00 GMT","url":"https://offland.bandcamp.com/album/parallels",)"
        R"("trackinfo":[)"
        + streamEntry(R"("id":1,"title":"Ingressive","track_num":1,"duration":1142.62)") + ','
        + streamEntry(R"("id":2,"title":"Crossing","track_num":2,"duration":300.5)") + ','
        + streamEntry(R"("id":3,"title":"Parallels II","track_num":3,"duration":215)") + ','
        + streamEntry(R"("id":4,"title":"Outro","track_num":4,"duration":61.25)") + "]}");
}

QByteArray sampleTrackWithAlbumPage()
{
    return tralbumPage(
        R"({"current":{"type":"track","title":"Caaa Baaa Aaa","artist":"Off Land","id":2001,"art_id":339985870,)"
        R"("track_number":1,"release_date":"15 Jan 2016 00:00:00 GMT"},)"
        R"("album_url":"https://offland.bandcamp.com/album/caaa-baaa-aaa","artist":"Off Land","trackinfo":[)"
        + streamEntry(R"("id":2001,"title":"Caaa Baaa Aaa","track_num":1,"duration":614.043)") + "]}");
}

QByteArray sampleStandaloneTrackPage()
{
    return tralbumPage(
        R"j({"current":{"type":"track","title":"Burial - Shutta - (Ravver Remix)","artist":"Ravver","id":3001,)j"
        R"j("track_number":null},"album_url":null,"trackinfo":[)j"
        + streamEntry(R"j("id":3001,"title":"Burial - Shutta - (Ravver Remix)","track_num":null,"duration":183.429)j")
        + "]}");
}

class UrlTests : public QObject
{
    Q_OBJECT

private slots:
    void acceptedUrls()
    {
        const auto discography = parseBandcampPageUrl(u"https://dither.bandcamp.com"_s);
        QVERIFY(discography.isValid());
        QCOMPARE(discography.kind, BandcampPageKind::Discography);
        QCOMPARE(discography.url, u"https://dither.bandcamp.com/music"_s);

        const auto album = parseBandcampPageUrl(u"  https://offland.bandcamp.com/album/parallels?from=x#y  "_s);
        QVERIFY(album.isValid());
        QCOMPARE(album.kind, BandcampPageKind::Album);
        QCOMPARE(album.url, u"https://offland.bandcamp.com/album/parallels"_s);

        const auto track = parseBandcampPageUrl(u"https://ravver.bandcamp.com/track/burial-shutta-ravver-remix"_s);
        QVERIFY(track.isValid());
        QCOMPARE(track.kind, BandcampPageKind::Track);

        // Custom domains are real Bandcamp hosts, so they are accepted.
        const auto custom = parseBandcampPageUrl(u"https://music.example.org/album/foo"_s);
        QVERIFY(custom.isValid());
        QCOMPARE(custom.kind, BandcampPageKind::Album);
    }

    void rejectedUrls()
    {
        QVERIFY(!parseBandcampPageUrl(u"not a url"_s).isValid());
        QVERIFY(!parseBandcampPageUrl(u"https://example.com/foo"_s).isValid());
        QVERIFY(!parseBandcampPageUrl(u"ftp://offland.bandcamp.com/album/parallels"_s).isValid());
        QVERIFY(!parseBandcampPageUrl(u"https://user:pw@offland.bandcamp.com/album/parallels"_s).isValid());
        QVERIFY(!parseBandcampPageUrl({}).isValid());
        QVERIFY(!parseBandcampPageUrl(QString(3000, u'a')).isValid());
    }
};

class MutationTests : public QObject
{
    Q_OBJECT

private slots:
    void truncatedPage()
    {
        const QByteArray body = sampleAlbumPage();
        // Cut inside the data-tralbum attribute so the JSON is incomplete.
        const qsizetype attr = body.indexOf("data-tralbum=");
        QVERIFY(attr > 0);
        QVERIFY(!parseAlbumPage(body.left(attr + 200), AlbumUrl).isValid());
        QVERIFY(!parseAlbumPage(body.left(attr + 13), AlbumUrl).isValid());
    }

    void invalidJson()
    {
        QVERIFY(!parseAlbumPage(tralbumPage(R"({"current":{"type":"album",)"), AlbumUrl).isValid());
        QVERIFY(!parseAlbumPage(tralbumPage(R"([1,2,3])"), AlbumUrl).isValid());
    }

    void differingRootIsWarnedAndCurrentKept()
    {
        const QByteArray body = tralbumPage(
            R"({"current":{"type":"album","title":"T","artist":"Current Artist"},"artist":"Root Artist","trackinfo":[]})");
        const auto result = parseAlbumPage(body, AlbumUrl);
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.artist, u"Current Artist"_s);
        QVERIFY(result.warnings.contains(
            u"current/artist (Current Artist) differs from root/artist (Root Artist), keeping current"_s));
    }

    void emptyCurrentFallsBackToRootWithoutWarning()
    {
        const QByteArray body
            = tralbumPage(R"({"current":{"type":"album","title":"T"},"artist":"Root Artist","trackinfo":[]})");
        const auto result = parseAlbumPage(body, AlbumUrl);
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.artist, u"Root Artist"_s);
        QVERIFY(result.warnings.filter(u"differs"_s).isEmpty());
    }

    void mutatedPagesNeverCrash()
    {
        bool ok        = false;
        int iterations = qEnvironmentVariableIntValue("BANDCAMP_MUTATION_ITERATIONS", &ok);
        if(!ok || iterations <= 0) {
            iterations = DefaultMutationIterations;
        }

        // Parser warnings would flood the output across this many iterations.
        QLoggingCategory::setFilterRules(u"fy.bandcamp.*=false"_s);

        std::mt19937 rng(MutationSeed);
        runAlbumMutations(sampleAlbumPage(), iterations, rng);
        runTrackMutations(sampleTrackWithAlbumPage(), TrackWithAlbumUrl, iterations, rng);
        runTrackMutations(sampleStandaloneTrackPage(), StandaloneTrackUrl, iterations, rng);

        QLoggingCategory::setFilterRules({});
    }

private:
    static QByteArray mutate(QByteArray data, std::mt19937& rng)
    {
        static const char* const Utf8Samples[] = {"\xC3\xA9", "\xF0\x9F\x8E\xB5", "\xED\xA0\x80", "\xFF", "\xE2\x82"};

        const int count = 1 + static_cast<int>(rng() % 3U);
        for(int m = 0; m < count && !data.isEmpty(); ++m) {
            const auto pos = static_cast<qsizetype>(rng() % static_cast<std::uint32_t>(data.size()));
            switch(rng() % 5U) {
                case 0:
                    data[pos] = static_cast<char>(rng() & 0xFFU);
                    break;
                case 1:
                    data.truncate(pos);
                    break;
                case 2: {
                    const auto length = static_cast<qsizetype>(1 + rng() % 64U);
                    data.insert(pos, data.mid(pos, length));
                    break;
                }
                case 3:
                    data.insert(pos, QByteArray(1, '\0'));
                    break;
                default:
                    data.insert(pos, Utf8Samples[rng() % std::size(Utf8Samples)]);
                    break;
            }
        }
        return data;
    }

    static void runAlbumMutations(const QByteArray& fixture, int iterations, std::mt19937& rng)
    {
        for(int i = 0; i < iterations; ++i) {
            const auto result = parseAlbumPage(mutate(fixture, rng), AlbumUrl);
            if(!result.isValid()) {
                continue;
            }
            QVERIFY2(result.value.title.size() <= 1024, qPrintable(QStringLiteral("iteration %1").arg(i)));
            for(const auto& track : result.value.tracks) {
                QVERIFY2(track.durationMs >= 0 && track.durationMs <= MaxTrackDurationMs,
                         qPrintable(QStringLiteral("iteration %1").arg(i)));
            }
        }
    }

    static void runTrackMutations(const QByteArray& fixture, const QUrl& url, int iterations, std::mt19937& rng)
    {
        for(int i = 0; i < iterations; ++i) {
            const auto result = parseTrackPage(mutate(fixture, rng), url);
            if(!result.isValid()) {
                continue;
            }
            QVERIFY2(result.value.durationMs >= 0 && result.value.durationMs <= MaxTrackDurationMs,
                     qPrintable(QStringLiteral("iteration %1").arg(i)));
        }
    }
};

class SettingsTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
    }

    void defaultsWhenMissing()
    {
        QSettings settings(m_dir.filePath(u"missing.ini"_s), QSettings::IniFormat);
        const auto values = loadBandcampSettings(settings);
        QCOMPARE(values.parallelism, DefaultParallelism);
        QCOMPARE(values.itemTimeoutMs, DefaultItemTimeoutMs);
    }

    void roundTrip()
    {
        const auto path = m_dir.filePath(u"roundtrip.ini"_s);
        {
            QSettings settings(path, QSettings::IniFormat);
            saveBandcampSettings(settings, {5, 45000});
        }
        QSettings settings(path, QSettings::IniFormat);
        const auto values = loadBandcampSettings(settings);
        QCOMPARE(values.parallelism, 5);
        QCOMPARE(values.itemTimeoutMs, 45000);
    }

    void outOfRangeIsClamped()
    {
        QSettings settings(m_dir.filePath(u"clamp.ini"_s), QSettings::IniFormat);
        settings.setValue(ParallelismKey, 0);
        settings.setValue(ItemTimeoutKey, 500);
        auto low = loadBandcampSettings(settings);
        QCOMPARE(low.parallelism, MinParallelism);
        QCOMPARE(low.itemTimeoutMs, MinItemTimeoutMs);

        settings.setValue(ParallelismKey, 99);
        settings.setValue(ItemTimeoutKey, 999999999999LL);
        auto high = loadBandcampSettings(settings);
        QCOMPARE(high.parallelism, MaxParallelism);
        QCOMPARE(high.itemTimeoutMs, MaxItemTimeoutMs);
    }

    void nonNumericFallsBackToDefault()
    {
        QSettings settings(m_dir.filePath(u"junk.ini"_s), QSettings::IniFormat);
        settings.setValue(ParallelismKey, u"many"_s);
        settings.setValue(ItemTimeoutKey, u""_s);
        const auto values = loadBandcampSettings(settings);
        QCOMPARE(values.parallelism, DefaultParallelism);
        QCOMPARE(values.itemTimeoutMs, DefaultItemTimeoutMs);
    }

private:
    QTemporaryDir m_dir;
};
} // namespace

// Defined in bandcamppostprocessortests.cpp.
int runPostProcessorTests(int argc, char** argv);

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    int status = 0;
    UrlTests url;
    status |= QTest::qExec(&url, argc, argv);
    status |= runPostProcessorTests(argc, argv);
    MutationTests mutation;
    status |= QTest::qExec(&mutation, argc, argv);
    SettingsTests settings;
    status |= QTest::qExec(&settings, argc, argv);
    return status;
}

#include "bandcamptests.moc"
