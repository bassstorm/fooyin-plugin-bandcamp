#include "bandcampparser.h"

#include "bandcamplogcapture.h"
#include "bandcamptestpages.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <limits>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;
using namespace BandcampTestPages;

namespace {
const QUrl PageUrl{u"https://artist.bandcamp.com/track/viewed?from=test#part"_s};

const ReleaseCandidate* candidate(const ReleaseParseOutcome& result)
{
    const auto* success = std::get_if<ResolutionSuccess<ReleaseCandidate>>(&result);
    return success ? &success->value : nullptr;
}

template <typename T>
const T* value(const ReleaseParseOutcome& result)
{
    const auto* release = candidate(result);
    return release ? std::get_if<T>(release) : nullptr;
}

ReleaseParseOutcome parse(const QByteArray& json)
{
    return parseReleasePage(tralbumPage(json), PageUrl);
}

bool hasEvent(const QString& category, const QString& event, const QString& detail = {})
{
    for(const auto& log : capturedLogs()) {
        if(log.category == category && log.message.contains(event) && log.message.contains(detail)) {
            return true;
        }
    }
    return false;
}

QByteArray musicListing(const QByteArray& listing)
{
    QByteArray body = tralbumPage(R"({"url":"https://artist.bandcamp.com/music"})");
    body.replace("</html>", listing + "</html>");
    return body;
}

class ParserTests : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void kindAndIdentityAliases();
    void decodingIsSinglePassAndChecked();
    void identityDisagreementAndShape_data();
    void identityDisagreementAndShape();
    void exactIdentityAndNumericValidation_data();
    void exactIdentityAndNumericValidation();
    void albumOwnershipAndProvenance();
    void viewedTrackUsesStableIdAndOwnFields();
    void rowFallbackIsExplicitAndRootIsOnlyContext();
    void absentOrAmbiguousMatchingRows();
    void parentEvidence_data();
    void parentEvidence();
    void preservesAllDeclaredMembers();
    void optionalNumericAndStreamValidation();
    void retainsSupportedDurationAndDateFormats();
    void limitsAreExplicitFailures();
    void diagnosticsAreCorrelatedRedactedAndIndependent();
    void extractsOrderedStructuredListingReferences();
    void htmlRecoveryPreservesStructuredSelection();
    void conflictingHtmlAttributesFailDespiteRecovery();
    void extractsClientItemsAsOrderedReferences();
    void ignoresLargeOpaquePageData();
    void clientItemsHaveIndependentCollectionBounds();
    void htmlDepthLimitFailsExplicitly();
    void malformedListingTailFailsClosed();
    void scriptTextIsNotContinuationMarkup();
    void extractsOpaqueContinuationLocator();
    void malformedListingEntriesFailExplicitly_data();
    void malformedListingEntriesFailExplicitly();
};

void ParserTests::init()
{
    QLoggingCategory::setFilterRules(u"fy.bandcamp.*=false\nfy.trace.bandcamp.*=false"_s);
}

void ParserTests::cleanup()
{
    QLoggingCategory::setFilterRules({});
}

void ParserTests::kindAndIdentityAliases()
{
    const auto result = parse(R"({"item_type":"a","id":"9223372036854775807","url":"not an identity","trackinfo":[]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->id.value, std::numeric_limits<qint64>::max());
    QCOMPARE(album->id.provenance, FieldProvenance::Explicit);
    QCOMPARE(album->sourceUrl.value.toString(), u"https://artist.bandcamp.com/track/viewed"_s);
    QCOMPARE(album->title.state, FieldState::Missing);

    const auto equal = parse(
        R"({"current":{"type":"album","id":9007199254740993},"item_type":"a","id":"9007199254740993","trackinfo":[]})");
    QVERIFY(value<AlbumCandidate>(equal));
    QCOMPARE(value<AlbumCandidate>(equal)->id.value, qint64{9007199254740993});

    const auto fallback = parse(R"({"current":{"type":8,"id":false},"item_type":"t","id":9,"trackinfo":[]})");
    QVERIFY(value<TrackPageCandidate>(fallback));
    QCOMPARE(value<TrackPageCandidate>(fallback)->track.id.value, qint64{9});
}

void ParserTests::decodingIsSinglePassAndChecked()
{
    const auto result = parse(R"({"current":{"type":"album","title":"&#65; &amp;quot; \uD83C\uDFB5"},"trackinfo":[]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->title.value, u"A &quot; 🎵"_s);
    const auto invalid = parseReleasePage("<script data-tralbum='\xff'>", PageUrl);
    QVERIFY(std::holds_alternative<ResolutionFailure>(invalid));
    QCOMPARE(std::get<ResolutionFailure>(invalid).error.code, ResolutionErrorCode::MalformedPage);
}

void ParserTests::identityDisagreementAndShape_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("kind-conflict") << QByteArray(R"({"current":{"type":"album"},"item_type":"t","trackinfo":[]})");
    QTest::newRow("id-conflict") << QByteArray(R"({"current":{"type":"album","id":1},"id":2,"trackinfo":[]})");
    QTest::newRow("unknown-kind") << QByteArray(R"({"current":{"type":"listing"},"trackinfo":[]})");
    QTest::newRow("url-only") << QByteArray("{\"url\":\"https://artist.bandcamp.com/album/x\"}");
    QTest::newRow("current-shape") << QByteArray(R"({"current":[],"item_type":"a","trackinfo":[]})");
    QTest::newRow("records-shape") << QByteArray(R"({"current":{"type":"album"},"trackinfo":{}})");
    QTest::newRow("root-array") << QByteArray("[]");
    QTest::newRow("root-scalar") << QByteArray("null");
}

void ParserTests::identityDisagreementAndShape()
{
    QFETCH(QByteArray, json);
    const auto result  = parse(json);
    const auto* failed = std::get_if<ResolutionFailure>(&result);
    QVERIFY(failed);
    QCOMPARE(failed->error.code, ResolutionErrorCode::MalformedPage);
    QCOMPARE(failed->error.stage, ResolutionStage::Parse);
    QCOMPARE(failed->error.resourceUrl, PageUrl);
}

void ParserTests::exactIdentityAndNumericValidation_data()
{
    QTest::addColumn<QByteArray>("number");
    QTest::addColumn<bool>("usable");
    QTest::addColumn<qint64>("expected");
    QTest::newRow("above-double-precision") << QByteArray("9007199254740993") << true << qint64{9007199254740993};
    QTest::newRow("max-integer") << QByteArray("9223372036854775807") << true << std::numeric_limits<qint64>::max();
    QTest::newRow("max-string") << QByteArray("\"9223372036854775807\"") << true << std::numeric_limits<qint64>::max();
    QTest::newRow("exact-large-decimal") << QByteArray("9007199254740993.0") << true << qint64{9007199254740993};
    QTest::newRow("exact-large-exponent") << QByteArray("90071992547409930e-1") << true << qint64{9007199254740993};
    QTest::newRow("overflow-integer") << QByteArray("9223372036854775808") << false << qint64{0};
    QTest::newRow("overflow-string") << QByteArray("\"9223372036854775808\"") << false << qint64{0};
    QTest::newRow("fraction") << QByteArray("1.5") << false << qint64{0};
    QTest::newRow("large-fraction") << QByteArray("9007199254740993.1") << false << qint64{0};
    QTest::newRow("tiny-fraction") << QByteArray("1.00000000000000001") << false << qint64{0};
    QTest::newRow("fraction-text") << QByteArray("\"1.5\"") << false << qint64{0};
    QTest::newRow("nonfinite-text") << QByteArray("\"nan\"") << false << qint64{0};
    QTest::newRow("zero") << QByteArray("0") << false << qint64{0};
    QTest::newRow("negative") << QByteArray("-1") << false << qint64{0};
    QTest::newRow("boolean") << QByteArray("true") << false << qint64{0};
    QTest::newRow("null") << QByteArray("null") << false << qint64{0};
}

void ParserTests::exactIdentityAndNumericValidation()
{
    QFETCH(QByteArray, number);
    QFETCH(bool, usable);
    QFETCH(qint64, expected);
    const auto result = parse(R"({"current":{"type":"album","id":)" + number + R"(},"trackinfo":[]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->id.isUsable(), usable);
    QCOMPARE(album->id.value, expected);
    if(!usable) {
        QCOMPARE(album->id.state, FieldState::Invalid);
    }
}

void ParserTests::albumOwnershipAndProvenance()
{
    const auto result = parse(R"({
            "current":{"type":"album","id":1,"title":" Own title ","artist":"Own artist","release_date":"bad","art_id":3},
            "artist":"Root artist","title":"not an alias","album_release_date":"2024-05-06","art_id":4,
            "trackinfo":[{"track_id":7,"title":"Child"}]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->title.value, u"Own title"_s);
    QCOMPARE(album->artist.value, u"Own artist"_s);
    QCOMPARE(album->artist.provenance, FieldProvenance::Explicit);
    QCOMPARE(album->releaseDate.value, QDate(2024, 5, 6));
    QCOMPARE(album->releaseDate.provenance, FieldProvenance::Fallback);
    QCOMPARE(album->artId.value, qint64{3});
    const auto& child = album->tracks.first();
    QCOMPARE(child.id.value, qint64{7});
    QCOMPARE(child.id.provenance, FieldProvenance::Explicit);
    QCOMPARE(child.artist.state, FieldState::Missing);
    QCOMPARE(child.releaseDate.state, FieldState::Missing);
    QCOMPARE(child.artId.state, FieldState::Missing);

    const auto fallback = parse(
        R"({"current":{"type":"album","artist":null},"artist":"Root","title":"Not album title","trackinfo":[]})");
    QVERIFY(value<AlbumCandidate>(fallback));
    QCOMPARE(value<AlbumCandidate>(fallback)->artist.value, u"Root"_s);
    QCOMPARE(value<AlbumCandidate>(fallback)->artist.provenance, FieldProvenance::Fallback);
    QCOMPARE(value<AlbumCandidate>(fallback)->title.state, FieldState::Missing);
}

void ParserTests::viewedTrackUsesStableIdAndOwnFields()
{
    const auto result = parse(R"({
            "current":{"type":"track","id":22,"title":"Current","artist":"Track artist","track_number":7,
                       "release_date":"2020-01-02","art_id":8,"album_id":11},
            "id":"22","item_type":"t","artist":"Album artist","art_id":99,"album_release_date":"2025-01-01",
            "album_url":"../album/parent?from=track",
            "trackinfo":[{"id":1,"title":"Wrong","duration":1},
                         {"track_id":"22","title":"Row","artist":"Row artist","track_num":2,"duration":"183.429",
                          "file":{"mp3-128":"https:\/\/cdn.example.com/a.mp3?token=abc%2f%41def&expires=123"}}]})");
    const auto* page  = value<TrackPageCandidate>(result);
    QVERIFY(page);
    QCOMPARE(page->matchingRecord.value, qsizetype{1});
    QCOMPARE(page->track.declaredIndex, qsizetype{1});
    QCOMPARE(page->track.id.value, qint64{22});
    QCOMPARE(page->track.title.value, u"Current"_s);
    QCOMPARE(page->track.artist.value, u"Track artist"_s);
    QCOMPARE(page->track.trackNumber.value, qint64{7});
    QCOMPARE(page->track.durationMs.value, qint64{183429});
    QCOMPARE(page->track.streamUrl.value, u"https://cdn.example.com/a.mp3?token=abc%2f%41def&expires=123"_s);
    QCOMPARE(page->track.releaseDate.value, QDate(2020, 1, 2));
    QCOMPARE(page->track.artId.value, qint64{8});
    QCOMPARE(page->fallbackArtist.value, u"Album artist"_s);
    QCOMPARE(page->fallbackArtist.provenance, FieldProvenance::Fallback);
    QCOMPARE(page->fallbackArtId.value, qint64{99});
    QCOMPARE(page->fallbackArtId.provenance, FieldProvenance::Fallback);
    QCOMPARE(page->parent.declaration, ParentDeclaration::AlbumBacked);
    QCOMPARE(page->parent.albumUrl.value.toString(), u"https://artist.bandcamp.com/album/parent?from=track"_s);
}

void ParserTests::rowFallbackIsExplicitAndRootIsOnlyContext()
{
    const auto result = parse(
        R"({"current":{"type":"track","id":3,"title":false,"artist":"","track_number":0},
            "artist":"Context","art_id":9,"album_release_date":"2025-01-01",
            "trackinfo":[{"id":3,"title":"Row","artist":"Recording","track_num":4}]})");
    const auto* page = value<TrackPageCandidate>(result);
    QVERIFY(page);
    QCOMPARE(page->track.title.value, u"Row"_s);
    QCOMPARE(page->track.artist.value, u"Recording"_s);
    QCOMPARE(page->track.artist.provenance, FieldProvenance::Explicit);
    QCOMPARE(page->track.trackNumber.value, qint64{4});
    QCOMPARE(page->track.releaseDate.state, FieldState::Missing);
    QCOMPARE(page->track.artId.state, FieldState::Missing);
    QCOMPARE(page->parent.declaration, ParentDeclaration::Unknown);
}

void ParserTests::absentOrAmbiguousMatchingRows()
{
    const auto missing
        = parse(R"({"current":{"type":"track","id":3,"title":"Viewed"},"trackinfo":[{"id":4,"title":"Not viewed"}]})");
    const auto* absent = value<TrackPageCandidate>(missing);
    QVERIFY(absent);
    QCOMPARE(absent->matchingRecord.state, FieldState::Missing);
    QCOMPARE(absent->track.title.value, u"Viewed"_s);
    QCOMPARE(absent->track.streamUrl.state, FieldState::Missing);

    const auto duplicate
        = parse(R"({"current":{"type":"track","id":3},"trackinfo":[{"id":3},{"track_id":3},{"id":3}]})");
    QVERIFY(value<TrackPageCandidate>(duplicate));
    QCOMPARE(value<TrackPageCandidate>(duplicate)->matchingRecord.state, FieldState::Invalid);
    QCOMPARE(value<TrackPageCandidate>(duplicate)->track.recordState, FieldState::Missing);

    const auto conflict = parse(R"({"current":{"type":"track","id":3},"trackinfo":[{"id":3,"track_id":4}]})");
    QVERIFY(value<TrackPageCandidate>(conflict));
    QCOMPARE(value<TrackPageCandidate>(conflict)->matchingRecord.state, FieldState::Missing);
}

void ParserTests::parentEvidence_data()
{
    QTest::addColumn<QByteArray>("fields");
    QTest::addColumn<QByteArray>("root");
    QTest::addColumn<ParentDeclaration>("declaration");
    QTest::addColumn<FieldState>("locationState");
    QTest::newRow("missing") << QByteArray() << QByteArray() << ParentDeclaration::Unknown << FieldState::Missing;
    QTest::newRow("missing-id-null-url") << QByteArray() << QByteArray(",\"album_url\":null")
                                         << ParentDeclaration::Unknown << FieldState::Invalid;
    QTest::newRow("explicit-null") << QByteArray(",\"album_id\":null") << QByteArray() << ParentDeclaration::NoParent
                                   << FieldState::Missing;
    QTest::newRow("explicit-zero") << QByteArray(",\"album_id\":0") << QByteArray() << ParentDeclaration::NoParent
                                   << FieldState::Missing;
    QTest::newRow("known-id-no-location")
        << QByteArray(",\"album_id\":12") << QByteArray() << ParentDeclaration::AlbumBacked << FieldState::Missing;
    QTest::newRow("known-id-bad-location")
        << QByteArray(",\"album_id\":\"12\"") << QByteArray(",\"album_url\":\"ftp://a/x\"")
        << ParentDeclaration::AlbumBacked << FieldState::Invalid;
    QTest::newRow("bad-id") << QByteArray(",\"album_id\":false") << QByteArray() << ParentDeclaration::Invalid
                            << FieldState::Missing;
    QTest::newRow("fractional-id") << QByteArray(",\"album_id\":1.00000000000000001") << QByteArray()
                                   << ParentDeclaration::Invalid << FieldState::Missing;
    QTest::newRow("no-parent-conflicting-location")
        << QByteArray(",\"album_id\":0") << QByteArray(",\"album_url\":\"/album/parent\"")
        << ParentDeclaration::NoParent << FieldState::Usable;
    QTest::newRow("only-location") << QByteArray() << QByteArray(",\"album_url\":\"/album/parent\"")
                                   << ParentDeclaration::Unknown << FieldState::Usable;
}

void ParserTests::parentEvidence()
{
    QFETCH(QByteArray, fields);
    QFETCH(QByteArray, root);
    QFETCH(ParentDeclaration, declaration);
    QFETCH(FieldState, locationState);
    const auto result = parse(R"({"current":{"type":"track","id":3)" + fields + "}" + root + R"(,"trackinfo":[]})");
    const auto* page  = value<TrackPageCandidate>(result);
    QVERIFY(page);
    QCOMPARE(page->parent.declaration, declaration);
    QCOMPARE(page->parent.albumUrl.state, locationState);
    if(declaration == ParentDeclaration::AlbumBacked) {
        QCOMPARE(page->parent.albumId.value, qint64{12});
    }
}

void ParserTests::preservesAllDeclaredMembers()
{
    const auto result
        = parse(R"({"current":{"type":"album","id":1},"trackinfo":[null,7,{},{"id":4},{"id":5,"track_id":6}]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->tracks.size(), qsizetype{5});
    for(qsizetype i = 0; i < album->tracks.size(); ++i) {
        QCOMPARE(album->tracks.at(i).declaredIndex, i);
    }
    QCOMPARE(album->tracks.at(0).recordState, FieldState::Invalid);
    QCOMPARE(album->tracks.at(1).recordState, FieldState::Invalid);
    QCOMPARE(album->tracks.at(2).id.state, FieldState::Missing);
    QCOMPARE(album->tracks.at(3).id.value, qint64{4});
    QCOMPARE(album->tracks.at(3).streamUrl.state, FieldState::Missing);
    QCOMPARE(album->tracks.at(4).id.state, FieldState::Invalid);
}

void ParserTests::optionalNumericAndStreamValidation()
{
    const auto result = parse(R"({"current":{"type":"album"},"trackinfo":[
            {"id":1,"duration":0,"track_num":100000,"file":{"mp3-128":"https:\/\/cdn.example/a?x=y"}},
            {"id":2,"duration":604800,"track_num":100001,"file":{"mp3-128":"http:\/\/cdn.example/a"}},
            {"id":3,"duration":604800.001,"track_num":1.5,"file":{"mp3-128":"https:\/\/user:pass@cdn.example/a"}},
            {"id":4,"duration":-1,"file":{"mp3-128":"/relative"}},
            {"id":5,"duration":"inf","file":false},
            {"id":6,"duration":false,"file":{"mp3-128":"https:\/\/[invalid"}},
            {"id":7,"duration":"1:60","file":{"mp3-128":true}}]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->tracks.at(0).durationMs.state, FieldState::Usable);
    QCOMPARE(album->tracks.at(0).durationMs.value, qint64{0});
    QCOMPARE(album->tracks.at(0).trackNumber.value, MaxTrackNumber);
    QVERIFY(album->tracks.at(0).streamUrl.isUsable());
    QCOMPARE(album->tracks.at(1).durationMs.value, MaxDurationMs);
    for(qsizetype i = 1; i < album->tracks.size(); ++i) {
        QCOMPARE(album->tracks.at(i).streamUrl.state, FieldState::Invalid);
    }
    for(qsizetype i = 2; i < album->tracks.size(); ++i) {
        QCOMPARE(album->tracks.at(i).durationMs.state, FieldState::Invalid);
    }
    QCOMPARE(album->tracks.at(1).trackNumber.state, FieldState::Invalid);
    QCOMPARE(album->tracks.at(2).trackNumber.state, FieldState::Invalid);
    QVERIFY(!isSuccess(parse(R"({"current":{"type":"album","id":1e999},"trackinfo":[]})")));
}

void ParserTests::retainsSupportedDurationAndDateFormats()
{
    const auto result = parse(R"({"current":{"type":"album","release_date":"2024-05-06T12:34:56Z"},
        "trackinfo":[{"id":1,"duration":"3:03.429"},{"id":2,"duration":"1:02:03.5"},
                     {"id":3,"duration":"1:60"},{"id":4,"duration":"1.5:00"}]})");
    const auto* album = value<AlbumCandidate>(result);
    QVERIFY(album);
    QCOMPARE(album->releaseDate.value, QDate(2024, 5, 6));
    QCOMPARE(album->tracks.at(0).durationMs.value, qint64{183429});
    QCOMPARE(album->tracks.at(1).durationMs.value, qint64{3723500});
    QCOMPARE(album->tracks.at(2).durationMs.state, FieldState::Invalid);
    QCOMPARE(album->tracks.at(3).durationMs.state, FieldState::Invalid);
}

void ParserTests::limitsAreExplicitFailures()
{
    const auto checkLimit = [](const ReleaseParseOutcome& result) {
        const auto* failed = std::get_if<ResolutionFailure>(&result);
        return failed && failed->error.code == ResolutionErrorCode::ResourceLimitExceeded
            && failed->error.stage == ResolutionStage::Parse;
    };
    QByteArray body = tralbumPage(R"({"current":{"type":"album"},"trackinfo":[]})");
    body.append(QByteArray(MaxPageBytes - body.size(), ' '));
    QVERIFY(isSuccess(parseReleasePage(body, PageUrl)));
    body.append(' ');
    const auto tooLarge = parseReleasePage(body, PageUrl);
    QVERIFY(std::holds_alternative<ResolutionFailure>(tooLarge));
    QCOMPARE(std::get<ResolutionFailure>(tooLarge).error.code, ResolutionErrorCode::PageTooLarge);

    const auto field = [](qsizetype chars) {
        return parse(R"({"current":{"type":"album","title":")" + QByteArray(chars, 'x') + R"("},"trackinfo":[]})");
    };
    const auto exact = field(MaxSourceFieldChars);
    QVERIFY(value<AlbumCandidate>(exact));
    QCOMPARE(value<AlbumCandidate>(exact)->title.value.size(), MaxSourceFieldChars);
    QVERIFY(checkLimit(field(MaxSourceFieldChars + 1)));
    QVERIFY(checkLimit(parse(R"({"current":{"type":"album"},"trackinfo":[],")"
                             + QByteArray(MaxSourceFieldChars + 1, 'k') + R"(":0})")));

    const auto depth = [](int count) {
        return parse(R"({"current":{"type":"album"},"trackinfo":[],"extra":)" + QByteArray(count - 1, '[') + "0"
                     + QByteArray(count - 1, ']') + "}");
    };
    QVERIFY(isSuccess(depth(MaxJsonDepth)));
    QVERIFY(checkLimit(depth(MaxJsonDepth + 1)));
    const auto rows = [](qsizetype count) {
        QByteArray array;
        for(qsizetype i = 0; i < count; ++i) {
            if(i) {
                array += ',';
            }
            array += "null";
        }
        return array;
    };
    const auto members = parse(R"({"current":{"type":"album"},"trackinfo":[)" + rows(MaxDeclaredTracks) + "]}");
    QVERIFY(value<AlbumCandidate>(members));
    QCOMPARE(value<AlbumCandidate>(members)->tracks.size(), MaxDeclaredTracks);
    QVERIFY(checkLimit(parse(R"({"current":{"type":"album"},"trackinfo":[)" + rows(MaxDeclaredTracks + 1) + "]}")));
    // root/current/type/trackinfo/extra account for five values.
    QVERIFY(isSuccess(
        parse(R"({"current":{"type":"album"},"trackinfo":[],"extra":[)" + rows(MaxSourceRecords - 5) + "]}")));
    QVERIFY(checkLimit(
        parse(R"({"current":{"type":"album"},"trackinfo":[],"extra":[)" + rows(MaxSourceRecords - 4) + "]}")));
}

void ParserTests::diagnosticsAreCorrelatedRedactedAndIndependent()
{
    const QByteArray json =
        R"({"current":{"type":"album","id":1,"title":"T","artist":"Own","release_date":false},
            "artist":"Other","album_release_date":"2024-01-02","extra":{"long":")"
        + QByteArray(1200, 'x') +
        R"(","url":"https:\/\/user:password@cdn.example/path?token=secret#fragment",
             "bad_url":"not-a-url?token=secret","authorization":{"headers":["Bearer secret"]},
             "multiline":"one\ninjected\u2028line"},
            "trackinfo":[{"id":9,"title":"Child","file":{"mp3-128":"https:\/\/cdn.example/audio?sig=secret",
                "mp3-v0":"invalid?token=secret"}}]})";
    const auto body = tralbumPage(json);
    LogCapture capture(u"fy.bandcamp.parser.debug=true\nfy.trace.bandcamp.parser.debug=true"_s);
    const auto result = parseReleasePage(body, PageUrl, {.jobId = 4, .requestId = 8});
    QVERIFY(value<AlbumCandidate>(result));
    QVERIFY(!capturedLogs().isEmpty());
    QVERIFY(capturedLogs().first().message.contains(u"source_snapshot"_s));
    QVERIFY(hasEvent(u"fy.bandcamp.parser"_s, u"source_conflict"_s, u"$.current.artist"_s));
    QVERIFY(hasEvent(u"fy.bandcamp.parser"_s, u"source_conflict"_s, u"$.artist"_s));
    QVERIFY(hasEvent(u"fy.bandcamp.parser"_s, u"source_unresolved"_s, u"$.trackinfo[0].artist"_s));
    QVERIFY(hasEvent(u"fy.trace.bandcamp.parser"_s, u"source_mapping"_s, u"old=\"<invalid>\""_s));
    int snapshots = 0;
    for(const auto& log : capturedLogs()) {
        QVERIFY(!log.message.contains(u"secret"_s));
        QVERIFY(!log.message.contains(u"password"_s));
        QVERIFY(!log.message.contains(u"?from="_s));
        QVERIFY(log.message.contains(u"job=\"4\""_s));
        QVERIFY(log.message.contains(u"request=\"8\""_s));
        QVERIFY(!log.message.contains(u'\n'));
        QVERIFY(!log.message.contains(QChar(0x2028)));
        if(log.message.contains(u"source_snapshot"_s)) {
            ++snapshots;
            const auto offset = log.message.indexOf(u" json="_s);
            QVERIFY(offset > 0);
            const auto snapshot = QJsonDocument::fromJson(log.message.mid(offset + 6).toUtf8());
            QVERIFY(snapshot.isObject());
            QCOMPARE(snapshot.object().value(u"extra").toObject().value(u"long").toString().size(), 1200);
            QCOMPARE(snapshot.object().value(u"extra").toObject().value(u"url").toString(),
                     u"https://cdn.example/path"_s);
            QCOMPARE(snapshot.object().value(u"trackinfo").toArray().size(), qsizetype{1});
            const auto extra = snapshot.object().value(u"extra").toObject();
            QCOMPARE(extra.value(u"bad_url").toString(), u"not-a-url"_s);
            QCOMPARE(extra.value(u"authorization").toObject().value(u"headers").toArray().size(), qsizetype{1});
            QCOMPARE(extra.value(u"multiline").toString(), u"one\ninjected\u2028line"_s);
        }
    }
    QCOMPARE(snapshots, 1);
    QVERIFY(hasEvent(u"fy.trace.bandcamp.parser"_s, u"bandcamp://track/9"_s));
    capturedLogs().clear();
    const auto rejected = parseReleasePage(tralbumPage("[1,2]"), PageUrl, {.requestId = 9});
    QVERIFY(!isSuccess(rejected));
    QVERIFY(hasEvent(u"fy.trace.bandcamp.parser"_s, u"source_snapshot"_s, u"json=[1,2]"_s));
    capturedLogs().clear();
    QVERIFY(!isSuccess(parseReleasePage(tralbumPage("null"), PageUrl, {.requestId = 10})));
    QVERIFY(hasEvent(u"fy.trace.bandcamp.parser"_s, u"source_snapshot"_s, u"json=null"_s));
    capturedLogs().clear();
    QLoggingCategory::setFilterRules(u"fy.bandcamp.parser.debug=false\nfy.trace.bandcamp.parser.debug=false"_s);
    const auto quiet = parseReleasePage(body, PageUrl);
    QVERIFY(value<AlbumCandidate>(quiet));
    QCOMPARE(value<AlbumCandidate>(quiet)->artist.value, value<AlbumCandidate>(result)->artist.value);
    QVERIFY(!hasEvent(u"fy.trace.bandcamp.parser"_s, u"source_snapshot"_s));
}

void ParserTests::extractsOrderedStructuredListingReferences()
{
    const QByteArray listing
        = R"(<ol id="music-grid"><li data-item-id="album-3493169048"><a href="/album/lp8"><p class="title">ignored</p></a></li>)"
          R"(<li class="music-grid-item" data-item-id="track-3734052618"><a href="/track/ride-with-me">ignored</a></li></ol>)";
    const QUrl effectiveUrl{u"https://artist.bandcamp.com/music"_s};
    const auto outcome  = parseDiscographyPage(musicListing(listing), effectiveUrl);
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&outcome);
    QVERIFY(success);
    QCOMPARE(success->value.effectiveUrl, effectiveUrl);
    QCOMPARE(success->value.references.size(), 2);
    QCOMPARE(success->value.references.at(0).ordinal, 0);
    QCOMPARE(success->value.references.at(0).id, qint64{3493169048});
    QCOMPARE(success->value.references.at(0).page.kind, BandcampPageKind::Album);
    QCOMPARE(success->value.references.at(0).page.url, u"https://artist.bandcamp.com/album/lp8"_s);
    QCOMPARE(success->value.references.at(1).ordinal, 1);
    QCOMPARE(success->value.references.at(1).id, qint64{3734052618});
    QCOMPARE(success->value.references.at(1).page.kind, BandcampPageKind::Track);
    QCOMPARE(success->value.references.at(1).page.url, u"https://artist.bandcamp.com/track/ride-with-me"_s);
    QVERIFY(!success->value.continuation);
}

void ParserTests::htmlRecoveryPreservesStructuredSelection()
{
    const QByteArray listing
        = R"(<main><ol id="music-grid"><li data-item-id="album-1"><div><a href="/album/one">One</a></div>)"
          R"(<li data-item-id="track-2"><a href="/track/two">Two</a></ol></main>)"
          R"(<link rel="alternate NEXT" href="?page=2">)";
    const auto outcome  = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&outcome);
    QVERIFY(success);
    QCOMPARE(success->value.references.size(), 2);
    QCOMPARE(success->value.references.at(0).id, qint64{1});
    QCOMPARE(success->value.references.at(1).id, qint64{2});
    QVERIFY(success->value.continuation);
    QCOMPARE(success->value.continuation->locator, QUrl(u"https://artist.bandcamp.com/music?page=2"_s));
}

void ParserTests::conflictingHtmlAttributesFailDespiteRecovery()
{
    const QByteArray listing
        = R"(<ol id="music-grid"><li data-item-id="album-1" data-item-id="album-2"><a href="/album/one"></a></li></ol>)"
          R"(<main>Unknown HTML5 tags must not hide earlier errors.</main>)";
    const auto outcome  = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* failure = std::get_if<ResolutionFailure>(&outcome);
    QVERIFY(failure);
    QCOMPARE(failure->error.code, ResolutionErrorCode::MalformedPage);
}

void ParserTests::extractsClientItemsAsOrderedReferences()
{
    const QByteArray listing
        = R"(<ol id="music-grid" data-client-items='[)"
          R"({"type":"album","id":41,"page_url":"/album/rendered","title":false,"artist":{"bad":true},"art_id":[]},)"
          R"({"type":"track","id":42,"page_url":"/track/song","title":17,"artist":false},)"
          R"({"type":"album","id":43,"page_url":"/album/tail"})"
          R"(]'><li data-item-id="album-41"><a href="/album/rendered"></a></li></ol>)";
    const auto outcome  = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&outcome);
    QVERIFY(success);
    QCOMPARE(success->value.references.size(), 3);
    QCOMPARE(success->value.references.at(0).ordinal, 0);
    QCOMPARE(success->value.references.at(0).id, qint64{41});
    QCOMPARE(success->value.references.at(0).page.url, u"https://artist.bandcamp.com/album/rendered"_s);
    QCOMPARE(success->value.references.at(1).ordinal, 1);
    QCOMPARE(success->value.references.at(1).id, qint64{42});
    QCOMPARE(success->value.references.at(1).page.kind, BandcampPageKind::Track);
    QCOMPARE(success->value.references.at(1).page.url, u"https://artist.bandcamp.com/track/song"_s);
    QCOMPARE(success->value.references.at(2).ordinal, 2);
    QCOMPARE(success->value.references.at(2).id, qint64{43});
    QCOMPARE(success->value.references.at(2).page.url, u"https://artist.bandcamp.com/album/tail"_s);
}

void ParserTests::ignoresLargeOpaquePageData()
{
    const QByteArray blob = R"({"cfg":")" + QByteArray(MaxSourceFieldChars + 1, 'x') + R"("})";
    const QByteArray listing
        = "<div id=\"pagedata\" data-blob='" + blob
        + "'></div><ol id=\"music-grid\"><li data-item-id=\"album-1\"><a href=\"/album/one\"></a></li></ol>";
    const auto parsed   = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&parsed);
    QVERIFY(success);
    QCOMPARE(success->value.references.size(), 1);
    QCOMPARE(success->value.references.first().page.url, u"https://artist.bandcamp.com/album/one"_s);
}

void ParserTests::clientItemsHaveIndependentCollectionBounds()
{
    const QList<QByteArray> malformedItems{
        R"([{"type":false,"id":1,"page_url":"/album/one"}])",
        R"([{"type":"album","id":"1","page_url":"/album/one"}])",
        R"([{"type":"album","id":1,"page_url":5}])",
    };
    for(const QByteArray& invalid : malformedItems) {
        const auto outcome
            = parseDiscographyPage(musicListing("<ol id=\"music-grid\" data-client-items='" + invalid + "'></ol>"),
                                   QUrl(u"https://artist.bandcamp.com/music"_s));
        QVERIFY(std::holds_alternative<ResolutionFailure>(outcome));
    }

    QByteArray items{"["};
    for(int i = 0; i < 400; ++i) {
        if(i > 0) {
            items += ',';
        }
        items += R"({"type":"album","id":)" + QByteArray::number(i + 1) + R"(,"page_url":"/album/entry-)"
               + QByteArray::number(i + 1) + R"("})";
    }
    items += ']';
    const QByteArray listing = "<ol id=\"music-grid\" data-client-items='" + items + "'></ol>";
    const auto parsed   = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&parsed);
    QVERIFY(success);
    QVERIFY(items.size() > MaxSourceFieldChars);
    QCOMPARE(success->value.references.size(), 400);

    const QByteArray nested = R"([{"type":"album","id":1,"page_url":"/album/one","extra":)"
                            + QByteArray(MaxJsonDepth - 1, '[') + "0" + QByteArray(MaxJsonDepth - 1, ']') + "}]";
    const auto tooDeep
        = parseDiscographyPage(musicListing("<ol id=\"music-grid\" data-client-items='" + nested + "'></ol>"),
                               QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* depthFailure = std::get_if<ResolutionFailure>(&tooDeep);
    QVERIFY(depthFailure);
    QCOMPARE(depthFailure->error.code, ResolutionErrorCode::ResourceLimitExceeded);

    QByteArray oversized
        = musicListing(R"(<ol id="music-grid"><li data-item-id="album-1"><a href="/album/one"></a></li></ol>)");
    oversized.append(QByteArray(MaxPageBytes - oversized.size() + 1, ' '));
    const auto tooLarge     = parseDiscographyPage(oversized, QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* sizeFailure = std::get_if<ResolutionFailure>(&tooLarge);
    QVERIFY(sizeFailure);
    QCOMPARE(sizeFailure->error.code, ResolutionErrorCode::PageTooLarge);
}

void ParserTests::htmlDepthLimitFailsExplicitly()
{
    QByteArray listing;
    for(qsizetype i = 0; i <= MaxHtmlDepth; ++i) {
        listing += "<div>";
    }
    listing += R"(<ol id="music-grid"><li data-item-id="album-1"><a href="/album/one"></a></li></ol>)";
    for(qsizetype i = 0; i <= MaxHtmlDepth; ++i) {
        listing += "</div>";
    }

    const auto parsed   = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* failure = std::get_if<ResolutionFailure>(&parsed);
    QVERIFY(failure);
    QCOMPARE(failure->error.code, ResolutionErrorCode::ResourceLimitExceeded);
}

void ParserTests::malformedListingTailFailsClosed()
{
    const QByteArray validGrid
        = R"(<ol id="music-grid"><li data-item-id="album-1"><a href="/album/one"></a></li></ol>)";
    const QList<QByteArray> malformedTails{
        "<div data-tail=\"" + QByteArray(MaxSourceFieldChars + 1, 'x') + R"("><a rel="next" href="?page=2"></a></div>)",
        "<!-- hidden <link rel=\"next\" href=\"?page=2\">",
        R"(<script type="text/template" id="music-grid-template">)",
        R"(<div data-client-items="[]"></div>)",
        R"(<div class="load-more"></div>)",
    };
    for(const QByteArray& tail : malformedTails) {
        const auto outcome
            = parseDiscographyPage(musicListing(validGrid + tail), QUrl(u"https://artist.bandcamp.com/music"_s));
        const auto* failure = std::get_if<ResolutionFailure>(&outcome);
        QVERIFY(failure);
        QVERIFY(failure->error.code == ResolutionErrorCode::MalformedPage
                || failure->error.code == ResolutionErrorCode::ResourceLimitExceeded);
    }
}

void ParserTests::scriptTextIsNotContinuationMarkup()
{
    const QByteArray listing
        = R"(<ol id="music-grid"><li data-item-id="album-1"><a href="/album/one"></a></li></ol>)"
          R"(<script>const example = "data-client-items <link rel='next' href='?page=2'>";</script>)"
          R"(<!-- data-client-items <link rel="next" href="?page=3"> -->)"
          R"(<bandcamp-widget>ignored</bandcamp-widget>)";
    const auto parsed   = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&parsed);
    QVERIFY(success);
    QCOMPARE(success->value.references.size(), 1);
    QVERIFY(!success->value.continuation);
}

void ParserTests::extractsOpaqueContinuationLocator()
{
    const QByteArray listing
        = R"(<link rel="next" href="?page=2"><ol id="music-grid"><li data-item-id="album-1"><a href="/album/one"></a></li></ol>)";
    const QUrl effectiveUrl{u"https://artist.bandcamp.com/music"_s};
    const auto outcome  = parseDiscographyPage(musicListing(listing), effectiveUrl);
    const auto* success = std::get_if<ResolutionSuccess<BandcampListingBatch>>(&outcome);
    QVERIFY(success);
    QVERIFY(success->value.continuation);
    QCOMPARE(success->value.continuation->locator, QUrl(u"https://artist.bandcamp.com/music?page=2"_s));
}

void ParserTests::malformedListingEntriesFailExplicitly_data()
{
    QTest::addColumn<QByteArray>("listing");
    QTest::newRow("missing-identity") << QByteArray(R"(<ol id="music-grid"><li><a href="/album/one"></a></li></ol>)");
    QTest::newRow("unsupported-identity")
        << QByteArray(R"(<ol id="music-grid"><li data-item-id="merch-1"><a href="/album/one"></a></li></ol>)");
    QTest::newRow("zero-identity") << QByteArray(
        R"(<ol id="music-grid"><li data-item-id="album-0"><a href="/album/zero"></a></li></ol>)");
    QTest::newRow("kind-disagreement") << QByteArray(
        R"(<ol id="music-grid"><li data-item-id="track-7"><a href="/album/wrong"></a></li></ol>)");
    QTest::newRow("missing-link") << QByteArray(R"(<ol id="music-grid"><li data-item-id="album-8"></li></ol>)");
    QTest::newRow("external-link") << QByteArray(
        R"(<ol id="music-grid"><li data-item-id="album-9"><a href="https://other.example/album/x"></a></li></ol>)");
    QTest::newRow("truncated-list") << QByteArray(
        R"(<ol id="music-grid"><li data-item-id="album-10"><a href="/album/x)");
}

void ParserTests::malformedListingEntriesFailExplicitly()
{
    QFETCH(QByteArray, listing);
    const auto outcome  = parseDiscographyPage(musicListing(listing), QUrl(u"https://artist.bandcamp.com/music"_s));
    const auto* failure = std::get_if<ResolutionFailure>(&outcome);
    QVERIFY(failure);
    QCOMPARE(failure->error.stage, ResolutionStage::Parse);
}
} // namespace

int runParserTests(int argc, char** argv)
{
    ParserTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampparsertests.moc"
