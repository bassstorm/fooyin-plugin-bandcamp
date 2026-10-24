#include "bandcampassembly.h"
#include "bandcamplogcapture.h"
#include "bandcampmodel.h"
#include "bandcampparser.h"
#include "bandcamppostprocessor.h"
#include "bandcampprocessors.h"
#include "bandcamptestpages.h"

#include <QElapsedTimer>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QUrl AlbumUrl{u"https://artist.bandcamp.com/album/release"_s};
const QUrl TrackUrl{u"https://artist.bandcamp.com/track/recording"_s};
const QUrl OtherTrackUrl{u"https://artist.bandcamp.com/track/other"_s};
const QString ChildStream{u"https://cdn.example.com/parent-stream.mp3?sig=child"_s};
const QString PageStream{u"https://cdn.example.com/page-stream.mp3?sig=page"_s};

template <typename T>
CandidateField<T> explicitValue(T value)
{
    return CandidateField<T>::usable(std::move(value), FieldProvenance::Explicit);
}

AlbumCandidate albumCandidate(qint64 id = 40, QString title = u"Album"_s, QList<TrackCandidate> tracks = {})
{
    AlbumCandidate album;
    album.id          = explicitValue(id);
    album.sourceUrl   = explicitValue(AlbumUrl);
    album.title       = explicitValue(std::move(title));
    album.artist      = explicitValue(QStringLiteral("Album artist"));
    album.releaseDate = explicitValue(QDate(2020, 1, 1));
    album.artId       = explicitValue(qint64{80});
    album.tracks      = std::move(tracks);
    return album;
}

TrackCandidate child(qint64 id, QString title, QString stream = ChildStream)
{
    TrackCandidate candidate;
    candidate.recordState = FieldState::Usable;
    candidate.id          = explicitValue(id);
    candidate.title       = explicitValue(std::move(title));
    candidate.artist      = explicitValue(QStringLiteral("Album artist"));
    candidate.trackNumber = explicitValue(qint64{1});
    candidate.durationMs  = explicitValue(qint64{120000});
    candidate.streamUrl   = explicitValue(std::move(stream));
    return candidate;
}

TrackPageCandidate trackPage(qint64 id = 1)
{
    TrackPageCandidate page;
    page.sourceUrl          = explicitValue(TrackUrl);
    page.track.recordState  = FieldState::Usable;
    page.track.id           = explicitValue(id);
    page.track.title        = explicitValue(QStringLiteral("Page title"));
    page.track.artist       = explicitValue(QStringLiteral("Page artist"));
    page.track.trackNumber  = explicitValue(qint64{7});
    page.track.durationMs   = explicitValue(qint64{150000});
    page.track.streamUrl    = explicitValue(PageStream);
    page.matchingRecord     = CandidateField<qsizetype>::usable(0, FieldProvenance::Explicit);
    page.parent.declaration = ParentDeclaration::AlbumBacked;
    page.parent.albumId     = explicitValue(qint64{40});
    page.parent.albumUrl    = explicitValue(AlbumUrl);
    return page;
}

template <typename T>
const ResolutionError* failure(const T& outcome)
{
    const auto* failed = std::get_if<ResolutionFailure>(&outcome);
    return failed ? &failed->error : nullptr;
}

BandcampTrack trackValue(qint64 id, QString title, QString artist = {}, int number = 0)
{
    BandcampTrack track;
    track.id          = id;
    track.title       = std::move(title);
    track.artist      = std::move(artist);
    track.trackNumber = number;
    track.sourceUrl   = AlbumUrl.toString();
    track.streamUrl   = ChildStream;
    return track;
}

void verifyParserNullOptionals()
{
    const auto parsedAlbum = parseReleasePage(
        albumPage(R"({"type":"album","id":40,"title":"Album","artist":null,"release_date":null,"art_id":null})",
                  {streamEntry(R"("id":1,"title":"Song","artist":null,"track_num":null,"duration":null)")}),
        AlbumUrl);
    const auto* albumSuccess = std::get_if<ResolutionSuccess<ReleaseCandidate>>(&parsedAlbum);
    QVERIFY(albumSuccess);
    const auto* album = std::get_if<AlbumCandidate>(&albumSuccess->value);
    QVERIFY(album);
    QCOMPARE(album->artist.state, FieldState::Invalid);
    QCOMPARE(album->releaseDate.state, FieldState::Invalid);
    QCOMPARE(album->artId.state, FieldState::Invalid);
    QCOMPARE(album->tracks.first().artist.state, FieldState::Invalid);
    QCOMPARE(album->tracks.first().trackNumber.state, FieldState::Invalid);
    QCOMPARE(album->tracks.first().durationMs.state, FieldState::Invalid);

    auto admittedAlbum = admitAlbum(*album);
    QVERIFY(isSuccess(admittedAlbum));
    auto albumAdmission = std::get<ResolutionSuccess<AlbumAdmission>>(admittedAlbum).value;
    postProcessAlbum(albumAdmission);
    QVERIFY(albumAdmission.album.metadata.artist.isEmpty());
    QVERIFY(!albumAdmission.album.metadata.releaseDate.isValid());
    QCOMPARE(albumAdmission.album.metadata.artId, qint64{0});
    QVERIFY(albumAdmission.album.tracks.first().artist.isEmpty());
    QCOMPARE(albumAdmission.album.tracks.first().trackNumber, 1);
    QCOMPARE(albumAdmission.album.tracks.first().durationMs, qint64{0});

    const QUrl standaloneUrl{u"https://artist.bandcamp.com/track/solo"_s};
    const auto parsedTrack = parseReleasePage(
        tralbumPage(
            R"({"current":{"type":"track","id":77,"title":"Solo","artist":null,"track_number":null,"release_date":null,"art_id":null,"album_id":null},"artist":null,"art_id":null,"album_url":null,"trackinfo":[{"id":77,"title":"Solo","artist":null,"track_num":null,"duration":null,"file":{"mp3-128":"https://cdn.example.com/solo.mp3?sig=solo"}}]})"),
        standaloneUrl);
    const auto* trackSuccess = std::get_if<ResolutionSuccess<ReleaseCandidate>>(&parsedTrack);
    QVERIFY(trackSuccess);
    const auto* page = std::get_if<TrackPageCandidate>(&trackSuccess->value);
    QVERIFY(page);
    QCOMPARE(page->track.trackNumber.state, FieldState::Invalid);
    QCOMPARE(page->track.releaseDate.state, FieldState::Invalid);
    QCOMPARE(page->track.artId.state, FieldState::Invalid);

    auto admittedTrack = admitStandaloneTrack(*page);
    QVERIFY(isSuccess(admittedTrack));
    auto trackAdmission = std::get<ResolutionSuccess<TrackAdmission>>(admittedTrack).value;
    postProcessTrack(trackAdmission);
    QVERIFY(!trackAdmission.result.album.has_value());
    QVERIFY(!trackAdmission.result.track.releaseDate.isValid());
    QCOMPARE(trackAdmission.result.track.artId, qint64{0});
    QCOMPARE(trackAdmission.result.track.trackNumber, 0);
    QCOMPARE(trackAdmission.result.track.streamUrl, u"https://cdn.example.com/solo.mp3?sig=solo"_s);
}

} // namespace

class AdmissionAndProcessorTests : public QObject
{
    Q_OBJECT

private slots:
    void wholeAlbumAdmitsOnlyValidCompleteMembership()
    {
        auto album        = albumCandidate(40, u"Release"_s, {child(1, u"One"_s), child(2, u"Two"_s)});
        const auto result = admitAlbum(album);
        QVERIFY(isSuccess(result));
        const auto& admitted = std::get<ResolutionSuccess<AlbumAdmission>>(result).value;
        QCOMPARE(admitted.album.metadata.id, qint64{40});
        QCOMPARE(admitted.album.metadata.title, u"Release"_s);
        QCOMPARE(admitted.album.metadata.trackTotal, 2);
        QCOMPARE(admitted.album.tracks.size(), qsizetype{2});
        QCOMPARE(admitted.album.tracks.at(0).streamUrl, ChildStream);
    }

    void albumRejectsMissingRequiredMetadata()
    {
        auto album        = albumCandidate();
        album.title       = CandidateField<QString>::missing();
        const auto result = admitAlbum(album);
        QVERIFY(!isSuccess(result));
        QCOMPARE(failure(result)->code, ResolutionErrorCode::RequiredFieldMissing);
        QCOMPARE(failure(result)->field, u"album.title"_s);
    }

    void invalidOptionalSourceMetadataBecomesUnknown()
    {
        auto album              = albumCandidate(40, u"Album"_s, {child(1, u"One"_s)});
        album.artist            = CandidateField<QString>::invalid();
        album.releaseDate       = CandidateField<QDate>::invalid();
        album.artId             = CandidateField<qint64>::invalid();
        auto& sourceTrack       = album.tracks[0];
        sourceTrack.artist      = CandidateField<QString>::invalid();
        sourceTrack.trackNumber = CandidateField<qint64>::invalid();
        sourceTrack.durationMs  = CandidateField<qint64>::invalid();
        sourceTrack.releaseDate = CandidateField<QDate>::invalid();
        sourceTrack.artId       = CandidateField<qint64>::invalid();

        const auto result = admitAlbum(album);
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<AlbumAdmission>>(result).value;
        postProcessAlbum(admitted);
        QVERIFY(admitted.album.metadata.artist.isEmpty());
        QVERIFY(!admitted.album.metadata.releaseDate.isValid());
        QCOMPARE(admitted.album.metadata.artId, qint64{0});
        QVERIFY(admitted.album.tracks.first().artist.isEmpty());
        QCOMPARE(admitted.album.tracks.first().trackNumber, 1);
        QCOMPARE(admitted.album.tracks.first().durationMs, qint64{0});
        QVERIFY(!admitted.album.tracks.first().releaseDate.isValid());
        QCOMPARE(admitted.album.tracks.first().artId, qint64{0});
    }

    void emptyAlbumFailsExplicitly()
    {
        const auto result = admitAlbum(albumCandidate());
        QVERIFY(!isSuccess(result));
        QCOMPARE(failure(result)->code, ResolutionErrorCode::NoResolvableTracks);
        QCOMPARE(failure(result)->field, u"members"_s);
    }

    void albumFailureIsAtomicAndIdentifiesFirstBadChild()
    {
        auto broken       = child(2, u"Two"_s);
        broken.streamUrl  = CandidateField<QString>::missing();
        const auto result = admitAlbum(albumCandidate(40, u"Album"_s, {child(1, u"One"_s), broken}));
        QVERIFY(!isSuccess(result));
        QCOMPARE(failure(result)->code, ResolutionErrorCode::RequiredFieldMissing);
        QCOMPARE(failure(result)->trackId, qint64{2});
        QCOMPARE(failure(result)->field, u"track.streamUrl"_s);
    }

    void invalidUsableOptionalValuesAreNotProjected()
    {
        auto album              = albumCandidate(40, u"Album"_s, {child(9, u"Nine"_s)});
        album.artist            = explicitValue(QStringLiteral("   "));
        album.releaseDate       = explicitValue(QDate{});
        album.artId             = explicitValue(qint64{-8});
        auto& sourceTrack       = album.tracks[0];
        sourceTrack.artist      = explicitValue(QStringLiteral(" "));
        sourceTrack.trackNumber = explicitValue(qint64{-2});
        sourceTrack.durationMs  = explicitValue(qint64{-1});
        sourceTrack.releaseDate = explicitValue(QDate{});
        sourceTrack.artId       = explicitValue(qint64{-4});

        const auto result = admitAlbum(album);
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<AlbumAdmission>>(result).value;
        postProcessAlbum(admitted);
        QVERIFY(admitted.album.metadata.artist.isEmpty());
        QVERIFY(!admitted.album.metadata.releaseDate.isValid());
        QCOMPARE(admitted.album.metadata.artId, qint64{0});
        QVERIFY(admitted.album.tracks.first().artist.isEmpty());
        QCOMPARE(admitted.album.tracks.first().trackNumber, 1);
        QCOMPARE(admitted.album.tracks.first().durationMs, qint64{0});
        QVERIFY(!admitted.album.tracks.first().releaseDate.isValid());
        QCOMPARE(admitted.album.tracks.first().artId, qint64{0});
    }

    void selectedTrackIgnoresMalformedUnrelatedSibling()
    {
        auto unrelated        = child(2, u"Unavailable"_s);
        unrelated.recordState = FieldState::Invalid;
        unrelated.id          = CandidateField<qint64>::invalid();
        const auto result
            = admitAlbumTrack(trackPage(1), albumCandidate(40, u"Parent"_s, {unrelated, child(1, u"One"_s)}));
        QVERIFY(isSuccess(result));
        const auto& admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        QVERIFY(admitted.result.album.has_value());
        QCOMPARE(admitted.result.album->trackTotal, 2);
        QCOMPARE(admitted.result.track.id, qint64{1});
        QVERIFY(!admitted.completeSiblingSet);
        QVERIFY(admitted.siblings.isEmpty());
    }

    void duplicateUnrelatedChildIdsDisableSiblingRules()
    {
        const auto result = admitAlbumTrack(
            trackPage(1),
            albumCandidate(40, u"Parent"_s,
                           {child(1, u"Selected"_s), child(2, u"Other one"_s), child(2, u"Other two"_s)}));
        QVERIFY(isSuccess(result));
        const auto& admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        QCOMPARE(admitted.result.track.id, qint64{1});
        QVERIFY(!admitted.completeSiblingSet);
        QVERIFY(admitted.siblings.isEmpty());
    }

    void selectedTrackRetainsValidatedParentOnce()
    {
        const auto result = admitAlbumTrack(trackPage(1), albumCandidate(40, u"Parent"_s, {child(1, u"One"_s)}));
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        postProcessTrack(admitted);
        QVERIFY(admitted.result.album.has_value());
        QCOMPARE(admitted.result.album->id, qint64{40});
        QCOMPARE(admitted.result.album->title, u"Parent"_s);
        QCOMPARE(admitted.result.album->trackTotal, 1);
        QCOMPARE(admitted.result.track.sourceUrl, AlbumUrl.toString());
    }

    void explicitAlbumChildFactsPrecedeTrackPageValues()
    {
        auto page               = trackPage();
        page.track.releaseDate  = explicitValue(QDate(2024, 5, 1));
        page.track.artId        = explicitValue(qint64{901});
        auto parentChild        = child(1, u"Album child title"_s);
        parentChild.artist      = explicitValue(QStringLiteral("Child artist"));
        parentChild.trackNumber = explicitValue(qint64{4});
        parentChild.durationMs  = explicitValue(qint64{90000});
        const auto result       = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {parentChild}));
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        QCOMPARE(admitted.result.track.title, u"Album child title"_s);
        QCOMPARE(admitted.result.track.artist, u"Child artist"_s);
        QCOMPARE(admitted.result.track.trackNumber, 4);
        QCOMPARE(admitted.result.track.durationMs, qint64{90000});
        postProcessTrack(admitted);
        QCOMPARE(admitted.result.track.releaseDate, QDate(2024, 5, 1));
        QCOMPARE(admitted.result.track.artId, qint64{901});
    }

    void missingChildFactsMayUseSelectedPage()
    {
        auto parentChild  = child(1, u"_"_s);
        parentChild.title = CandidateField<QString>::missing();
        auto page         = trackPage(1);
        page.track.title  = explicitValue(QStringLiteral("Track-page title"));
        const auto result = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {parentChild}));
        QVERIFY(isSuccess(result));
        QCOMPARE(std::get<ResolutionSuccess<TrackAdmission>>(result).value.result.track.title, u"Track-page title"_s);
    }

    void reconciliationPrecedesRequiredAndOptionalAdmission()
    {
        auto page               = trackPage();
        page.track.artist       = CandidateField<QString>::invalid();
        page.track.trackNumber  = CandidateField<qint64>::invalid();
        page.track.durationMs   = CandidateField<qint64>::invalid();
        auto parentChild        = child(1, u"_"_s);
        parentChild.title       = CandidateField<QString>::invalid();
        parentChild.artist      = explicitValue(QStringLiteral("Album child artist"));
        parentChild.trackNumber = explicitValue(qint64{4});
        parentChild.durationMs  = explicitValue(qint64{90000});

        const auto result = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {parentChild}));
        QVERIFY(isSuccess(result));
        const auto& track = std::get<ResolutionSuccess<TrackAdmission>>(result).value.result.track;
        QCOMPARE(track.title, u"Page title"_s);
        QCOMPARE(track.artist, u"Album child artist"_s);
        QCOMPARE(track.trackNumber, 4);
        QCOMPARE(track.durationMs, qint64{90000});
    }

    void assemblyLogsCrossPageAuthorityWithoutSignedStreams()
    {
        LogCapture capture(u"fy.bandcamp.resolve.debug=true\n"
                           "fy.trace.bandcamp.resolve.debug=true"_s);
        auto page                    = trackPage(1);
        auto parentChild             = child(1, u"Inferred child title"_s);
        parentChild.title.provenance = FieldProvenance::Derived;
        const auto result            = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {parentChild}));
        QVERIFY(isSuccess(result));
        QCOMPARE(std::get<ResolutionSuccess<TrackAdmission>>(result).value.result.track.title, u"Page title"_s);

        bool sawTitleChoice = false;
        for(const auto& log : capturedLogs()) {
            QVERIFY(!log.message.contains(u"sig=child"_s));
            QVERIFY(!log.message.contains(u"sig=page"_s));
            if(log.category == u"fy.trace.bandcamp.resolve"_s && log.message.contains(u"assembly_field"_s)
               && log.message.contains(u"field=\"track.title\""_s)) {
                sawTitleChoice = true;
                QVERIFY(log.message.contains(u"bandcamp://track/1"_s));
                QVERIFY(log.message.contains(u"old=\"Inferred child title\""_s));
                QVERIFY(log.message.contains(u"new=\"Page title\""_s));
                QVERIFY(log.message.contains(u"provenance=\"explicit\""_s));
                QVERIFY(log.message.contains(u"source=\"selected_track_page\""_s));
            }
            if(log.category == u"fy.bandcamp.resolve"_s && log.message.contains(u"semantic_conflict"_s)
               && log.message.contains(u"field=\"track.title\""_s)) {
                QVERIFY(log.message.contains(u"album_child_value=\"Inferred child title\""_s));
                QVERIFY(log.message.contains(u"selected_page_value=\"Page title\""_s));
                QVERIFY(log.message.contains(u"decision=\"selected_track_page\""_s));
            }
            if(log.category == u"fy.trace.bandcamp.resolve"_s && log.message.contains(u"assembly_field"_s)) {
                if(log.message.contains(u"field=\"album.sourceUrl\""_s)) {
                    QVERIFY(log.message.contains(u"bandcamp://track/1"_s));
                    QVERIFY(log.message.contains(u"old=\"https://artist.bandcamp.com/album/release\""_s));
                    QVERIFY(log.message.contains(u"new=\"https://artist.bandcamp.com/album/release\""_s));
                }
                if(log.message.contains(u"field=\"album.trackTotal\""_s)) {
                    QVERIFY(log.message.contains(u"new=\"1\""_s));
                    QVERIFY(log.message.contains(u"source=\"declared album membership\""_s));
                }
            }
        }
        QVERIFY(sawTitleChoice);
    }

    void childStreamCannotBeBypassedByTrackPage()
    {
        auto parentChild      = child(1, u"One"_s);
        parentChild.streamUrl = CandidateField<QString>::missing();
        auto page             = trackPage(1);
        page.track.streamUrl  = explicitValue(PageStream);
        const auto result     = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {parentChild}));
        QVERIFY(!isSuccess(result));
        QCOMPARE(failure(result)->field, u"track.streamUrl"_s);
        QCOMPARE(failure(result)->trackId, qint64{1});

        parentChild.streamUrl = CandidateField<QString>::invalid();
        const auto invalid    = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {parentChild}));
        QVERIFY(!isSuccess(invalid));
        QCOMPARE(failure(invalid)->field, u"track.streamUrl"_s);
        QCOMPARE(failure(invalid)->code, ResolutionErrorCode::InvalidField);
    }

    void selectedChildMustMatchUniquelyAndParentIdentityMustAgree()
    {
        auto noMatch = admitAlbumTrack(trackPage(8), albumCandidate(40, u"Album"_s, {child(1, u"One"_s)}));
        QVERIFY(!isSuccess(noMatch));
        QCOMPARE(failure(noMatch)->field, u"parent.membership"_s);

        auto duplicate = admitAlbumTrack(
            trackPage(1), albumCandidate(40, u"Album"_s, {child(1, u"One"_s), child(1, u"Duplicate"_s)}));
        QVERIFY(!isSuccess(duplicate));
        QVERIFY(failure(duplicate)->reason.contains(u"not unique"_s));

        auto page           = trackPage(1);
        page.parent.albumId = explicitValue(qint64{41});
        auto mismatch       = admitAlbumTrack(page, albumCandidate(40, u"Album"_s, {child(1, u"One"_s)}));
        QVERIFY(!isSuccess(mismatch));
        QCOMPARE(failure(mismatch)->code, ResolutionErrorCode::RelationshipUnresolved);
        QCOMPARE(failure(mismatch)->field, u"parent.albumId"_s);
    }

    void parentEvidenceDoesNotGuessStandalone()
    {
        auto page               = trackPage();
        page.parent.declaration = ParentDeclaration::Unknown;
        QCOMPARE(std::get<RequiredAlbumParent>(classifyParentEvidence(page)).url, AlbumUrl);

        page.parent.albumUrl = CandidateField<QUrl>::missing();
        const auto missing   = classifyParentEvidence(page);
        QVERIFY(std::holds_alternative<ResolutionFailure>(missing));

        page.parent.declaration = ParentDeclaration::Invalid;
        const auto invalid      = classifyParentEvidence(page);
        QVERIFY(std::holds_alternative<ResolutionFailure>(invalid));

        page.parent.declaration = ParentDeclaration::AlbumBacked;
        page.parent.albumId     = explicitValue(qint64{40});
        const auto noLocation   = classifyParentEvidence(page);
        QVERIFY(std::holds_alternative<ResolutionFailure>(noLocation));
        QCOMPARE(std::get<ResolutionFailure>(noLocation).error.code, ResolutionErrorCode::ParentUnavailable);
    }

    void parentAdmissionErrorsRetainSelectedIdentity()
    {
        auto page           = trackPage(7);
        auto parent         = albumCandidate(41, u"Parent"_s, {child(7, u"Song"_s)});
        const auto mismatch = admitAlbumTrack(page, parent);
        QVERIFY(failure(mismatch));
        QCOMPARE(failure(mismatch)->trackId, qint64{7});

        parent.id               = explicitValue(qint64{40});
        parent.title            = CandidateField<QString>::missing();
        const auto missingTitle = admitAlbumTrack(page, parent);
        QVERIFY(failure(missingTitle));
        QCOMPARE(failure(missingTitle)->trackId, qint64{7});

        page.parent.albumUrl = explicitValue(QUrl(u"ftp://artist.bandcamp.com/album/parent"_s));
        const auto location  = classifyParentEvidence(page);
        QVERIFY(std::holds_alternative<ResolutionFailure>(location));
        QCOMPARE(std::get<ResolutionFailure>(location).error.trackId, qint64{7});

        page.parent.declaration = ParentDeclaration::NoParent;
        page.parent.albumId     = CandidateField<qint64>::missing();
        page.parent.albumUrl    = CandidateField<QUrl>::missing();
        page.sourceUrl          = explicitValue(AlbumUrl);
        const auto standalone   = admitStandaloneTrack(page);
        QVERIFY(failure(standalone));
        QCOMPARE(failure(standalone)->trackId, qint64{7});
    }

    void explicitNoParentWithUsableLocationIsContradictory()
    {
        auto page               = trackPage();
        page.parent.declaration = ParentDeclaration::NoParent;
        page.parent.albumId     = CandidateField<qint64>::invalid();
        const auto result       = classifyParentEvidence(page);
        QVERIFY(std::holds_alternative<ResolutionFailure>(result));
        QCOMPARE(std::get<ResolutionFailure>(result).error.code, ResolutionErrorCode::RelationshipUnresolved);

        page.parent.albumUrl = CandidateField<QUrl>::invalid();
        QVERIFY(std::holds_alternative<StandaloneParentEvidence>(classifyParentEvidence(page)));

        page.parent.albumId      = explicitValue(qint64{40});
        const auto conflictingId = classifyParentEvidence(page);
        QVERIFY(std::holds_alternative<ResolutionFailure>(conflictingId));
        QCOMPARE(std::get<ResolutionFailure>(conflictingId).error.field, u"parent.albumId"_s);
    }

    void standaloneTrackHasNoFabricatedAlbumOrNumber()
    {
        auto page               = trackPage();
        page.sourceUrl          = explicitValue(OtherTrackUrl);
        page.track.trackNumber  = CandidateField<qint64>::missing();
        page.parent.declaration = ParentDeclaration::NoParent;
        page.parent.albumId     = CandidateField<qint64>::invalid();
        page.parent.albumUrl    = CandidateField<QUrl>::missing();
        const auto result       = admitStandaloneTrack(page);
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        postProcessTrack(admitted);
        QVERIFY(!admitted.result.album.has_value());
        QCOMPARE(admitted.result.track.sourceUrl, OtherTrackUrl.toString());
        QCOMPARE(admitted.result.track.trackNumber, 0);
    }

    void standaloneRejectsInvalidSupportedStream()
    {
        auto page               = trackPage();
        page.parent.declaration = ParentDeclaration::NoParent;
        page.parent.albumId     = CandidateField<qint64>::invalid();
        page.parent.albumUrl    = CandidateField<QUrl>::missing();
        page.track.streamUrl    = explicitValue(QStringLiteral("http://cdn.example.com/track.mp3"));
        const auto result       = admitStandaloneTrack(page);
        QVERIFY(!isSuccess(result));
        QCOMPARE(failure(result)->field, u"track.streamUrl"_s);
        QCOMPARE(failure(result)->code, ResolutionErrorCode::InvalidField);
    }

    void albumAndTrackPassesRespectFullSiblingRequirements()
    {
        QList<BandcampTrack> siblings{trackValue(1, u"Artist - One"_s), trackValue(2, u"Artist - Two"_s)};
        BandcampAlbumMetadata album;
        album.id    = 4;
        album.title = u"Album"_s;
        AlbumMetadataProvenance albumProvenance;
        AlbumProcessorContext incomplete{siblings, false, {}, albumProvenance};
        AlbumArtistInferenceProcessor{}.process(album, incomplete);
        QVERIFY(album.artist.isEmpty());

        TrackMetadataProvenance trackProvenance;
        TrackProcessorContext trackContext{nullptr, siblings, false, 0, {}, {}, {}, trackProvenance};
        auto track = siblings.first();
        TrackArtistInferenceProcessor{}.process(track, trackContext);
        QVERIFY(track.artist.isEmpty());
        QCOMPARE(TrackNumberRepairProcessor{}.process(track, trackContext), 0);
    }

    void isolatedProcessorsApplyOneRule()
    {
        QList<BandcampTrack> explicitSiblings{trackValue(1, u"One"_s, u"Artist"_s),
                                              trackValue(2, u"Two"_s, u"artist"_s)};
        BandcampAlbumMetadata album;
        AlbumMetadataProvenance albumProvenance;
        AlbumProcessorContext albumContext{explicitSiblings, true, {}, albumProvenance};
        QCOMPARE(AlbumArtistInferenceProcessor{}.process(album, albumContext), 1);
        QCOMPARE(album.artist, u"Artist"_s);
        QCOMPARE(albumProvenance.artist, FieldProvenance::Derived);

        QList<BandcampTrack> siblings{trackValue(1, u"Artist - One"_s), trackValue(2, u"Artist - Two"_s)};
        TrackMetadataProvenance provenance;
        TrackProcessorContext context{nullptr, siblings, true, 0, {}, {}, {}, provenance};
        context.facts.inferredArtist = QStringLiteral("Artist");
        auto track                   = siblings.first();
        QCOMPARE(TrackArtistInferenceProcessor{}.process(track, context), 1);
        QCOMPARE(track.artist, u"Artist"_s);
        QCOMPARE(provenance.artist, FieldProvenance::Derived);
        QCOMPARE(TrackTitlePrefixProcessor{}.process(track, context), 1);
        QCOMPARE(track.title, u"One"_s);
    }

    void everyRuleMutatesOnlyItsOwnedField()
    {
        AlbumMetadataProvenance albumProvenance;
        QList<BandcampTrack> siblings;
        AlbumProcessorContext albumContext{siblings, false, {}, albumProvenance};
        BandcampAlbumMetadata album;
        album.title  = u"Artist - Release"_s;
        album.artist = u"Artist"_s;
        QCOMPARE(AlbumTitlePrefixProcessor{}.process(album, albumContext), 1);
        QCOMPARE(album.title, u"Release"_s);
        QCOMPARE(album.artist, u"Artist"_s);

        BandcampAlbumMetadata parent;
        parent.artist      = u"Album artist"_s;
        parent.releaseDate = QDate(2023, 4, 5);
        parent.artId       = 44;
        TrackMetadataProvenance provenance;
        TrackProcessorContext context{&parent, siblings, false, -1, {}, {}, {}, provenance};
        auto track = trackValue(4, u"Song"_s);

        QCOMPARE(TrackArtistFallbackProcessor{}.process(track, context), 1);
        QCOMPARE(track.artist, u"Album artist"_s);
        QVERIFY(!track.releaseDate.isValid());
        QCOMPARE(track.artId, qint64{0});
        QCOMPARE(provenance.artist, FieldProvenance::Inherited);

        QCOMPARE(TrackDateInheritanceProcessor{}.process(track, context), 1);
        QCOMPARE(track.releaseDate, QDate(2023, 4, 5));
        QCOMPARE(track.artId, qint64{0});

        QCOMPARE(TrackArtworkInheritanceProcessor{}.process(track, context), 1);
        QCOMPARE(track.artId, qint64{44});

        TrackMetadataProvenance fallbackProvenance;
        TrackProcessorContext fallbackContext{nullptr, siblings,          false, -1, {}, explicitValue(qint64{55}),
                                              {},      fallbackProvenance};
        auto standalone  = trackValue(5, u"Standalone"_s);
        standalone.artId = 0;
        QCOMPARE(TrackArtworkFallbackProcessor{}.process(standalone, fallbackContext), 1);
        QCOMPARE(standalone.artId, qint64{55});
        QCOMPARE(fallbackProvenance.artId, FieldProvenance::Fallback);
    }

    void sharedArtistInferenceAndCleanupComposeInOrder()
    {
        auto first         = child(1, u"Artist - One"_s);
        auto second        = child(2, u"Artist - Two"_s);
        first.artist       = CandidateField<QString>::missing();
        second.artist      = CandidateField<QString>::missing();
        second.trackNumber = explicitValue(qint64{2});
        auto source        = albumCandidate(40, u"Album"_s, {first, second});
        source.artist      = CandidateField<QString>::missing();
        auto result        = admitAlbum(source);
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<AlbumAdmission>>(result).value;
        postProcessAlbum(admitted);
        QVERIFY(admitted.album.metadata.artist.isEmpty());
        QCOMPARE(admitted.album.tracks.at(0).artist, u"Artist"_s);
        QCOMPARE(admitted.album.tracks.at(0).title, u"One"_s);
        QCOMPARE(admitted.album.tracks.at(1).title, u"Two"_s);
    }

    void trackArtistInferenceSupportsDifferentArtistsAcrossCompleteSet()
    {
        auto first    = child(1, u"Artist One - Song A"_s);
        auto second   = child(2, u"Artist Two - Song B"_s);
        first.artist  = CandidateField<QString>::missing();
        second.artist = CandidateField<QString>::missing();
        first.artist  = CandidateField<QString>::missing();
        second.artist = CandidateField<QString>::missing();
        auto result   = admitAlbum(albumCandidate(40, u"Various Artists"_s, {first, second}));
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<AlbumAdmission>>(result).value;
        postProcessAlbum(admitted);
        QCOMPARE(admitted.album.tracks.at(0).artist, u"Artist One"_s);
        QCOMPARE(admitted.album.tracks.at(0).title, u"Song A"_s);
        QCOMPARE(admitted.album.tracks.at(1).artist, u"Artist Two"_s);
        QCOMPARE(admitted.album.tracks.at(1).title, u"Song B"_s);
    }

    void trackArtistInferenceRequiresAllShapedAndNoExplicitArtists()
    {
        auto first            = child(1, u"Artist One - Song A"_s);
        auto second           = child(2, u"Artist Two - Song B"_s);
        first.artist          = CandidateField<QString>::missing();
        second.artist         = CandidateField<QString>::missing();
        second.title          = explicitValue(QStringLiteral("Unshaped title"));
        auto unshapedSource   = albumCandidate(40, u"Album"_s, {first, second});
        unshapedSource.artist = CandidateField<QString>::missing();
        auto unshaped         = admitAlbum(unshapedSource);
        QVERIFY(isSuccess(unshaped));
        auto unshapedAdmission = std::get<ResolutionSuccess<AlbumAdmission>>(unshaped).value;
        postProcessAlbum(unshapedAdmission);
        QVERIFY(unshapedAdmission.album.tracks.at(0).artist.isEmpty());

        second.title          = explicitValue(QStringLiteral("Artist Two - Song B"));
        second.artist         = explicitValue(QStringLiteral("Owned artist"));
        auto explicitSource   = albumCandidate(40, u"Album"_s, {first, second});
        explicitSource.artist = CandidateField<QString>::missing();
        auto explicitArtist   = admitAlbum(explicitSource);
        QVERIFY(isSuccess(explicitArtist));
        auto explicitAdmission = std::get<ResolutionSuccess<AlbumAdmission>>(explicitArtist).value;
        postProcessAlbum(explicitAdmission);
        QVERIFY(explicitAdmission.album.tracks.at(0).artist.isEmpty());
        QCOMPARE(explicitAdmission.album.tracks.at(1).artist, u"Owned artist"_s);
    }

    void maximumAlbumMembershipUsesPreparedSiblingFacts()
    {
        QList<TrackCandidate> children;
        children.reserve(MaxDeclaredTracks);
        for(qint64 id = 1; id <= MaxDeclaredTracks; ++id) {
            auto track        = child(id, QStringLiteral("Artist %1 - Song %1").arg(id));
            track.artist      = CandidateField<QString>::missing();
            track.trackNumber = CandidateField<qint64>::missing();
            children.append(std::move(track));
        }

        auto outcome = admitAlbum(albumCandidate(40, u"Large release"_s, std::move(children)));
        QVERIFY(isSuccess(outcome));
        auto admission = std::get<ResolutionSuccess<AlbumAdmission>>(outcome).value;
        QElapsedTimer timer;
        timer.start();
        postProcessAlbum(admission);
        QVERIFY2(timer.elapsed() < 10000, "10,000-member processing exceeded the generous regression bound");
        QCOMPARE(admission.album.tracks.size(), MaxDeclaredTracks);
        QCOMPARE(admission.album.tracks.first().artist, u"Artist 1"_s);
        QCOMPARE(admission.album.tracks.first().trackNumber, 1);
        QCOMPARE(admission.album.tracks.last().artist, u"Artist 10000"_s);
        QCOMPARE(admission.album.tracks.last().trackNumber, 10000);
    }

    void maximumNumberRepairHandlesRepeatedExplicitNumbers()
    {
        QList<TrackCandidate> children;
        children.reserve(MaxDeclaredTracks);
        for(qint64 id = 1; id <= MaxDeclaredTracks; ++id) {
            auto track        = child(id, u"Song"_s);
            track.trackNumber = id % 2 == 1 ? explicitValue(qint64{1}) : CandidateField<qint64>::missing();
            children.append(std::move(track));
        }
        auto outcome = admitAlbum(albumCandidate(40, u"Large release"_s, std::move(children)));
        QVERIFY(isSuccess(outcome));
        auto admission = std::get<ResolutionSuccess<AlbumAdmission>>(outcome).value;
        QElapsedTimer timer;
        timer.start();
        postProcessAlbum(admission);
        QVERIFY2(timer.elapsed() < 10000, "Repeated explicit numbers caused excessive repair work");
        for(qsizetype i = 0; i < admission.album.tracks.size(); ++i) {
            QCOMPARE(admission.album.tracks.at(i).trackNumber, i % 2 == 0 ? 1 : static_cast<int>(i / 2 + 2));
        }
    }

    void trackDateAndArtStayOwnedWhenParentDiffers()
    {
        auto page              = trackPage();
        page.track.releaseDate = explicitValue(QDate(2024, 2, 3));
        page.track.artId       = explicitValue(qint64{990});
        auto album             = albumCandidate(40, u"Parent"_s, {child(1, u"One"_s)});
        album.releaseDate      = explicitValue(QDate(2019, 1, 1));
        album.artId            = explicitValue(qint64{80});
        auto result            = admitAlbumTrack(page, album);
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        postProcessTrack(admitted);
        QCOMPARE(admitted.result.track.releaseDate, QDate(2024, 2, 3));
        QCOMPARE(admitted.result.track.artId, qint64{990});
    }

    void missingTrackDateAndArtInheritFromParent()
    {
        auto page              = trackPage();
        page.track.releaseDate = CandidateField<QDate>::missing();
        page.track.artId       = CandidateField<qint64>::missing();
        auto result            = admitAlbumTrack(page, albumCandidate(40, u"Parent"_s, {child(1, u"One"_s)}));
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<TrackAdmission>>(result).value;
        postProcessTrack(admitted);
        QCOMPARE(admitted.result.track.releaseDate, QDate(2020, 1, 1));
        QCOMPARE(admitted.result.track.artId, qint64{80});
        QCOMPARE(admitted.trackProvenance.releaseDate, FieldProvenance::Inherited);
        QCOMPARE(admitted.trackProvenance.artId, FieldProvenance::Inherited);
    }

    void trackNumberRepairReservesEveryExplicitNumber()
    {
        QList<BandcampTrack> siblings{trackValue(1, u"One"_s, {}, 1), trackValue(2, u"Missing"_s),
                                      trackValue(3, u"Two"_s, {}, 2)};
        TrackMetadataProvenance provenance;
        TrackProcessorContext context{nullptr, siblings, true, 1, {}, {}, {}, provenance};
        context.facts.repairedTrackNumber = 3;
        auto missing                      = siblings.at(1);
        QCOMPARE(TrackNumberRepairProcessor{}.process(missing, context), 1);
        QCOMPARE(missing.trackNumber, 3);
        QCOMPARE(provenance.trackNumber, FieldProvenance::Derived);
        QCOMPARE(siblings.at(2).trackNumber, 2);
    }

    void trackNumberRepairFillsUnreservedGap()
    {
        QList<BandcampTrack> siblings{trackValue(1, u"One"_s, {}, 1), trackValue(2, u"Two"_s, {}, 2),
                                      trackValue(3, u"Missing"_s), trackValue(4, u"Four"_s, {}, 4)};
        TrackMetadataProvenance provenance;
        TrackProcessorContext context{nullptr, siblings, true, 2, {}, {}, {}, provenance};
        context.facts.repairedTrackNumber = 3;
        auto missing                      = siblings.at(2);
        QCOMPARE(TrackNumberRepairProcessor{}.process(missing, context), 1);
        QCOMPARE(missing.trackNumber, 3);
    }

    void trackNumberRepairPreservesRepeatedExplicitNumbers()
    {
        QList<BandcampTrack> siblings{trackValue(1, u"One"_s, {}, 2), trackValue(2, u"Two"_s, {}, 2),
                                      trackValue(3, u"Missing"_s)};
        TrackMetadataProvenance provenance;
        TrackProcessorContext context{nullptr, siblings, true, 1, {}, {}, {}, provenance};
        auto repeated = siblings.at(1);
        QCOMPARE(TrackNumberRepairProcessor{}.process(repeated, context), 0);
        QCOMPARE(repeated.trackNumber, 2);

        context.siblingIndex              = 2;
        context.facts.repairedTrackNumber = 3;
        auto missing                      = siblings.at(2);
        QCOMPARE(TrackNumberRepairProcessor{}.process(missing, context), 1);
        QCOMPARE(missing.trackNumber, 3);
    }

    void composedNumberRepairReservesAllExplicitNumbers()
    {
        auto first          = child(1, u"One"_s);
        first.trackNumber   = explicitValue(qint64{1});
        auto missing        = child(2, u"Missing"_s);
        missing.trackNumber = CandidateField<qint64>::missing();
        auto later          = child(3, u"Later explicit"_s);
        later.trackNumber   = explicitValue(qint64{2});
        auto result         = admitAlbum(albumCandidate(40, u"Album"_s, {first, missing, later}));
        QVERIFY(isSuccess(result));
        auto admitted = std::get<ResolutionSuccess<AlbumAdmission>>(result).value;
        postProcessAlbum(admitted);
        QCOMPARE(admitted.album.tracks.at(0).trackNumber, 1);
        QCOMPARE(admitted.album.tracks.at(1).trackNumber, 3);
        QCOMPARE(admitted.album.tracks.at(2).trackNumber, 2);
    }

    void processorsEmitOldNewProvenanceAndNoNoopEvents()
    {
        LogCapture capture(u"fy.trace.bandcamp.tracks.debug=true"_s);
        TrackAdmission admission;
        admission.result.track = trackValue(77, u"Artist - Song"_s, u"Artist"_s);
        admission.correlation  = {.jobId = 5, .resourceUrl = AlbumUrl, .trackId = 77};
        postProcessTrack(admission);
        QCOMPARE(admission.result.track.title, u"Song"_s);
        bool sawChange = false;
        for(const auto& log : capturedLogs()) {
            if(log.category == u"fy.trace.bandcamp.tracks"_s && log.message.contains(u"processor_change"_s)) {
                sawChange = true;
                QVERIFY(log.message.contains(u"bandcamp://track/77"_s));
                QVERIFY(log.message.contains(u"old=\"Artist - Song\""_s));
                QVERIFY(log.message.contains(u"new=\"Song\""_s));
                QVERIFY(log.message.contains(u"provenance=\"derived\""_s));
                QVERIFY(log.message.contains(u"reason="_s));
            }
        }
        QVERIFY(sawChange);

        capturedLogs().clear();
        TrackAdmission noop;
        noop.result.track = trackValue(78, u"Song"_s, u"Artist"_s);
        noop.correlation  = {.jobId = 5, .resourceUrl = TrackUrl, .trackId = 78};
        postProcessTrack(noop);
        for(const auto& log : capturedLogs()) {
            QVERIFY(!log.message.contains(u"processor_change"_s));
        }
    }

    void parserCandidatesFeedAdmissionWithoutLegacyProjection()
    {
        const auto parsed = parseReleasePage(albumPage(R"({"type":"album","id":40,"title":"Album","artist":"Artist"})",
                                                       {streamEntry(R"("id":1,"title":"Song","track_num":1)")}),
                                             AlbumUrl);
        const auto* success = std::get_if<ResolutionSuccess<ReleaseCandidate>>(&parsed);
        QVERIFY(success);
        const auto* album = std::get_if<AlbumCandidate>(&success->value);
        QVERIFY(album);
        const auto admitted = admitAlbum(*album);
        QVERIFY(isSuccess(admitted));
        const auto& result = std::get<ResolutionSuccess<AlbumAdmission>>(admitted).value;
        QCOMPARE(result.album.tracks.first().streamUrl, QStringLiteral("https://cdn.example.com/track.mp3"));
    }

    void parserNullOptionalValuesRemainUnknownAfterResolution()
    {
        verifyParserNullOptionals();
    }
};

int runPostProcessorTests(int argc, char** argv)
{
    AdmissionAndProcessorTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcamppostprocessortests.moc"
