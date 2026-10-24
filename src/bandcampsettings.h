#pragma once

#include <QtGlobal>

class QSettings;

namespace Fooyin::Bandcamp {
constexpr auto ParallelismKey              = "Bandcamp/Parallelism";
constexpr auto ItemTimeoutKey              = "Bandcamp/ItemTimeoutMs";
constexpr auto DiscographyOrderKey         = "Bandcamp/DiscographyOrder";
constexpr auto PrePlaybackRefreshWindowKey = "Bandcamp/PrePlaybackRefreshWindowMinutes";

constexpr int DefaultParallelism          = 2;
constexpr int MinParallelism              = 1;
constexpr int MaxParallelism              = 8;
constexpr int DefaultItemTimeoutMs        = 10000;
constexpr int MinItemTimeoutMs            = 1000;
constexpr int MaxItemTimeoutMs            = 120000;
constexpr int DefaultRefreshWindowMinutes = 60;
constexpr int MinRefreshWindowMinutes     = 0;
constexpr int MaxRefreshWindowMinutes     = 1440;
constexpr qint64 RefreshWindowMsPerMinute = 60'000;

enum class BandcampDiscographyOrder : int
{
    Default,
    Unsorted,
    Ascending,
    Descending,
};

struct BandcampSettings
{
    int parallelism{DefaultParallelism};
    int itemTimeoutMs{DefaultItemTimeoutMs};
    BandcampDiscographyOrder discographyOrder{BandcampDiscographyOrder::Default};
    int prePlaybackRefreshWindowMinutes{DefaultRefreshWindowMinutes};
};

// Missing or non-numeric values fall back to defaults. Out-of-range values are clamped. Both are logged.
BandcampSettings loadBandcampSettings(const QSettings& settings);
void saveBandcampSettings(QSettings& settings, const BandcampSettings& values);
qint64 refreshWindowMilliseconds(int minutes);
} // namespace Fooyin::Bandcamp
