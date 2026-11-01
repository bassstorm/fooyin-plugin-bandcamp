#pragma once

#include <QString>

#include <optional>

namespace Fooyin::Bandcamp {
[[nodiscard]] QString bandcampTrackUri(qint64 trackId);
[[nodiscard]] std::optional<qint64> parseBandcampTrackUri(const QString& uri);
} // namespace Fooyin::Bandcamp
