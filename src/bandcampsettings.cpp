#include "bandcampsettings.h"

#include "bandcamplogging.h"

#include <QSettings>

#include <algorithm>
#include <limits>

namespace Fooyin::Bandcamp {
namespace {
int readClamped(const QSettings& settings, const char* key, int fallback, int min, int max)
{
    if(!settings.contains(key)) {
        return fallback;
    }

    bool ok{false};
    const qlonglong value = settings.value(key).toLongLong(&ok);
    if(!ok) {
        qCWarning(BANDCAMP_SETTINGS) << "value" << key << "is not a number, using" << fallback;
        return fallback;
    }

    const qlonglong clamped = std::clamp<qlonglong>(value, min, max);
    if(clamped != value) {
        qCWarning(BANDCAMP_SETTINGS) << "value" << key << "out of range, clamped to" << clamped;
    }
    return static_cast<int>(clamped);
}
} // namespace

BandcampSettings loadBandcampSettings(const QSettings& settings)
{
    BandcampSettings values;
    values.parallelism = readClamped(settings, ParallelismKey, DefaultParallelism, MinParallelism, MaxParallelism);
    values.itemTimeoutMs
        = readClamped(settings, ItemTimeoutKey, DefaultItemTimeoutMs, MinItemTimeoutMs, MaxItemTimeoutMs);
    values.discographyOrder = static_cast<BandcampDiscographyOrder>(readClamped(
        settings, DiscographyOrderKey, static_cast<int>(BandcampDiscographyOrder::Default),
        static_cast<int>(BandcampDiscographyOrder::Default), static_cast<int>(BandcampDiscographyOrder::Descending)));
    values.prePlaybackRefreshWindowMinutes
        = readClamped(settings, PrePlaybackRefreshWindowKey, DefaultRefreshWindowMinutes, MinRefreshWindowMinutes,
                      MaxRefreshWindowMinutes);
    return values;
}

void saveBandcampSettings(QSettings& settings, const BandcampSettings& values)
{
    settings.setValue(ParallelismKey, values.parallelism);
    settings.setValue(ItemTimeoutKey, values.itemTimeoutMs);
    settings.setValue(DiscographyOrderKey, static_cast<int>(values.discographyOrder));
    settings.setValue(PrePlaybackRefreshWindowKey, values.prePlaybackRefreshWindowMinutes);
}

qint64 refreshWindowMilliseconds(int minutes)
{
    const int clamped = std::clamp(minutes, MinRefreshWindowMinutes, MaxRefreshWindowMinutes);
    if(clamped != minutes) {
        qCWarning(BANDCAMP_SETTINGS) << "pre-playback refresh window" << minutes << "out of range, clamped to"
                                     << clamped;
    }
    const qint64 checkedMinutes = clamped;
    if(checkedMinutes > std::numeric_limits<qint64>::max() / RefreshWindowMsPerMinute) {
        qCWarning(BANDCAMP_SETTINGS) << "pre-playback refresh window conversion overflowed";
        return MaxRefreshWindowMinutes * RefreshWindowMsPerMinute;
    }
    return checkedMinutes * RefreshWindowMsPerMinute;
}

OperationOptions clampOperationOptions(OperationOptions options)
{
    options.parallelism   = std::clamp(options.parallelism, MinParallelism, MaxParallelism);
    options.itemTimeoutMs = std::clamp(options.itemTimeoutMs, MinItemTimeoutMs, MaxItemTimeoutMs);
    const int order       = static_cast<int>(options.discographyOrder);
    if(order < static_cast<int>(BandcampDiscographyOrder::Default)
       || order > static_cast<int>(BandcampDiscographyOrder::Descending)) {
        qCWarning(BANDCAMP_SETTINGS) << "discography order" << order << "is invalid, using Default";
        options.discographyOrder = BandcampDiscographyOrder::Default;
    }
    const qint64 minimumWindowMs = MinRefreshWindowMinutes * RefreshWindowMsPerMinute;
    const qint64 maximumWindowMs = MaxRefreshWindowMinutes * RefreshWindowMsPerMinute;
    const qint64 clampedWindowMs = std::clamp(options.prePlaybackRefreshWindowMs, minimumWindowMs, maximumWindowMs);
    if(clampedWindowMs != options.prePlaybackRefreshWindowMs) {
        qCWarning(BANDCAMP_SETTINGS) << "pre-playback refresh window milliseconds" << options.prePlaybackRefreshWindowMs
                                     << "out of range, clamped to" << clampedWindowMs;
    }
    options.prePlaybackRefreshWindowMs = clampedWindowMs;
    return options;
}
} // namespace Fooyin::Bandcamp
