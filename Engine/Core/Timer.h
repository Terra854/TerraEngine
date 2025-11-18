#pragma once

#include <chrono>

// Utility timer used by the main engine loop to measure delta time between
// frames. The class intentionally stays lightweight and header-only so it can be
// reused by subsystems without additional dependencies.
class FrameTimer {
public:
    FrameTimer();

    // Returns the time in seconds since the previous Tick call. The first call
    // uses the creation time as the starting point.
    double Tick();

private:
    std::chrono::steady_clock::time_point lastTick;
};
