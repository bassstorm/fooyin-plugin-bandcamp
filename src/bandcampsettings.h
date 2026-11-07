#pragma once

#include <QtGlobal>

class QSettings;

namespace Fooyin::Bandcamp {
constexpr auto ParallelismKey              = "Bandcamp/Parallelism";
constexpr auto DiscographyOrderKey         = "Bandcamp/DiscographyOrder";
constexpr auto PrePlaybackRefreshWindowKey = "Bandcamp/PrePlaybackRefreshWindowMinutes";
constexpr auto AttemptTimeoutKey           = "Bandcamp/AttemptTimeoutMs";
constexpr auto MaxAttemptsKey              = "Bandcamp/MaxAttempts";
constexpr auto AttemptCooldownKey          = "Bandcamp/AttemptCooldownMs";
constexpr auto ProbeStreamsOnImportKey     = "Bandcamp/ProbeStreamsOnImport";

constexpr int DefaultParallelism          = 2;
constexpr int MinParallelism              = 1;
constexpr int MaxParallelism              = 8;
constexpr int DefaultRefreshWindowMinutes = 60;
constexpr int MinRefreshWindowMinutes     = 0;
constexpr int MaxRefreshWindowMinutes     = 1440;
constexpr qint64 RefreshWindowMsPerMinute = 60'000;
constexpr int DefaultAttemptTimeoutMs     = 10000;
constexpr int MinAttemptTimeoutMs         = 1000;
constexpr int MaxAttemptTimeoutMs         = 30000;
constexpr int DefaultAttemptsPerPage      = 2;
constexpr int MinAttemptsPerPage          = 1;
constexpr int MaxAttemptsPerPage          = 10;
constexpr int DefaultAttemptCooldownMs    = 2000;
constexpr int MinAttemptCooldownMs        = 0;
constexpr int MaxAttemptCooldownMs        = 30000;

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
    int attemptTimeoutMs{DefaultAttemptTimeoutMs};
    BandcampDiscographyOrder discographyOrder{BandcampDiscographyOrder::Default};
    int prePlaybackRefreshWindowMinutes{DefaultRefreshWindowMinutes};
    int maxAttempts{DefaultAttemptsPerPage};
    int attemptCooldownMs{DefaultAttemptCooldownMs};
    bool probeStreamsOnImport{false};
};

struct OperationOptions
{
    int parallelism{DefaultParallelism};
    int attemptTimeoutMs{DefaultAttemptTimeoutMs};
    BandcampDiscographyOrder discographyOrder{BandcampDiscographyOrder::Default};
    qint64 prePlaybackRefreshWindowMs{DefaultRefreshWindowMinutes * RefreshWindowMsPerMinute};
    int maxAttempts{DefaultAttemptsPerPage};
    int attemptCooldownMs{DefaultAttemptCooldownMs};
};

// Missing or non-numeric values fall back to defaults. Out-of-range values are clamped. Both are logged.
BandcampSettings loadBandcampSettings(const QSettings& settings);
void saveBandcampSettings(QSettings& settings, const BandcampSettings& values);
qint64 refreshWindowMilliseconds(int minutes);
OperationOptions makeOperationOptions(const BandcampSettings& settings);
OperationOptions clampOperationOptions(OperationOptions options);
} // namespace Fooyin::Bandcamp
