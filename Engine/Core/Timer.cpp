#include "Timer.h"

Timer::Timer()
{
	Start();
}

void Timer::Start()
{
	lastTick = clock::now();
}

double Timer::Mark()
{
	auto now = clock::now();
	std::chrono::duration<double> elapsed = now - lastTick;
	lastTick = now;
	return elapsed.count();
}

double Timer::Peek() const
{
	std::chrono::duration<double> elapsed = clock::now() - lastTick;
	return elapsed.count();
}
