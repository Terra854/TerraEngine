// ProfilerScope.h — Contains declarations for the Profiler Scope file-specific types, helper functions, and module integration code used by runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#pragma once

#include "Profiling/Profiler.h"

namespace TerraEngine::Profiling
{
// class ProfilerScope groups related state and operations so callers have one documented contract.
class ProfilerScope
{
public:
	ProfilerScope(const char* name, ProfilerModuleId moduleId, const char* taskLabel = nullptr)
	{
		Profiler::Instance().BeginSample(name, moduleId, taskLabel);
		active = true;
	}

	~ProfilerScope()
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (active)
		{
			Profiler::Instance().EndSample();
		}
	}

	ProfilerScope(const ProfilerScope&) = delete;
	ProfilerScope& operator=(const ProfilerScope&) = delete;

private:
	bool active = false;
};
}

#define TERRAENGINE_PROFILE_CONCAT_INNER(a, b) a##b
#define TERRAENGINE_PROFILE_CONCAT(a, b) TERRAENGINE_PROFILE_CONCAT_INNER(a, b)
#define TERRAENGINE_PROFILE_SCOPE(name, moduleId) TerraEngine::Profiling::ProfilerScope TERRAENGINE_PROFILE_CONCAT(terraengineProfilerScope_, __LINE__)(name, moduleId)
#define TERRAENGINE_PROFILE_TASK_SCOPE(name, moduleId, taskLabel) TerraEngine::Profiling::ProfilerScope TERRAENGINE_PROFILE_CONCAT(terraengineProfilerTaskScope_, __LINE__)(name, moduleId, taskLabel)
