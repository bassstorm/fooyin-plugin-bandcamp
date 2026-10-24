#pragma once

#include <chrono>

namespace Fooyin::Bandcamp {
// Monotonic time source. Tests inject a manual clock to control deadlines without sleeping.
class BandcampClock
{
public:
    virtual ~BandcampClock() = default;

    [[nodiscard]] virtual std::chrono::milliseconds now() const = 0;
};

class SystemClock final : public BandcampClock
{
public:
    [[nodiscard]] std::chrono::milliseconds now() const override;
};

// A point in time on a BandcampClock. Negative budgets are already expired. Budgets past the largest
// representable instant saturate to it, so such a deadline never expires on its own.
class Deadline
{
public:
    [[nodiscard]] static Deadline after(const BandcampClock& clock, std::chrono::milliseconds budget);

    [[nodiscard]] bool expired(const BandcampClock& clock) const;
    [[nodiscard]] std::chrono::milliseconds remaining(const BandcampClock& clock) const;

private:
    explicit Deadline(std::chrono::milliseconds at)
        : m_at{at}
    { }

    std::chrono::milliseconds m_at{0};
};
} // namespace Fooyin::Bandcamp
