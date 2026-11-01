#pragma once

#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QString>

#include <atomic>
#include <optional>
#include <variant>

namespace Fooyin::Bandcamp::Private {
// Shared with release attribute extraction; decoding is single-pass.
QString decodeHtmlEntities(const QString& text, const std::atomic_bool* stop);

struct HtmlLimits
{
    qsizetype pageBytes;
    // Maximum ancestors per element; nodes and attributes share the record budget.
    qsizetype depth;
    qsizetype fieldChars;
    qsizetype records;
};

using HtmlAttributes = QHash<QString, QString>;

struct HtmlTag
{
    QString name;
    HtmlAttributes attributes;
    qsizetype depth{0};
    bool closing{false};
};

struct HtmlEnd {};
struct HtmlCancelled {};
struct HtmlError
{
    enum Kind : quint8 { Malformed, ResourceLimit };
    Kind kind;
    qsizetype offset;
};
using HtmlRead = std::variant<HtmlTag, HtmlEnd, HtmlError, HtmlCancelled>;

// Borrowed input, one tag at a time. Only open element names are retained.
// Depth reflects the limited ordered-list closure rules, not browser tree repair.
class HtmlReader
{
public:
    HtmlReader(QByteArrayView body, HtmlLimits limits, const std::atomic_bool* stop = nullptr);
    HtmlRead next();

private:
    bool cancelled() const;
    bool record();
    bool advance();
    bool skipSpace();
    bool skipRawText();
    HtmlRead readTag();
    void setError(HtmlError::Kind kind);

    QByteArrayView m_body;
    HtmlLimits m_limits;
    const std::atomic_bool* m_stop;
    qsizetype m_cursor{0};
    qsizetype m_records{0};
    QList<QString> m_open;
    QString m_rawText;
    std::optional<HtmlError> m_error;
    bool m_ended{false};
};
} // namespace Fooyin::Bandcamp::Private
