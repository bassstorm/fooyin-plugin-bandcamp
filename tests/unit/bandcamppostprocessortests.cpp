#include "bandcamppostprocessor.h"

#include "bandcampdirtytitle.h"
#include "bandcampparser.h"
#include "bandcampprocessors.h"
#include "bandcamptestpages.h"

#include <QtTest>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QUrl AlbumUrl{u"https://offland.bandcamp.com/album/parallels"_s};
const QUrl StandaloneTrackUrl{u"https://ravver.bandcamp.com/track/burial-shutta-ravver-remix"_s};

BandcampTrack rawTrack(qint64 id, const QString& title, const QString& artist = {}, int trackNumber = 0)
{
    BandcampTrack track;
    track.id          = id;
    track.title       = title;
    track.artist      = artist;
    track.trackNumber = trackNumber;
    return track;
}

BandcampAlbum albumWith(const QString& title = {}, const QString& artist = {}, QList<BandcampTrack> tracks = {})
{
    BandcampAlbum album;
    album.title  = title;
    album.artist = artist;
    album.tracks = std::move(tracks);
    return album;
}

// Runs a track processor on every track of the album, with the album as parent.
template <typename Processor>
int runOnTracks(const Processor& processor, BandcampAlbum& album, QStringList& warnings)
{
    int changes = 0;
    for(BandcampTrack& track : album.tracks) {
        changes += processor.process(track, &album, warnings);
    }
    return changes;
}

// Parses an album page and post-processes it, the same way the service does.
BandcampAlbumResult processedAlbum(const QByteArray& body)
{
    auto result = parseAlbumPage(body, AlbumUrl);
    if(result.isValid()) {
        postProcessAlbum(result.value, result.warnings);
    }
    return result;
}

// Parses a track page and post-processes it, the same way the service does.
BandcampTrackResult processedTrack(const QByteArray& body, const QUrl& url)
{
    auto result = parseTrackPage(body, url);
    if(result.isValid()) {
        postProcessTrack(result.value, result.warnings);
    }
    return result;
}

class PostProcessorTests : public QObject
{
    Q_OBJECT

private slots:
    void releaseArtistInheritedByTracks()
    {
        BandcampAlbum album = albumWith({}, u"Off Land"_s, {rawTrack(1, u"A"_s)});
        QStringList warnings;
        runOnTracks(ReleaseFieldsProcessor{}, album, warnings);
        QCOMPARE(album.tracks.at(0).albumArtist, u"Off Land"_s);
        QVERIFY(warnings.isEmpty());
    }

    void releaseDateInheritedByTracksWithoutOne()
    {
        BandcampAlbum album         = albumWith({}, {}, {rawTrack(1, u"A"_s), rawTrack(2, u"B"_s)});
        album.releaseDate           = QDate(2024, 5, 1);
        album.tracks[1].releaseDate = QDate(2020, 1, 1);
        QStringList warnings;
        runOnTracks(ReleaseFieldsProcessor{}, album, warnings);
        QCOMPARE(album.tracks.at(0).releaseDate, QDate(2024, 5, 1));
        QCOMPARE(album.tracks.at(1).releaseDate, QDate(2020, 1, 1));
    }

    void releaseFieldsKeepsTrackDateWhenAlbumDiffers()
    {
        BandcampAlbum album         = albumWith({}, {}, {rawTrack(1, u"A"_s)});
        album.releaseDate           = QDate(2020, 1, 1);
        album.tracks[0].releaseDate = QDate(2024, 5, 1);
        QStringList warnings;
        QCOMPARE(ReleaseFieldsProcessor{}.process(album.tracks[0], &album, warnings), 0);
        QCOMPARE(album.tracks[0].releaseDate, QDate(2024, 5, 1));
    }

    void playableTracksRemoveStreamlessTracksWithWarning()
    {
        BandcampAlbum album       = albumWith({}, {}, {rawTrack(1, u"A"_s), rawTrack(2, u"B"_s)});
        album.tracks[0].streamUrl = u"https://cdn.example.com/a.mp3"_s;
        QStringList warnings;
        QCOMPARE(PlayableTracksProcessor{}.process(album, warnings), 1);
        QCOMPARE(album.tracks.size(), qsizetype{1});
        QCOMPARE(album.tracks.at(0).id, qint64{1});
        QCOMPARE(warnings, QStringList{u"track 2 has no usable stream, skipped"_s});
    }

    void playableTracksWarnWhenAlbumEmpty()
    {
        BandcampAlbum album = albumWith({}, {}, {rawTrack(1, u"A"_s)});
        QStringList warnings;
        PlayableTracksProcessor{}.process(album, warnings);
        QVERIFY(album.tracks.isEmpty());
        QVERIFY(warnings.contains(u"album has no playable tracks"_s));
    }

    void artistInferenceNeedsEveryTitleDirty()
    {
        BandcampAlbum album = albumWith({}, {}, {rawTrack(1, u"Off Land - A"_s), rawTrack(2, u"B"_s)});
        QStringList warnings;
        QCOMPARE(ArtistInferenceProcessor{}.process(album, warnings), 0);
        QVERIFY(album.tracks.at(0).artist.isEmpty());
        QVERIFY(warnings.isEmpty());
    }

    void artistInferenceSetsArtistFromTitles()
    {
        BandcampAlbum album = albumWith({}, {}, {rawTrack(1, u"Off Land - A"_s), rawTrack(2, u"Off Land - B"_s)});
        QStringList warnings;
        QCOMPARE(ArtistInferenceProcessor{}.process(album, warnings), 2);
        for(const BandcampTrack& track : std::as_const(album.tracks)) {
            QCOMPARE(track.artist, u"Off Land"_s);
        }
    }

    void artistInferenceSkipsWhenArtistReported()
    {
        BandcampAlbum album
            = albumWith({}, {}, {rawTrack(1, u"Off Land - A"_s, u"Off Land"_s), rawTrack(2, u"Off Land - B"_s)});
        QStringList warnings;
        QCOMPARE(ArtistInferenceProcessor{}.process(album, warnings), 0);
        QVERIFY(album.tracks.at(1).artist.isEmpty());
    }

    void releaseArtistFallbackOnlyFillsMissing()
    {
        BandcampAlbum album = albumWith({}, u"Off Land"_s, {rawTrack(1, u"A"_s), rawTrack(2, u"B"_s, u"Other"_s)});
        QStringList warnings;
        QCOMPARE(runOnTracks(ReleaseArtistFallbackProcessor{}, album, warnings), 1);
        QCOMPARE(album.tracks.at(0).artist, u"Off Land"_s);
        QCOMPARE(album.tracks.at(1).artist, u"Other"_s);
    }

    void releaseArtistFallbackNoopWithoutReleaseArtist()
    {
        BandcampAlbum album = albumWith({}, {}, {rawTrack(1, u"A"_s)});
        QStringList warnings;
        QCOMPARE(runOnTracks(ReleaseArtistFallbackProcessor{}, album, warnings), 0);
        QVERIFY(album.tracks.at(0).artist.isEmpty());
    }

    void titlePrefixRemovesMatchingArtistOnly()
    {
        BandcampAlbum album = albumWith(
            {}, {},
            {rawTrack(1, u"off land - Ingressive"_s, u"Off Land"_s), rawTrack(2, u"Other - Remix"_s, u"Off Land"_s)});
        QStringList warnings;
        QCOMPARE(runOnTracks(TrackTitlePrefixProcessor{}, album, warnings), 1);
        QCOMPARE(album.tracks.at(0).title, u"Ingressive"_s);
        QCOMPARE(album.tracks.at(1).title, u"Other - Remix"_s);
    }

    void titlePrefixSplitsStandaloneTrackTitles()
    {
        BandcampTrack track = rawTrack(1, u"Off Land - A"_s, u"Off Land"_s);
        QStringList warnings;
        QCOMPARE(TrackTitlePrefixProcessor{}.process(track, nullptr, warnings), 1);
        QCOMPARE(track.title, u"A"_s);
    }

    void titlePrefixLeavesUnrelatedTrackTitles()
    {
        BandcampTrack track = rawTrack(1, u"Ravver - A"_s, u"Off Land"_s);
        QStringList warnings;
        QCOMPARE(TrackTitlePrefixProcessor{}.process(track, nullptr, warnings), 0);
        QCOMPARE(track.title, u"Ravver - A"_s);
    }

    void standaloneTrackPrefixUsesArtistFromPageArtist()
    {
        // The page artist (albumArtist on a standalone track) fills the missing track artist first,
        // so the title prefix can be recognised.
        BandcampTrack track = rawTrack(1, u"Ravver - Track"_s);
        track.albumArtist   = u"Ravver"_s;
        QStringList warnings;
        QCOMPARE(ReleaseArtistFallbackProcessor{}.process(track, nullptr, warnings), 1);
        QCOMPARE(TrackTitlePrefixProcessor{}.process(track, nullptr, warnings), 1);
        QCOMPARE(track.title, u"Track"_s);
    }

    void dirtyTitleSplitsOnArtistSeparator()
    {
        const DirtyTitle split = splitDirtyTitle(u"Off Land - Ingressive"_s);
        QVERIFY(split.matched);
        QCOMPARE(split.artist, u"Off Land"_s);
        QCOMPARE(split.title, u"Ingressive"_s);

        // The artist ends at the first separator, so later separators stay in the title.
        const DirtyTitle nested = splitDirtyTitle(u"Ravver - Track - Remix"_s);
        QCOMPARE(nested.artist, u"Ravver"_s);
        QCOMPARE(nested.title, u"Track - Remix"_s);

        QVERIFY(!splitDirtyTitle(u"NoSeparator"_s).matched);
        QVERIFY(!splitDirtyTitle(u"A-B"_s).matched);
        QVERIFY(sameArtist(u"off land"_s, u"Off Land"_s));
        QVERIFY(!sameArtist(u"Off Land"_s, u"Ravver"_s));
    }

    void missingTitleWarnsAndKeepsTitleEmpty()
    {
        BandcampAlbum album = albumWith({}, u"Off Land"_s, {rawTrack(7, {})});
        QStringList warnings;
        QCOMPARE(AlbumMissingTitleProcessor{}.process(album, warnings), 0);
        QCOMPARE(runOnTracks(TrackMissingTitleProcessor{}, album, warnings), 0);
        QVERIFY(album.tracks.at(0).title.isEmpty());
        QCOMPARE(warnings, (QStringList{u"album has no title"_s, u"track 7 has no title"_s}));
    }

    void missingTitleAppliesToTrackPages()
    {
        BandcampTrack track = rawTrack(7, {});
        QStringList warnings;
        QCOMPARE(TrackMissingTitleProcessor{}.process(track, nullptr, warnings), 0);
        QVERIFY(track.title.isEmpty());
        QCOMPARE(warnings, QStringList{u"track 7 has no title"_s});
    }

    void trackNumberRepairKeepsValidNumbers()
    {
        BandcampAlbum album
            = albumWith({}, {}, {rawTrack(1, u"A"_s, {}, 2), rawTrack(2, u"B"_s, {}, 2), rawTrack(3, u"C"_s)});
        QStringList warnings;
        QCOMPARE(TrackNumberRepairProcessor{}.process(album, warnings), 2);
        QCOMPARE(album.tracks.at(0).trackNumber, 2);
        QCOMPARE(album.tracks.at(1).trackNumber, 3);
        QCOMPARE(album.tracks.at(2).trackNumber, 4);
        QCOMPARE(warnings,
                 (QStringList{u"track 2 number 2 is repeated, using 3"_s, u"track 3 has no number, using 4"_s}));
    }

    void trackNumberRepairFillsGap()
    {
        // The example from the plan: 1 2 null 4 5 becomes 1 2 3 4 5.
        BandcampAlbum album = albumWith({}, {},
                                        {rawTrack(1, u"A"_s, {}, 1), rawTrack(2, u"B"_s, {}, 2), rawTrack(3, u"C"_s),
                                         rawTrack(4, u"D"_s, {}, 4), rawTrack(5, u"E"_s, {}, 5)});
        QStringList warnings;
        QCOMPARE(TrackNumberRepairProcessor{}.process(album, warnings), 1);
        for(qsizetype i = 0; i < album.tracks.size(); ++i) {
            QCOMPARE(album.tracks.at(i).trackNumber, static_cast<int>(i + 1));
        }
        QCOMPARE(warnings, QStringList{u"track 3 has no number, using 3"_s});
    }

    void trackNumberRepairCountsFromOneWithoutNumbers()
    {
        BandcampAlbum album = albumWith({}, {}, {rawTrack(1, u"A"_s), rawTrack(2, u"B"_s)});
        QStringList warnings;
        QCOMPARE(TrackNumberRepairProcessor{}.process(album, warnings), 2);
        QCOMPARE(album.tracks.at(0).trackNumber, 1);
        QCOMPARE(album.tracks.at(1).trackNumber, 2);
    }

    void pipelineInfersArtistBeforeStrippingPrefix()
    {
        // Inference has to run before the title prefix is stripped, because the prefix is only removed
        // once the artist is known.
        BandcampAlbum album = albumWith({}, {}, {rawTrack(1, u"Off Land - A"_s), rawTrack(2, u"Off Land - B"_s)});
        for(BandcampTrack& track : album.tracks) {
            track.streamUrl = u"https://cdn.example.com/stream.mp3"_s;
        }
        QStringList warnings;
        postProcessAlbum(album, warnings);
        QCOMPARE(album.tracks.at(0).artist, u"Off Land"_s);
        QCOMPARE(album.tracks.at(0).title, u"A"_s);
        QCOMPARE(album.tracks.at(1).artist, u"Off Land"_s);
        QCOMPARE(album.tracks.at(1).title, u"B"_s);
    }

    void albumArtistAndTitleStripFromParsedPage()
    {
        // Mixed titles: no inference, so the album artist is used and only matching prefixes are removed.
        const auto result
            = processedAlbum(tralbumPage(R"J({"current":{"type":"album","title":"Parallels"},"artist":"Off Land",)J"
                                         R"J("trackinfo":[)J"
                                         + streamEntry(R"("id":1,"title":"Off Land - Ingressive")") + ","
                                         + streamEntry(R"("id":2,"title":"Crossing")") + ","
                                         + streamEntry(R"("id":3,"title":"Someone Else - Remix")") + "]}"));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.tracks.at(0).artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(0).title, u"Ingressive"_s);
        QCOMPARE(result.value.tracks.at(1).artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(1).title, u"Crossing"_s);
        QCOMPARE(result.value.tracks.at(2).artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(2).title, u"Someone Else - Remix"_s);
    }

    void artistInferredFromParsedPage()
    {
        const auto result = processedAlbum(albumPage(R"({"type":"album","title":"Parallels"})",
                                                     {streamEntry(R"("id":1,"title":"Off Land - Ingressive")"),
                                                      streamEntry(R"("id":2,"title":"Off Land - Crossing")")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.tracks.at(0).artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(0).title, u"Ingressive"_s);
        QCOMPARE(result.value.tracks.at(1).artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(1).title, u"Crossing"_s);
    }

    void explicitArtistPrefixStrippedFromParsedPage()
    {
        const auto result
            = processedAlbum(albumPage(R"({"type":"album","title":"Parallels","artist":"Off Land"})",
                                       {streamEntry(R"("id":1,"title":"off land - Ingressive","artist":"Off Land")"),
                                        streamEntry(R"("id":2,"title":"Off Land - Crossing","artist":"Off Land")")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.tracks.at(0).title, u"Ingressive"_s);
        QCOMPARE(result.value.tracks.at(0).artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(1).title, u"Crossing"_s);
    }

    void trackNumbersRecoveredFromParsedPage()
    {
        const auto result = processedAlbum(
            albumPage(R"({"type":"album","title":"X"})",
                      {streamEntry(R"("id":1,"title":"A","track_num":2)"),
                       streamEntry(R"("id":2,"title":"B","track_num":2)"), streamEntry(R"("id":3,"title":"C")")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.tracks.at(0).trackNumber, 2);
        QCOMPARE(result.value.tracks.at(1).trackNumber, 3);
        QCOMPARE(result.value.tracks.at(2).trackNumber, 4);
        QVERIFY(result.warnings.contains(u"track 2 number 2 is repeated, using 3"_s));
        QVERIFY(result.warnings.contains(u"track 3 has no number, using 4"_s));
    }

    void trackNumberGapFilledFromParsedPage()
    {
        const auto result = processedAlbum(albumPage(
            R"({"type":"album","title":"X"})",
            {streamEntry(R"("id":1,"title":"A","track_num":1)"), streamEntry(R"("id":2,"title":"B","track_num":2)"),
             streamEntry(R"("id":3,"title":"C")"), streamEntry(R"("id":4,"title":"D","track_num":4)"),
             streamEntry(R"("id":5,"title":"E","track_num":5)")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.tracks.size(), qsizetype{5});
        for(qsizetype i = 0; i < result.value.tracks.size(); ++i) {
            QCOMPARE(result.value.tracks.at(i).trackNumber, static_cast<int>(i + 1));
        }
        QCOMPARE(result.warnings.filter(u"number"_s).size(), 1);
        QVERIFY(result.warnings.contains(u"album release date is missing"_s));
    }

    void missingNumbersCountFromOneFromParsedPage()
    {
        const auto result = processedAlbum(
            albumPage(R"({"type":"album","title":"X"})",
                      {streamEntry(R"("id":1,"title":"A")"), streamEntry(R"("id":2,"title":"B","track_num":null)")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.tracks.at(0).trackNumber, 1);
        QCOMPARE(result.value.tracks.at(1).trackNumber, 2);
    }

    void missingTitleStandaloneTrackFromParsedPage()
    {
        const auto result = processedTrack(
            tralbumPage(R"({"current":{"type":"track"},"trackinfo":[)" + streamEntry(R"("id":9)") + "]}"),
            StandaloneTrackUrl);
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(result.value.title.isEmpty());
        QVERIFY(result.warnings.contains(u"track 9 has no title"_s));
    }

    void standaloneTrackArtistComesFromPageArtist()
    {
        // Track pages have no album artist. The root "artist" is the track's own artist, and the
        // fallback has to fill the track from it when trackinfo carries none.
        const auto result = processedTrack(
            tralbumPage(R"J({"current":{"type":"track","title":"Shutta (Ravver Remix)"},"artist":"Ravver",)J"
                        R"J("trackinfo":[)J"
                        + streamEntry(R"("id":9,"artist":null)") + "]}"),
            StandaloneTrackUrl);
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.artist, u"Ravver"_s);
        QCOMPARE(result.value.albumArtist, u"Ravver"_s);
    }

    void albumDateTakenFromCurrentReleaseDate()
    {
        const auto result = processedAlbum(
            tralbumPage(R"J({"current":{"type":"album","title":"Parallels","release_date":"1 Mar 2010 00:00:00 GMT"},)J"
                        R"J("album_release_date":"15 Jan 2016 00:00:00 GMT","trackinfo":[)J"
                        + streamEntry(R"("id":1,"title":"A","track_num":1)") + "]}"));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.releaseDate, QDate(2010, 3, 1));
        QCOMPARE(result.value.tracks.at(0).releaseDate, QDate(2010, 3, 1));
    }

    void albumArtistPrefersCurrentArtist()
    {
        const auto result = processedAlbum(
            tralbumPage(R"J({"current":{"type":"album","title":"Parallels","artist":"Off Land"},"artist":"Ravver",)J"
                        R"J("trackinfo":[)J"
                        + streamEntry(R"("id":1,"title":"A","track_num":1)") + "]}"));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.artist, u"Off Land"_s);
    }

    void albumDateFallsBackToRootReleaseDate()
    {
        const auto result = processedAlbum(tralbumPage(
            R"J({"current":{"type":"album","title":"Parallels"},"album_release_date":"15 Jan 2016 00:00:00 GMT",)J"
            R"J("trackinfo":[)J"
            + streamEntry(R"("id":1,"title":"A","track_num":1)") + "]}"));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.releaseDate, QDate(2016, 1, 15));
        QVERIFY(result.warnings.isEmpty());
    }

    void albumDateMissingWarns()
    {
        const auto result = processedAlbum(
            albumPage(R"({"type":"album","title":"Parallels"})", {streamEntry(R"("id":1,"title":"A","track_num":1)")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QVERIFY(!result.value.releaseDate.isValid());
        QVERIFY(!result.value.tracks.at(0).releaseDate.isValid());
        QVERIFY(result.warnings.contains(u"album release date is missing"_s));
    }

    void albumArtistInferredFromSharedTrackArtist()
    {
        const auto result
            = processedAlbum(albumPage(R"({"type":"album","title":"Parallels"})",
                                       {streamEntry(R"("id":1,"title":"Ingressive","artist":"Off Land")"),
                                        streamEntry(R"("id":2,"title":"Crossing","artist":"off land")")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.artist, u"Off Land"_s);
        QCOMPARE(result.value.tracks.at(0).albumArtist, u"Off Land"_s);
    }

    void albumTracksUseAlbumSourceUrl()
    {
        const auto result = processedAlbum(
            albumPage(R"({"type":"album","title":"Parallels"})", {streamEntry(R"("id":1,"title":"A","track_num":1)")}));
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.sourceUrl, AlbumUrl.toString());
        QCOMPARE(result.value.tracks.at(0).sourceUrl, AlbumUrl.toString());
    }

    void trackWithAlbumSourcesFromAlbumUrl()
    {
        const QUrl trackUrl{u"https://offland.bandcamp.com/track/caaa-baaa-aaa"_s};
        const auto result = processedTrack(
            tralbumPage(
                R"J({"current":{"type":"track","title":"Caaa Baaa Aaa","artist":"Off Land",)J"
                R"J("release_date":"15 Jan 2016 00:00:00 GMT"},)J"
                R"J("album_url":"https://offland.bandcamp.com/album/parallels","artist":"Off Land","trackinfo":[)J"
                + streamEntry(R"("id":1,"title":"Caaa Baaa Aaa")") + "]}"),
            trackUrl);
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.sourceUrl, AlbumUrl.toString());
        QCOMPARE(result.value.releaseDate, QDate(2016, 1, 15));
    }

    void standaloneTrackSourcesFromOwnUrl()
    {
        const auto result = processedTrack(
            tralbumPage(R"J({"current":{"type":"track","title":"Shutta","artist":"Ravver"},"album_url":null,)J"
                        R"J("artist":"Ravver","trackinfo":[)J"
                        + streamEntry(R"("id":9,"title":"Shutta")") + "]}"),
            StandaloneTrackUrl);
        QVERIFY2(result.isValid(), qPrintable(result.error));
        QCOMPARE(result.value.sourceUrl, StandaloneTrackUrl.toString());
    }

    void trackDateTakenFromAlbumWhenMissing()
    {
        const auto album
            = processedAlbum(tralbumPage(R"J({"current":{"type":"album","title":"Parallels"},)J"
                                         R"J("album_release_date":"15 Jan 2016 00:00:00 GMT","trackinfo":[)J"
                                         + streamEntry(R"("id":1,"title":"A","track_num":1)") + "]}"));
        QVERIFY2(album.isValid(), qPrintable(album.error));

        const QUrl trackUrl{u"https://offland.bandcamp.com/track/a"_s};
        auto track = processedTrack(
            tralbumPage(R"J({"current":{"type":"track","title":"A","artist":"Off Land"},)J"
                        R"J("album_url":"https://offland.bandcamp.com/album/parallels","artist":"Off Land",)J"
                        R"J("trackinfo":[)J"
                        + streamEntry(R"("id":1,"title":"A")") + "]}"),
            trackUrl);
        QVERIFY2(track.isValid(), qPrintable(track.error));
        QVERIFY(!track.value.releaseDate.isValid());

        QStringList warnings;
        postProcessAlbumTrack(track.value, album.value, warnings);
        QCOMPARE(track.value.releaseDate, QDate(2016, 1, 15));
    }
};
} // namespace

int runPostProcessorTests(int argc, char** argv)
{
    PostProcessorTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcamppostprocessortests.moc"
