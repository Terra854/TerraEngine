#pragma once

#include <chrono>

// Lightweight helper for measuring frame-to-frame timing so the main loop can
// track delta time without duplicating chrono logic.
class Timer {
public:
	Timer();

	// Reset the start time to "now". Should be called before entering the run
	// loop so the first delta reflects the first frame.
	void Start();

	// Returns the time in seconds since the last call to Mark (or Start if
	// this is the first measurement) and updates the internal reference point.
	double Mark();

	// Returns the elapsed seconds since the last Start/Mark without resetting
	// the timer, useful for diagnostics.
	double Peek() const;

private:
	using clock = std::chrono::steady_clock;
	clock::time_point lastTick;
};
