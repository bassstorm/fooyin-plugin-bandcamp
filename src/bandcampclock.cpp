#include "bandcampclock.h"

#include <algorithm>

namespace Fooyin::Bandcamp {
std::chrono::milliseconds SystemClock::now() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

namespace {
// Adds a non-negative budget to an instant. A sum that would overflow saturates at the largest instant.
std::chrono::milliseconds addBudget(std::chrono::milliseconds at, std::chrono::milliseconds budget)
{
    if(at >= std::chrono::milliseconds{0} && budget > std::chrono::milliseconds::max() - at) {
        return std::chrono::milliseconds::max();
    }
    return at + budget;
}
} // namespace

Deadline Deadline::after(const BandcampClock& clock, std::chrono::milliseconds budget)
{
    return Deadline{addBudget(clock.now(), std::max(budget, std::chrono::milliseconds{0}))};
}

bool Deadline::expired(const BandcampClock& clock) const
{
    return clock.now() >= m_at;
}

std::chrono::milliseconds Deadline::remaining(const BandcampClock& clock) const
{
    const auto now = clock.now();
    if(now >= m_at) {
        return std::chrono::milliseconds{0};
    }
    // The difference only overflows when the clock is negative and the deadline lies beyond the positive range.
    if(now < std::chrono::milliseconds{0} && m_at > std::chrono::milliseconds::max() + now) {
        return std::chrono::milliseconds::max();
    }
    return m_at - now;
}
} // namespace Fooyin::Bandcamp
