#pragma once

#include "CoreGlobal.h"

#include <QString>

#include <chrono>

namespace hwsim::core {

using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

/// Monotonic clock. Everything that measures an interval (signal generators,
/// timeout faults, response latency statistics) uses this rather than wall
/// clock time so that a system clock adjustment cannot corrupt a running
/// simulation.
[[nodiscard]] HWSIM_CORE_API qint64 monotonicMs() noexcept;
[[nodiscard]] HWSIM_CORE_API qint64 monotonicUs() noexcept;

/// Wall clock, used only for timestamps that a human reads.
[[nodiscard]] HWSIM_CORE_API qint64 wallClockMs() noexcept;
[[nodiscard]] HWSIM_CORE_API QString formatWallClock(qint64 epochMs, bool withDate = false);
[[nodiscard]] HWSIM_CORE_API QString formatDuration(qint64 milliseconds);

/// Indirection point for tests: a virtual clock can be installed so that
/// timeout and signal generation behaviour is reproducible without sleeping.
class HWSIM_CORE_API IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual qint64 nowMs() const noexcept = 0;
};

[[nodiscard]] HWSIM_CORE_API IClock& systemClock();

} // namespace hwsim::core
