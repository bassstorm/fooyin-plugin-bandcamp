#include "bandcampsettings.h"

#include "bandcamplogging.h"

#include <QSettings>

#include <algorithm>

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
    return values;
}

void saveBandcampSettings(QSettings& settings, const BandcampSettings& values)
{
    settings.setValue(ParallelismKey, values.parallelism);
    settings.setValue(ItemTimeoutKey, values.itemTimeoutMs);
}
} // namespace Fooyin::Bandcamp
