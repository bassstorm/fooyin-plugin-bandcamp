#pragma once

#include <QString>

namespace Fooyin::Bandcamp {
struct DirtyTitle
{
    QString artist;
    QString title;
    bool matched{false};
};

// Splits "Artist - Title" when the text has that shape.
DirtyTitle splitDirtyTitle(const QString& text);

// Compares artist names ignoring case.
bool sameArtist(const QString& a, const QString& b);
} // namespace Fooyin::Bandcamp
