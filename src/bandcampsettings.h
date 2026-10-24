#pragma once

class QSettings;

namespace Fooyin::Bandcamp {
constexpr auto ParallelismKey = "Bandcamp/Parallelism";
constexpr auto ItemTimeoutKey = "Bandcamp/ItemTimeoutMs";

constexpr int DefaultParallelism   = 2;
constexpr int MinParallelism       = 1;
constexpr int MaxParallelism       = 8;
constexpr int DefaultItemTimeoutMs = 10000;
constexpr int MinItemTimeoutMs     = 1000;
constexpr int MaxItemTimeoutMs     = 120000;

struct BandcampSettings
{
    int parallelism{DefaultParallelism};
    int itemTimeoutMs{DefaultItemTimeoutMs};
};

// Missing or non-numeric values fall back to defaults. Out-of-range values are clamped. Both are logged.
BandcampSettings loadBandcampSettings(const QSettings& settings);
void saveBandcampSettings(QSettings& settings, const BandcampSettings& values);
} // namespace Fooyin::Bandcamp
