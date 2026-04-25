#pragma once

#include "Profiling/Profiler.h"

namespace TerraEngine::Profiling
{
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
