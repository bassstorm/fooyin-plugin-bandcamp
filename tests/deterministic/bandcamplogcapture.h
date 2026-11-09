#pragma once

#include <QList>
#include <QLoggingCategory>
#include <QString>

#include <mutex>

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

inline std::mutex& capturedLogsMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline QList<CapturedLog> capturedLogsSnapshot()
{
    const std::scoped_lock lock{capturedLogsMutex()};
    return capturedLogs();
}

inline void captureLog(QtMsgType, const QMessageLogContext& context, const QString& message)
{
    const std::scoped_lock lock{capturedLogsMutex()};
    capturedLogs().append({QString::fromLatin1(context.category), message});
}

class LogCapture
{
public:
    explicit LogCapture(const QString& rules)
        : m_previous{qInstallMessageHandler(captureLog)}
    {
        const std::scoped_lock lock{capturedLogsMutex()};
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
