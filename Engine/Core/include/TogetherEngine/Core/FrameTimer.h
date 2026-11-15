#pragma once

#include <chrono>

namespace together::core {

struct FrameSample {
    double delta_seconds{};
    double frames_per_second{};
};

class FrameTimer final {
public:
    using Clock = std::chrono::steady_clock;

    FrameTimer() : previous_(Clock::now()) {}

    FrameSample tick() noexcept {
        const auto now = Clock::now();
        const std::chrono::duration<double> elapsed = now - previous_;
        previous_ = now;
        const double delta = elapsed.count();
        return {delta, delta > 0.0 ? 1.0 / delta : 0.0};
    }

    void reset() noexcept { previous_ = Clock::now(); }

private:
    Clock::time_point previous_;
};

} // namespace together::core
