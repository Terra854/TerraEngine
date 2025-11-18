#include "Timer.h"

FrameTimer::FrameTimer()
    : lastTick(std::chrono::steady_clock::now())
{
}

double FrameTimer::Tick()
{
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> delta = now - lastTick;
    lastTick = now;
    return delta.count();
}
