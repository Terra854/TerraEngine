// Profiler.cpp — Contains the Profiler file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/Profiler.h"

#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace TerraEngine::Profiling
{
namespace
{
// struct ThreadSampleState groups related state and operations so callers have one documented contract.
struct ThreadSampleState
{
	std::vector<ProfilerSample> openSamples;
};

thread_local ThreadSampleState t_sampleState;

// CurrentThreadLabel is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string CurrentThreadLabel()
{
	std::ostringstream stream;
	stream << std::this_thread::get_id();
	return stream.str();
}

// FindModule is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerModuleSummary* FindModule(std::vector<ProfilerModuleSummary>& modules, ProfilerModuleId moduleId)
{
	for (ProfilerModuleSummary& module : modules)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (module.moduleId == moduleId)
		{
			return &module;
		}
	}
	return nullptr;
}

// AddOrReplaceMetric is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AddOrReplaceMetric(ProfilerModuleSummary& module, std::string name, double value, std::string unit)
{
	for (ProfilerMetric& metric : module.metrics)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metric.name == name)
		{
			metric.value = value;
			metric.unit = std::move(unit);
			return;
		}
	}
	module.metrics.push_back({std::move(name), value, std::move(unit)});
}
}

// Profiler::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Profiler& Profiler::Instance()
{
	static Profiler profiler;
	return profiler;
}

// Profiler::StartRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::StartRecording(const ProfilerCaptureMetadata& metadata, const ProfilerCaptureSettings& requestedSettings)
{
	std::lock_guard<std::mutex> lock(mutex);
	settings = requestedSettings;
	settings.captureMode = ProfilerCaptureMode::LiveRolling;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!AnyModuleEnabled(settings))
	{
		settings.cpuEngineEnabled = true;
	}

	document = {};
	document.metadata = metadata;
	document.metadata.settings = settings;
	document.metadata.startedAt = document.metadata.startedAt.empty() ? MakeReadableTimestamp() : document.metadata.startedAt;
	document.frames.clear();
	document.sourcePath.reset();
	currentFrame.reset();
	selectedFrameIndex.reset();
	sessionStart = std::chrono::steady_clock::now();
	currentFrameStart = sessionStart;
	nextFrameIndex = 0u;
	nextSampleId = 1u;
	queuedTaskCount = 0u;
	startedTaskCount = 0u;
	finishedTaskCount = 0u;
	lastError.clear();
	statusText = "Recording";
	state = ProfilerSessionState::Recording;
}

// Profiler::PauseRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::PauseRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state == ProfilerSessionState::Recording)
	{
		state = ProfilerSessionState::Paused;
		statusText = "Paused";
	}
}

// Profiler::ResumeRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::ResumeRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state == ProfilerSessionState::Paused)
	{
		state = ProfilerSessionState::Recording;
		statusText = "Recording";
	}
}

// Profiler::FinishRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureDocument Profiler::FinishRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		currentFrame->complete = false;
		AggregateSamplesForFrame(*currentFrame);
		document.frames.push_back(*currentFrame);
		currentFrame.reset();
	}

	TrimFrameBuffer(document.frames, settings);
	document.metadata.savedAt = MakeReadableTimestamp();
	document.metadata.settings.captureMode = ProfilerCaptureMode::FinalizingToArtifact;
	state = ProfilerSessionState::Finalizing;
	statusText = "Finalizing capture";
	return document;
}

// Profiler::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	document = {};
	currentFrame.reset();
	selectedFrameIndex.reset();
	t_sampleState.openSamples.clear();
	state = ProfilerSessionState::Idle;
	statusText = "Idle";
	lastError.clear();
}

// Profiler::LoadDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::LoadDocument(const ProfilerCaptureDocument& loadedDocument)
{
	std::lock_guard<std::mutex> lock(mutex);
	document = loadedDocument;
	settings = document.metadata.settings;
	settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;
	currentFrame.reset();
	selectedFrameIndex = document.frames.empty() ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{document.frames.front().frameIndex};
	state = ProfilerSessionState::Loaded;
	statusText = "Loaded capture";
	lastError.clear();
}

// Profiler::BeginFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::BeginFrame()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording)
	{
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		currentFrame->complete = false;
		AggregateSamplesForFrame(*currentFrame);
		document.frames.push_back(*currentFrame);
	}

	currentFrameStart = std::chrono::steady_clock::now();
	ProfilerFrameRecord frame;
	frame.frameIndex = nextFrameIndex++;
	frame.frameStartMs = NowMs();
	frame.complete = true;
	frame.moduleSummaries = MakeDefaultModuleSummaries(settings);
	currentFrame = std::move(frame);
	t_sampleState.openSamples.clear();
}

// Profiler::EndFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndFrame()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	currentFrame->durationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentFrameStart).count();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (!t_sampleState.openSamples.empty())
	{
		ProfilerSample sample = t_sampleState.openSamples.back();
		t_sampleState.openSamples.pop_back();
		sample.complete = false;
		sample.durationMs = std::max(0.0, currentFrame->durationMs - sample.startMs);
		currentFrame->samples.push_back(std::move(sample));
	}
	AggregateSamplesForFrame(*currentFrame);
	document.frames.push_back(*currentFrame);
	TrimFrameBuffer(document.frames, settings);
	currentFrame.reset();
}

// Profiler::BeginSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value() || !settings.recordMarkers)
	{
		return;
	}

	ProfilerSample sample;
	sample.sampleId = nextSampleId++;
	sample.parentSampleId = t_sampleState.openSamples.empty() ? 0u : t_sampleState.openSamples.back().sampleId;
	sample.moduleId = moduleId;
	sample.name = name != nullptr ? name : "Unnamed Sample";
	sample.threadLabel = CurrentThreadLabel();
	sample.taskLabel = taskLabel != nullptr ? taskLabel : "";
	sample.startMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentFrameStart).count();
	sample.depth = static_cast<std::uint32_t>(t_sampleState.openSamples.size());
	t_sampleState.openSamples.push_back(std::move(sample));
}

// Profiler::EndSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndSample()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value() || t_sampleState.openSamples.empty())
	{
		return;
	}

	ProfilerSample sample = t_sampleState.openSamples.back();
	t_sampleState.openSamples.pop_back();
	sample.durationMs = std::max(0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentFrameStart).count() - sample.startMs);
	currentFrame->samples.push_back(std::move(sample));
}

// Profiler::RecordRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordRenderStats(const RenderStatsSnapshot& gameStats, const RenderStatsSnapshot& sceneStats)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!currentFrame.has_value() || state != ProfilerSessionState::Recording)
	{
		return;
	}

	ProfilerModuleSummary* engineCpu = FindModule(currentFrame->moduleSummaries, ProfilerModuleId::CpuEngine);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (engineCpu == nullptr)
	{
		return;
	}
	engineCpu->available = gameStats.valid || sceneStats.valid || engineCpu->available;
	AddOrReplaceMetric(*engineCpu, "Game frame time", gameStats.frameTimeMs, "ms");
	AddOrReplaceMetric(*engineCpu, "Scene frame time", sceneStats.frameTimeMs, "ms");
	AddOrReplaceMetric(*engineCpu, "Game draw calls", static_cast<double>(gameStats.drawCalls), "count");
	AddOrReplaceMetric(*engineCpu, "Scene draw calls", static_cast<double>(sceneStats.drawCalls), "count");
	AddOrReplaceMetric(*engineCpu, "Game triangles", static_cast<double>(gameStats.triangleCount), "count");
	AddOrReplaceMetric(*engineCpu, "Scene triangles", static_cast<double>(sceneStats.triangleCount), "count");
}

// Profiler::RecordTaskQueued is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskQueued()
{
	std::lock_guard<std::mutex> lock(mutex);
	++queuedTaskCount;
}

// Profiler::RecordTaskStarted is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskStarted(const char* taskLabel)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		++startedTaskCount;
	}
	BeginSample("ThreadPool Task", ProfilerModuleId::TaskExecution, taskLabel);
}

// Profiler::RecordTaskFinished is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskFinished()
{
	EndSample();
	std::lock_guard<std::mutex> lock(mutex);
	++finishedTaskCount;
}

// Profiler::SelectFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::SelectFrame(std::uint64_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex);
	selectedFrameIndex = frameIndex;
}

// Profiler::GetState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerSessionState Profiler::GetState() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return state;
}

// Profiler::GetSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureSettings Profiler::GetSettings() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return settings;
}

// Profiler::GetDocumentSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureDocument Profiler::GetDocumentSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	ProfilerCaptureDocument snapshot = document;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		snapshot.frames.push_back(*currentFrame);
	}
	return snapshot;
}

// Profiler::GetFramesSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<ProfilerFrameRecord> Profiler::GetFramesSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<ProfilerFrameRecord> frames = document.frames;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		frames.push_back(*currentFrame);
	}
	return frames;
}

// Profiler::GetSelectedFrameSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<ProfilerFrameRecord> Profiler::GetSelectedFrameSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!selectedFrameIndex.has_value())
	{
		if (!document.frames.empty())
		{
			return document.frames.back();
		}
		return currentFrame;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ProfilerFrameRecord& frame : document.frames)
	{
		if (frame.frameIndex == *selectedFrameIndex)
		{
			return frame;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value() && currentFrame->frameIndex == *selectedFrameIndex)
	{
		return currentFrame;
	}
	return std::nullopt;
}

// Profiler::GetSourcePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<std::filesystem::path> Profiler::GetSourcePath() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return document.sourcePath;
}

// Profiler::GetLastError is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Profiler::GetLastError() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return lastError;
}

// Profiler::GetStatusText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Profiler::GetStatusText() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return statusText;
}

// Profiler::IsCapturing is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::IsCapturing() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return state == ProfilerSessionState::Recording || state == ProfilerSessionState::Paused;
}

// Profiler::NowMs is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
double Profiler::NowMs() const
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sessionStart).count();
}

// Profiler::AggregateSamplesForFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::AggregateSamplesForFrame(ProfilerFrameRecord& frame) const
{
	for (ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		module.totalMs = 0.0;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ProfilerSample& sample : frame.samples)
	{
		if (ProfilerModuleSummary* module = FindModule(frame.moduleSummaries, sample.moduleId))
		{
			module->available = true;
			module->totalMs += sample.durationMs;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ProfilerModuleSummary* engineCpu = FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine))
	{
		engineCpu->available = settings.cpuEngineEnabled;
		AddOrReplaceMetric(*engineCpu, "Frame duration", frame.durationMs, "ms");
		AddOrReplaceMetric(*engineCpu, "Engine sample count", static_cast<double>(std::count_if(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& sample) {
			return sample.moduleId == ProfilerModuleId::CpuEngine;
		})), "count");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ProfilerModuleSummary* tasks = FindModule(frame.moduleSummaries, ProfilerModuleId::TaskExecution))
	{
		tasks->available = settings.taskExecutionEnabled;
		AddOrReplaceMetric(*tasks, "Queued tasks", static_cast<double>(queuedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Started tasks", static_cast<double>(startedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Finished tasks", static_cast<double>(finishedTaskCount), "count");
	}

	std::sort(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& left, const ProfilerSample& right) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (left.startMs == right.startMs)
		{
			return left.depth < right.depth;
		}
		return left.startMs < right.startMs;
	});
}
}
