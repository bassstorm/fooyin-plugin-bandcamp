#include "bandcamphtmlreader_p.h"

#include <QStringDecoder>

namespace Fooyin::Bandcamp::Private {
namespace {
bool space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

char lower(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

bool letter(char ch)
{
    ch = lower(ch);
    return ch >= 'a' && ch <= 'z';
}

bool nameChar(char ch)
{
    return letter(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == ':';
}

bool isVoid(const QString& name)
{
    return name == u"area" || name == u"base" || name == u"br" || name == u"col" || name == u"embed"
        || name == u"hr" || name == u"img" || name == u"input" || name == u"link" || name == u"meta"
        || name == u"param" || name == u"source" || name == u"track" || name == u"wbr";
}

bool isRaw(const QString& name)
{
    return name == u"script" || name == u"style" || name == u"title" || name == u"textarea" || name == u"xmp"
        || name == u"iframe" || name == u"noembed" || name == u"noframes" || name == u"noscript"
        || name == u"plaintext";
}

bool decodeEntity(QStringView name, QString& out)
{
    if(name == u"amp") {
        out = QStringLiteral("&");
    }
    else if(name == u"quot") {
        out = QStringLiteral("\"");
    }
    else if(name == u"apos") {
        out = QStringLiteral("'");
    }
    else if(name == u"lt") {
        out = QStringLiteral("<");
    }
    else if(name == u"gt") {
        out = QStringLiteral(">");
    }
    else {
        if(name.isEmpty() || name.at(0) != u'#') {
            return false;
        }
        const bool hex    = name.size() > 1 && (name.at(1) == u'x' || name.at(1) == u'X');
        const auto digits = name.mid(hex ? 2 : 1);
        if(digits.isEmpty() || digits.size() > 8) {
            return false;
        }
        bool ok         = false;
        const uint code = digits.toUInt(&ok, hex ? 16 : 10);
        if(!ok || code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        const char32_t ch = static_cast<char32_t>(code);
        out               = QString::fromUcs4(&ch, 1);
    }
    return true;
}
} // namespace

QString decodeHtmlEntities(const QString& text, const std::atomic_bool* stop)
{
    QString out;
    out.reserve(text.size());
    for(qsizetype i = 0; i < text.size();) {
        if((i & 0x3ff) == 0 && stop && stop->load(std::memory_order_relaxed)) {
            return {};
        }
        if(text.at(i) == u'&') {
            const qsizetype end = qMin(text.size(), i + 12);
            qsizetype semi      = i + 1;
            while(semi < end && text.at(semi) != u';') {
                ++semi;
            }
            QString decoded;
            if(semi < end && decodeEntity(QStringView(text).mid(i + 1, semi - i - 1), decoded)) {
                out += decoded;
                i = semi + 1;
                continue;
            }
        }
        out += text.at(i++);
    }
    return out;
}

HtmlReader::HtmlReader(QByteArrayView body, HtmlLimits limits, const std::atomic_bool* stop)
    : m_body{body}
    , m_limits{limits}
    , m_stop{stop}
{
    if(body.size() > limits.pageBytes) {
        setError(HtmlError::ResourceLimit);
        return;
    }
    // Validate even ignored text, without copying large opaque attributes.
    QStringDecoder decoder(QStringDecoder::Utf8);
    for(qsizetype offset = 0; offset < body.size() && !cancelled(); offset += 4096) {
        [[maybe_unused]] const QString chunk = decoder(body.sliced(offset, qMin(qsizetype{4096}, body.size() - offset)));
        if(decoder.hasError()) {
            setError(HtmlError::Malformed);
            return;
        }
    }
    // Flush a potentially unfinished UTF-8 sequence.
    [[maybe_unused]] const QString tail = decoder(QByteArrayView("\0", 1));
    if(decoder.hasError()) {
        setError(HtmlError::Malformed);
    }
}

bool HtmlReader::cancelled() const
{
    return m_stop && m_stop->load(std::memory_order_relaxed);
}

void HtmlReader::setError(HtmlError::Kind kind)
{
    m_error = HtmlError{kind, m_cursor};
}

bool HtmlReader::record()
{
    if(++m_records > m_limits.records) {
        setError(HtmlError::ResourceLimit);
        return false;
    }
    return true;
}

bool HtmlReader::advance()
{
    if(cancelled()) {
        return false;
    }
    ++m_cursor;
    return true;
}

bool HtmlReader::skipSpace()
{
    while(m_cursor < m_body.size() && space(m_body.at(m_cursor))) {
        if(!advance()) {
            return false;
        }
    }
    return !cancelled();
}

bool HtmlReader::skipRawText()
{
    const QByteArray name = m_rawText.toLatin1();
    while(m_cursor < m_body.size()) {
        if(m_rawText != u"plaintext" && m_body.at(m_cursor) == '<'
           && m_body.size() - m_cursor > name.size() + 2 && m_body.at(m_cursor + 1) == '/') {
            bool matches = true;
            for(qsizetype i = 0; i < name.size(); ++i) {
                if(lower(m_body.at(m_cursor + 2 + i)) != name.at(i)) {
                    matches = false;
                    break;
                }
            }
            const char after = m_body.at(m_cursor + 2 + name.size());
            if(matches && (space(after) || after == '>')) {
                m_rawText.clear();
                return true;
            }
        }
        if(!advance()) {
            return false;
        }
    }
    if(m_rawText != u"plaintext") {
        setError(HtmlError::Malformed);
        return false;
    }
    m_rawText.clear();
    m_open.removeLast();
    return true;
}

HtmlRead HtmlReader::readTag()
{
    const auto malformed = [this]() -> HtmlRead {
        setError(HtmlError::Malformed);
        return *m_error;
    };
    ++m_cursor; // '<' was checked by next().
    HtmlTag tag;
    if(m_cursor < m_body.size() && m_body.at(m_cursor) == '/') {
        tag.closing = true;
        ++m_cursor;
    }
    if(m_cursor == m_body.size() || !letter(m_body.at(m_cursor))) {
        return malformed();
    }
    const qsizetype nameStart = m_cursor;
    while(m_cursor < m_body.size() && nameChar(m_body.at(m_cursor))) {
        if(!advance()) {
            return HtmlCancelled{};
        }
    }
    if(m_cursor - nameStart > m_limits.fieldChars || (!tag.closing && !record())) {
        setError(HtmlError::ResourceLimit);
        return *m_error;
    }
    tag.name = QString::fromLatin1(m_body.sliced(nameStart, m_cursor - nameStart)).toLower();
    bool selfClosing{false};
    while(m_cursor < m_body.size()) {
        if(!skipSpace()) {
            return HtmlCancelled{};
        }
        if(m_cursor == m_body.size()) {
            return malformed();
        }
        const char ch = m_body.at(m_cursor);
        if(ch == '>') {
            ++m_cursor;
            break;
        }
        if(ch == '/' && !tag.closing && m_cursor + 1 < m_body.size() && m_body.at(m_cursor + 1) == '>') {
            m_cursor += 2;
            selfClosing = true;
            break;
        }
        if(tag.closing) {
            return malformed();
        }
        const qsizetype attrStart = m_cursor;
        while(m_cursor < m_body.size() && !space(m_body.at(m_cursor)) && m_body.at(m_cursor) != '='
              && m_body.at(m_cursor) != '>' && m_body.at(m_cursor) != '/') {
            const char attr = m_body.at(m_cursor);
            if(attr == '<' || attr == '"' || attr == '\'' || attr == '\0') {
                return malformed();
            }
            if(!advance()) {
                return HtmlCancelled{};
            }
        }
        if(m_cursor == attrStart) {
            return malformed();
        }
        if(!record()) {
            return *m_error;
        }
        const QString name = QString::fromUtf8(m_body.sliced(attrStart, m_cursor - attrStart)).toLower();
        if(name.size() > m_limits.fieldChars) {
            setError(HtmlError::ResourceLimit);
            return *m_error;
        }
        if(tag.attributes.contains(name)) {
            return malformed();
        }
        if(!skipSpace()) {
            return HtmlCancelled{};
        }
        qsizetype valueStart = m_cursor;
        qsizetype valueSize{0};
        if(m_cursor < m_body.size() && m_body.at(m_cursor) == '=') {
            ++m_cursor;
            if(!skipSpace()) {
                return HtmlCancelled{};
            }
            if(m_cursor == m_body.size()) {
                return malformed();
            }
            const char quote = m_body.at(m_cursor);
            const bool quoted = quote == '"' || quote == '\'';
            if(quoted) {
                ++m_cursor;
            }
            valueStart = m_cursor;
            while(m_cursor < m_body.size()
                  && (quoted ? m_body.at(m_cursor) != quote
                             : !space(m_body.at(m_cursor)) && m_body.at(m_cursor) != '>')) {
                const char value = m_body.at(m_cursor);
                if(!quoted && (value == '<' || value == '"' || value == '\'' || value == '=' || value == '`')) {
                    return malformed();
                }
                if(!advance()) {
                    return HtmlCancelled{};
                }
            }
            valueSize = m_cursor - valueStart;
            if(m_cursor == m_body.size() || (!quoted && valueSize == 0)) {
                return malformed();
            }
            if(quoted) {
                ++m_cursor;
            }
        }
        QString value;
        if(name != u"data-blob") {
            value = decodeHtmlEntities(QString::fromUtf8(m_body.sliced(valueStart, valueSize)), m_stop);
            if(cancelled()) {
                return HtmlCancelled{};
            }
            const qsizetype limit = name == u"data-client-items" ? m_limits.pageBytes : m_limits.fieldChars;
            if(value.size() > limit) {
                setError(HtmlError::ResourceLimit);
                return *m_error;
            }
        }
        tag.attributes.insert(name, std::move(value));
    }
    if(m_cursor == 0 || m_body.at(m_cursor - 1) != '>') {
        return malformed();
    }
    if(tag.closing) {
        // Inline SVG can use an unclosed reference leaf. This does not
        // relax nesting for arbitrary unknown elements.
        if(isVoid(tag.name) || (tag.name == u"use" && m_open.contains(QStringLiteral("svg")))) {
            tag.depth = m_open.size();
            return tag;
        }
        qsizetype index = m_open.size() - 1;
        if(tag.name == u"ol" || tag.name == u"ul") {
            while(index >= 0 && m_open.at(index) != u"ol" && m_open.at(index) != u"ul") {
                --index;
            }
            if(index >= 0 && index + 1 < m_open.size() && m_open.at(index + 1) != u"li") {
                return malformed();
            }
        }
        if(index < 0 || m_open.at(index) != tag.name) {
            // A stray non-list end tag cannot change ancestry. Do not repair
            // a mismatched open ancestor or discard an earlier error.
            if(tag.name != u"ol" && tag.name != u"ul" && tag.name != u"li" && !m_open.contains(tag.name)) {
                tag.depth = m_open.size();
                return tag;
            }
            return malformed();
        }
        tag.depth = index;
        m_open.resize(index);
    }
    else {
        if(tag.name == u"li") {
            qsizetype index = m_open.size() - 1;
            while(index >= 0 && m_open.at(index) != u"ol" && m_open.at(index) != u"ul") {
                --index;
            }
            if(index >= 0 && m_open.at(index) == u"ol" && index + 1 < m_open.size()
               && m_open.at(index + 1) == u"li") {
                m_open.resize(index + 1);
            }
        }
        tag.depth = m_open.size();
        if(tag.depth > m_limits.depth) {
            setError(HtmlError::ResourceLimit);
            return *m_error;
        }
        const bool svgLeaf = tag.name == u"use" && m_open.contains(QStringLiteral("svg"));
        const bool rawText = isRaw(tag.name);
        if((!selfClosing || rawText) && !isVoid(tag.name) && !svgLeaf) {
            m_open.append(tag.name);
            if(rawText) {
                m_rawText = tag.name;
            }
        }
    }
    return tag;
}

HtmlRead HtmlReader::next()
{
    if(cancelled()) {
        return HtmlCancelled{};
    }
    if(m_error) {
        return *m_error;
    }
    if(m_ended) {
        return HtmlEnd{};
    }
    if(!m_rawText.isEmpty()) {
        const qsizetype start = m_cursor;
        if(!skipRawText()) {
            return cancelled() ? HtmlRead{HtmlCancelled{}} : HtmlRead{*m_error};
        }
        if(m_cursor > start && !record()) {
            return *m_error;
        }
    }
    while(m_cursor < m_body.size()) {
        if(cancelled()) {
            return HtmlCancelled{};
        }
        if(m_body.at(m_cursor) != '<') {
            if(!record()) {
                return *m_error;
            }
            while(m_cursor < m_body.size() && m_body.at(m_cursor) != '<') {
                if(!advance()) {
                    return HtmlCancelled{};
                }
            }
            continue;
        }
        const auto remaining = m_body.sliced(m_cursor);
        const bool comment = remaining.startsWith("<!--");
        const bool cdata = remaining.startsWith("<![CDATA[");
        if(comment || cdata) {
            if(!record()) {
                return *m_error;
            }
            m_cursor += comment ? 4 : 9;
            const QByteArrayView terminator = comment ? QByteArrayView("-->") : QByteArrayView("]]>");
            while(m_cursor < m_body.size() && !m_body.sliced(m_cursor).startsWith(terminator)) {
                if(!advance()) {
                    return HtmlCancelled{};
                }
            }
            if(m_cursor == m_body.size()) {
                setError(HtmlError::Malformed);
                return *m_error;
            }
            m_cursor += 3;
            continue;
        }
        if(m_cursor + 1 < m_body.size() && (m_body.at(m_cursor + 1) == '!' || m_body.at(m_cursor + 1) == '?')) {
            if(!record()) {
                return *m_error;
            }
            m_cursor += 2;
            char quote{0};
            while(m_cursor < m_body.size()) {
                const char ch = m_body.at(m_cursor);
                if(!quote && ch == '>') {
                    break;
                }
                if(!quote && (ch == '<' || ch == '[')) {
                    setError(HtmlError::Malformed);
                    return *m_error;
                }
                if(quote == ch) {
                    quote = 0;
                }
                else if(!quote && (ch == '\'' || ch == '"')) {
                    quote = ch;
                }
                if(!advance()) {
                    return HtmlCancelled{};
                }
            }
            if(m_cursor == m_body.size()) {
                setError(HtmlError::Malformed);
                return *m_error;
            }
            ++m_cursor;
            continue;
        }
        return readTag();
    }
    // Document wrappers may omit their end tags; source-bearing elements may not.
    for(const QString& name : m_open) {
        if(name != u"html" && name != u"head" && name != u"body") {
            setError(HtmlError::Malformed);
            return *m_error;
        }
    }
    m_open.clear();
    m_ended = true;
    return HtmlEnd{};
}
} // namespace Fooyin::Bandcamp::Private
