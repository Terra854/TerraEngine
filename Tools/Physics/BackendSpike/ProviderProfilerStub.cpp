// Disposable profiler boundary for the provider-selection fixture.
//
// TerraEngine's ThreadPool normally reports task counters to the complete
// runtime profiler. The standalone backend probe links only the worker pool and
// logger, so these four definitions preserve the same calls without pulling the
// renderer-facing profiler implementation into a dependency-selection tool.

#include "Engine/Profiling/Profiler.h"

namespace TerraEngine::Profiling
{
Profiler& Profiler::Instance()
{
	static Profiler profiler;
	return profiler;
}

void Profiler::RecordTaskQueued()
{
	// The provider fixture records its own submitted-job counter in JSON.
}

bool Profiler::RecordTaskStarted(const char*)
{
	// No profiler sample is opened, so ThreadPool will not try to close one.
	return false;
}

void Profiler::RecordTaskFinished(bool)
{
	// The matching start function never opens a sample in the disposable probe.
}
}
