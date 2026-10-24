#include "bandcampdiagnostics.h"
#include "bandcamplogcapture.h"
#include "bandcamplogging.h"

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
    void redactsUrlsForLogs();
    void formatsCorrelatedSingleLineRecord();
    void leadsWithTrackUriWhenTrackIsKnown();
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

void DiagnosticsTests::redactsUrlsForLogs()
{
    QCOMPARE(redactUrlForLog(QUrl(u"https://user:pw@artist.bandcamp.com/track/y?sig=SECRET#frag"_s)),
             u"https://artist.bandcamp.com/track/y"_s);
    QCOMPARE(redactUrlForLog(QUrl(u"http://[bad"_s)), u"<invalid url>"_s);

    const DiagnosticField field = DiagnosticField::url(u"url"_s, QUrl(u"https://a.bandcamp.com/t/z?sig=SECRET"_s));
    QCOMPARE(field.value, u"https://a.bandcamp.com/t/z"_s);
    const auto text
        = DiagnosticField::text(u"detail"_s, u"failed at https://user:pass@cdn.example.com/song.mp3?sig=SECRET"_s);
    QCOMPARE(text.value, u"failed at https://cdn.example.com/song.mp3"_s);
}

void DiagnosticsTests::formatsCorrelatedSingleLineRecord()
{
    const CorrelationContext context{
        .jobId       = 3,
        .requestId   = 4,
        .resourceUrl = QUrl(u"https://a.bandcamp.com/album/x?sig=SECRET"_s),
    };
    const QString record = formatTraceEvent(
        context, u"request.finished"_s,
        {DiagnosticField::number(u"status"_s, 200), DiagnosticField::text(u"error"_s, u"line1\nline2"_s)});

    QCOMPARE(record, u"job=\"3\" request=\"4\" release=\"https://a.bandcamp.com/album/x\" request.finished "
                     u"status=\"200\" error=\"line1\\nline2\""_s);
    QVERIFY(!record.contains(u"SECRET"_s));
    QVERIFY(!record.contains(u'\n'));
}

void DiagnosticsTests::leadsWithTrackUriWhenTrackIsKnown()
{
    const CorrelationContext context{.trackId = 9};
    QCOMPARE(formatTraceEvent(context, u"probe"_s, {}), u"bandcamp://track/9 probe"_s);
}

void DiagnosticsTests::capsFieldCountAndRecordLength()
{
    QList<DiagnosticField> fields;
    for(int i = 0; i < 20; ++i) {
        fields.append(DiagnosticField::text(u"f%1"_s.arg(i), u"v"_s));
    }
    const QString few = formatTraceEvent(CorrelationContext{}, u"probe"_s, fields);
    QVERIFY(few.contains(u"f15=\"v\""_s));
    QVERIFY(!few.contains(u"f16="_s));
    QVERIFY(few.endsWith(u"fields_omitted=\"4\""_s));

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
