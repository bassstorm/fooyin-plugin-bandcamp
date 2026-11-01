#pragma once

#include <QList>
#include <QLoggingCategory>
#include <QString>

// Collects every Qt log message while in scope. Filter rules are applied for the scope and reset afterwards.
struct CapturedLog
{
    QString category;
    QString message;
};

inline QList<CapturedLog>& capturedLogs()
{
    static QList<CapturedLog> logs;
    return logs;
}

inline void captureLog(QtMsgType, const QMessageLogContext& context, const QString& message)
{
    capturedLogs().append({QString::fromLatin1(context.category), message});
}

class LogCapture
{
public:
    explicit LogCapture(const QString& rules)
        : m_previous{qInstallMessageHandler(captureLog)}
    {
        capturedLogs().clear();
        QLoggingCategory::setFilterRules(rules);
    }

    ~LogCapture()
    {
        qInstallMessageHandler(m_previous);
        QLoggingCategory::setFilterRules({});
    }

    LogCapture(const LogCapture&)            = delete;
    LogCapture& operator=(const LogCapture&) = delete;

private:
    QtMessageHandler m_previous;
};
