// Timer.cpp — Implementation of the high-resolution frame timer.
//
// This file implements the FrameTimer methods using std::chrono to measure the
// elapsed time between engine frames. It provides a simple and accurate way to
// calculate delta time for physics, animation, and other time-dependent engine
// systems.

#include "Timer.h"

FrameTimer::FrameTimer()
	: lastTick(std::chrono::steady_clock::now())
{
}

// FrameTimer::Tick is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
double FrameTimer::Tick()
{
	const auto now = std::chrono::steady_clock::now();
	const std::chrono::duration<double> delta = now - lastTick;
	lastTick = now;
	return delta.count();
}
