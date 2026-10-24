#pragma once

#include "bandcampdiagnostics.h"
#include "bandcampmodel.h"

#include <QByteArray>
#include <QStringList>

namespace Fooyin::Bandcamp {
// Temporary bridge for the pre-admission service and manual probe. Remove in
// Increment 3. Warnings here belong only to the old processors, never to source
// extraction.
template <typename T> struct LegacyPageResult
{
    T value;
    QString error;
    QStringList warnings;
    [[nodiscard]] bool isValid() const
    {
        return error.isEmpty();
    }
};

using LegacyAlbumResult = LegacyPageResult<BandcampAlbum>;
using LegacyTrackResult = LegacyPageResult<BandcampTrack>;

LegacyAlbumResult parseLegacyAlbumPage(const QByteArray& body, const QUrl& url, CorrelationContext context = {});
LegacyTrackResult parseLegacyTrackPage(const QByteArray& body, const QUrl& url, CorrelationContext context = {});
} // namespace Fooyin::Bandcamp
