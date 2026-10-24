#include "bandcampdirtytitle.h"

#include <QRegularExpression>

namespace Fooyin::Bandcamp {
DirtyTitle splitDirtyTitle(const QString& text)
{
    // Same shape as Gravifon's DirtyTitle.
    const QRegularExpression pattern(QStringLiteral(R"re(\A(.+?)\s[-_:.]\s(.+)\z)re"));
    const QRegularExpressionMatch match = pattern.match(text);
    if(!match.hasMatch()) {
        return {};
    }
    return {match.captured(1), match.captured(2), true};
}

bool sameArtist(const QString& a, const QString& b)
{
    return QString::compare(a, b, Qt::CaseInsensitive) == 0;
}
} // namespace Fooyin::Bandcamp
