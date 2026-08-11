// ProfilerScope.h — Contains declarations for the Profiler Scope file-specific types, helper functions, and module integration code used by runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#pragma once

#include "Profiling/Profiler.h"

namespace TerraEngine::Profiling
{
// ProfilerLiteralLabel is a C++20 structural value. Using a string literal as a
// non-type template parameter gives its bytes static storage without requiring
// a per-frame producer copy. Dynamic labels continue through ProfilerScope.
template<std::size_t Size>
struct ProfilerLiteralLabel
{
	char value[Size]{};

	consteval ProfilerLiteralLabel(const char (&source)[Size])
	{
		for(std::size_t index = 0u; index < Size; ++index)
		{
			value[index] = source[index];
		}
	}
};

// class ProfilerScope groups related state and operations so callers have one documented contract.
class ProfilerScope
{
public:
	ProfilerScope(const char* name, ProfilerModuleId moduleId, const char* taskLabel = nullptr)
	{
		// BeginSample returns false on the common idle path. Remember that
		// result so the destructor can skip EndSample without touching the
		// profiler mutex when no sample was pushed.
		active = Profiler::Instance().BeginSample(name, moduleId, taskLabel);
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

// ProfilerLiteralScope is selected by the normal profiling macros. The label is
// part of the type and therefore remains valid until process shutdown; only a
// genuinely dynamic task label is copied into the bounded producer record.
template<ProfilerLiteralLabel Label>
class ProfilerLiteralScope
{
public:
	explicit ProfilerLiteralScope(
		ProfilerModuleId moduleId,
		const char* taskLabel = nullptr)
		: active(Profiler::Instance().BeginStaticSample(
			Label.value,
			sizeof(Label.value) - 1u,
			moduleId,
			taskLabel))
	{
	}

	~ProfilerLiteralScope()
	{
		if(active)
		{
			Profiler::Instance().EndSample();
		}
	}

	ProfilerLiteralScope(const ProfilerLiteralScope&) = delete;
	ProfilerLiteralScope& operator=(const ProfilerLiteralScope&) = delete;

private:
	bool active = false;
};

// ProfilerEditorWorkScope keeps editor-only UI work out of the game-facing frame
// duration and CPU-engine samples. It belongs at the outer EditorSystem update
// boundary, not around individual windows, so every native, managed, and Vulkan
// editor call follows one exclusion rule without publishing editor diagnostics.
class ProfilerEditorWorkScope
{
public:
	ProfilerEditorWorkScope()
		: active(Profiler::Instance().BeginEditorWork())
	{
	}

	~ProfilerEditorWorkScope()
	{
		Profiler::Instance().EndEditorWork(active);
	}

	ProfilerEditorWorkScope(const ProfilerEditorWorkScope&) = delete;
	ProfilerEditorWorkScope& operator=(const ProfilerEditorWorkScope&) = delete;

private:
	bool active = false;
};
}

#define TERRAENGINE_PROFILE_CONCAT_INNER(a, b) a##b
#define TERRAENGINE_PROFILE_CONCAT(a, b) TERRAENGINE_PROFILE_CONCAT_INNER(a, b)
#define TERRAENGINE_PROFILE_SCOPE(name, moduleId) TerraEngine::Profiling::ProfilerLiteralScope<name> TERRAENGINE_PROFILE_CONCAT(terraengineProfilerScope_, __LINE__)(moduleId)
#define TERRAENGINE_PROFILE_TASK_SCOPE(name, moduleId, taskLabel) TerraEngine::Profiling::ProfilerLiteralScope<name> TERRAENGINE_PROFILE_CONCAT(terraengineProfilerTaskScope_, __LINE__)(moduleId, taskLabel)
#define TERRAENGINE_PROFILE_DYNAMIC_SCOPE(name, moduleId) TerraEngine::Profiling::ProfilerScope TERRAENGINE_PROFILE_CONCAT(terraengineDynamicProfilerScope_, __LINE__)(name, moduleId)
