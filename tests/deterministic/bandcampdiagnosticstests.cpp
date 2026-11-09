#include "bandcampattemptsupervisor.h"
#include "bandcampdiagnostics.h"
#include "bandcamplogcapture.h"
#include "bandcamplogging.h"
#include "bandcampoutcome.h"

#include <QtTest>

using namespace Qt::StringLiterals;
using namespace Fooyin::Bandcamp;

namespace {
int g_formatCalls{0};

QString countedRecord()
{
    ++g_formatCalls;
    return formatTraceEvent(CorrelationContext{}, u"probe"_s, {});
}

class DiagnosticsTests : public QObject
{
    Q_OBJECT

private slots:
    void escapesDelimitersAndControls();
    void capsFieldLengthWithoutSplittingSurrogates();
    void cleansUrlsForLogs();
    void keepsRawTextUnscanned();
    void formatsCorrelatedSingleLineRecord();
    void usesAlbumAndTrackKeysWithoutRepeatingUrl();
    void usesUrlKeyOnlyWhenNoEntityIsKnown();
    void resolutionRetryIsSingleWarning();
    void resolutionFailureIsSingleWarningWithKnownAlbum();
    void capsFieldCountAndRecordLength();
    void traceIsFilteredSeparatelyFromDebug();
    void disabledTraceSkipsFormatting();
};

void DiagnosticsTests::escapesDelimitersAndControls()
{
    QCOMPARE(escapeDiagnosticText(u"a\"b\\c"_s), u"a\\\"b\\\\c"_s);
    QCOMPARE(escapeDiagnosticText(u"x\ny\rz\tw"_s), u"x\\ny\\rz\\tw"_s);
    QCOMPARE(escapeDiagnosticText(QString(QChar(0x01))), u"\\u0001"_s);
    QCOMPARE(escapeDiagnosticText(QString(QChar(0x2028))), u"\\u2028"_s);
    QCOMPARE(escapeDiagnosticText(u"plain text"_s), u"plain text"_s);
}

void DiagnosticsTests::capsFieldLengthWithoutSplittingSurrogates()
{
    const QString longText = QString(1000, u'a');
    const QString capped   = escapeDiagnosticText(longText);
    QCOMPARE(capped.size(), MaxDiagnosticFieldChars);
    QVERIFY(capped.endsWith(u"..."_s));

    // 252 ASCII characters, then one surrogate pair that would straddle the cap.
    const QString straddling = QString(252, u'a') + u"\U0001F600"_s + u"b"_s;
    const QString cut        = escapeDiagnosticText(straddling);
    QVERIFY(cut.endsWith(u"..."_s));
    QVERIFY(!cut.chopped(3).back().isHighSurrogate());
}

void DiagnosticsTests::cleansUrlsForLogs()
{
    QCOMPARE(cleanUrlForLog(QUrl(u"https://user:pw@artist.bandcamp.com/track/y?sig=SECRET#frag"_s)),
             u"https://artist.bandcamp.com/track/y"_s);
    QCOMPARE(cleanUrlForLog(QUrl(u"http://[bad"_s)), u"<invalid url>"_s);

    const DiagnosticField field = DiagnosticField::url(u"url"_s, QUrl(u"https://a.bandcamp.com/t/z?sig=SECRET"_s));
    QCOMPARE(field.value, u"https://a.bandcamp.com/t/z"_s);
}

void DiagnosticsTests::keepsRawTextUnscanned()
{
    // Free-form text is never regex-scanned, so raw source_snapshot JSON keeps its stream URLs verbatim.
    const QString json          = u"{\"file\":{\"mp3-128\":\"https://cdn.example.com/song.mp3?sig=KEEP\"}}"_s;
    const DiagnosticField field = DiagnosticField::text(u"json"_s, json);
    QCOMPARE(field.value, json);
}

void DiagnosticsTests::formatsCorrelatedSingleLineRecord()
{
    const CorrelationContext context{.albumId = 3};
    const QString record = formatTraceEvent(
        context, u"fetch"_s,
        {DiagnosticField::url(u"url"_s, QUrl(u"https://a.bandcamp.com/album/x?sig=SECRET"_s)),
         DiagnosticField::number(u"status"_s, 200), DiagnosticField::text(u"error"_s, u"line1\nline2"_s)});

    QCOMPARE(record, u"album=3 fetch url=https://a.bandcamp.com/album/x status=200 error=\"line1\\nline2\""_s);
    QVERIFY(!record.contains(u"SECRET"_s));
    QVERIFY(!record.contains(u'\n'));
}

void DiagnosticsTests::usesAlbumAndTrackKeysWithoutRepeatingUrl()
{
    QCOMPARE(formatTraceEvent(CorrelationContext{.albumId = 2}, u"probe"_s, {}), u"album=2 probe"_s);
    QCOMPARE(formatTraceEvent(CorrelationContext{.trackId = 9}, u"probe"_s, {}), u"track=9 probe"_s);

    const CorrelationContext both{
        .albumId = 2, .trackId = 9, .resourceUrl = QUrl(u"https://a.bandcamp.com/album/x?sig=SECRET"_s)};
    QCOMPARE(formatTraceEvent(both, u"probe"_s, {}), u"album=2 track=9 probe"_s);
}

void DiagnosticsTests::usesUrlKeyOnlyWhenNoEntityIsKnown()
{
    const CorrelationContext context{.resourceUrl = QUrl(u"https://a.bandcamp.com/music?sig=SECRET#top"_s)};
    QCOMPARE(formatTraceEvent(context, u"probe"_s, {}), u"url=https://a.bandcamp.com/music probe"_s);

    const CorrelationContext withAlbum = context.withAlbum(7);
    QCOMPARE(formatTraceEvent(withAlbum, u"probe"_s, {}), u"album=7 probe"_s);
    QCOMPARE(context.albumId, 0);
}

void DiagnosticsTests::resolutionRetryIsSingleWarning()
{
    LogCapture capture(u"fy.bandcamp.resolve.debug=true"_s);
    ResolutionError error;
    error.code   = ResolutionErrorCode::Timeout;
    error.stage  = ResolutionStage::Fetch;
    error.reason = u"attempt timed out"_s;
    BandcampAttemptSupervisor attempts(
        BandcampAttemptPolicy{.attemptTimeoutMs = 1000, .maxAttempts = 3, .cooldownMs = 250});
    attempts.resetBudget();
    attempts.beginRetry();
    logResolutionRetry(CorrelationContext{.albumId = 5}, error, 0, attempts);

    QCOMPARE(capturedLogs().size(), 1);
    QCOMPARE(capturedLogs().first().category, u"fy.bandcamp.resolve"_s);
    QCOMPARE(capturedLogs().first().message,
             u"album=5 retry attempt=2/3 delayMs=250 code=timeout reason=\"attempt timed out\""_s);
}

void DiagnosticsTests::resolutionFailureIsSingleWarningWithKnownAlbum()
{
    LogCapture capture(u"fy.bandcamp.resolve.debug=true"_s);
    ResolutionError error;
    error.code    = ResolutionErrorCode::RequiredFieldMissing;
    error.stage   = ResolutionStage::Admit;
    error.albumId = 11;
    error.trackId = 12;
    error.field   = u"track.streamUrl"_s;
    error.value   = u"null"_s;
    error.reason  = u"stream missing"_s;
    logResolutionFailure(CorrelationContext{.resourceUrl = QUrl(u"https://a.bandcamp.com/album/x?sig=SECRET"_s)}, error,
                         0);

    QCOMPARE(capturedLogs().size(), 1);
    QCOMPARE(capturedLogs().first().category, u"fy.bandcamp.resolve"_s);
    QCOMPARE(capturedLogs().first().message,
             u"album=11 track=12 failed stage=admit code=required_field_missing field=track.streamUrl value=null "
             u"reason=\"stream missing\" url=https://a.bandcamp.com/album/x"_s);
}

void DiagnosticsTests::capsFieldCountAndRecordLength()
{
    QList<DiagnosticField> fields;
    for(int i = 0; i < 20; ++i) {
        fields.append(DiagnosticField::text(u"f%1"_s.arg(i), u"v"_s));
    }
    const QString few = formatTraceEvent(CorrelationContext{}, u"probe"_s, fields);
    QVERIFY(few.contains(u"f15=v"_s));
    QVERIFY(!few.contains(u"f16="_s));
    QVERIFY(few.endsWith(u"fields_omitted=4"_s));

    QList<DiagnosticField> large;
    for(int i = 0; i < MaxDiagnosticFields; ++i) {
        large.append(DiagnosticField::text(u"f%1"_s.arg(i), QString(1000, u'x')));
    }
    const QString capped = formatTraceEvent(CorrelationContext{}, u"probe"_s, large);
    QCOMPARE(capped.size(), MaxDiagnosticRecordChars);
    QVERIFY(capped.endsWith(u"..."_s));
}

void DiagnosticsTests::traceIsFilteredSeparatelyFromDebug()
{
    {
        LogCapture capture(u"fy.bandcamp.*.debug=true\nfy.trace.bandcamp.*.debug=false"_s);
        qCDebug(BANDCAMP_FETCH) << "ordinary debug";
        qCDebug(BANDCAMP_TRACE_FETCH) << "trace";
        QCOMPARE(capturedLogs().size(), 1);
        QCOMPARE(capturedLogs().first().category, u"fy.bandcamp.fetch"_s);
    }
    {
        LogCapture capture(u"fy.trace.bandcamp.*.debug=true"_s);
        qCDebug(BANDCAMP_TRACE_FETCH) << "trace";
        QCOMPARE(capturedLogs().size(), 1);
        QCOMPARE(capturedLogs().first().category, u"fy.trace.bandcamp.fetch"_s);
    }
}

void DiagnosticsTests::disabledTraceSkipsFormatting()
{
    LogCapture capture(u"fy.trace.bandcamp.*.debug=false"_s);
    g_formatCalls = 0;
    qCDebug(BANDCAMP_TRACE_FETCH).noquote() << countedRecord();
    QCOMPARE(g_formatCalls, 0);

    QLoggingCategory::setFilterRules(u"fy.trace.bandcamp.*.debug=true"_s);
    qCDebug(BANDCAMP_TRACE_FETCH).noquote() << countedRecord();
    QCOMPARE(g_formatCalls, 1);
}
} // namespace

int runDiagnosticsTests(int argc, char** argv)
{
    DiagnosticsTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "bandcampdiagnosticstests.moc"
