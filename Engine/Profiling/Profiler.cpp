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
struct ThreadSampleState
{
	std::vector<ProfilerSample> openSamples;
};

thread_local ThreadSampleState t_sampleState;

std::string CurrentThreadLabel()
{
	std::ostringstream stream;
	stream << std::this_thread::get_id();
	return stream.str();
}

ProfilerModuleSummary* FindModule(std::vector<ProfilerModuleSummary>& modules, ProfilerModuleId moduleId)
{
	for (ProfilerModuleSummary& module : modules)
	{
		if (module.moduleId == moduleId)
		{
			return &module;
		}
	}
	return nullptr;
}

void AddOrReplaceMetric(ProfilerModuleSummary& module, std::string name, double value, std::string unit)
{
	for (ProfilerMetric& metric : module.metrics)
	{
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

Profiler& Profiler::Instance()
{
	static Profiler profiler;
	return profiler;
}

void Profiler::StartRecording(const ProfilerCaptureMetadata& metadata, const ProfilerCaptureSettings& requestedSettings)
{
	std::lock_guard<std::mutex> lock(mutex);
	settings = requestedSettings;
	settings.captureMode = ProfilerCaptureMode::LiveRolling;
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

void Profiler::PauseRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == ProfilerSessionState::Recording)
	{
		state = ProfilerSessionState::Paused;
		statusText = "Paused";
	}
}

void Profiler::ResumeRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == ProfilerSessionState::Paused)
	{
		state = ProfilerSessionState::Recording;
		statusText = "Recording";
	}
}

ProfilerCaptureDocument Profiler::FinishRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
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

void Profiler::BeginFrame()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording)
	{
		return;
	}

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

void Profiler::EndFrame()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	currentFrame->durationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentFrameStart).count();
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

void Profiler::BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel)
{
	std::lock_guard<std::mutex> lock(mutex);
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

void Profiler::EndSample()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value() || t_sampleState.openSamples.empty())
	{
		return;
	}

	ProfilerSample sample = t_sampleState.openSamples.back();
	t_sampleState.openSamples.pop_back();
	sample.durationMs = std::max(0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - currentFrameStart).count() - sample.startMs);
	currentFrame->samples.push_back(std::move(sample));
}

void Profiler::RecordRenderStats(const RenderStatsSnapshot& gameStats, const RenderStatsSnapshot& sceneStats)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!currentFrame.has_value() || state != ProfilerSessionState::Recording)
	{
		return;
	}

	ProfilerModuleSummary* engineCpu = FindModule(currentFrame->moduleSummaries, ProfilerModuleId::CpuEngine);
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

void Profiler::RecordTaskQueued()
{
	std::lock_guard<std::mutex> lock(mutex);
	++queuedTaskCount;
}

void Profiler::RecordTaskStarted(const char* taskLabel)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		++startedTaskCount;
	}
	BeginSample("ThreadPool Task", ProfilerModuleId::TaskExecution, taskLabel);
}

void Profiler::RecordTaskFinished()
{
	EndSample();
	std::lock_guard<std::mutex> lock(mutex);
	++finishedTaskCount;
}

void Profiler::SelectFrame(std::uint64_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex);
	selectedFrameIndex = frameIndex;
}

ProfilerSessionState Profiler::GetState() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return state;
}

ProfilerCaptureSettings Profiler::GetSettings() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return settings;
}

ProfilerCaptureDocument Profiler::GetDocumentSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	ProfilerCaptureDocument snapshot = document;
	if (currentFrame.has_value())
	{
		snapshot.frames.push_back(*currentFrame);
	}
	return snapshot;
}

std::vector<ProfilerFrameRecord> Profiler::GetFramesSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<ProfilerFrameRecord> frames = document.frames;
	if (currentFrame.has_value())
	{
		frames.push_back(*currentFrame);
	}
	return frames;
}

std::optional<ProfilerFrameRecord> Profiler::GetSelectedFrameSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!selectedFrameIndex.has_value())
	{
		if (!document.frames.empty())
		{
			return document.frames.back();
		}
		return currentFrame;
	}
	for (const ProfilerFrameRecord& frame : document.frames)
	{
		if (frame.frameIndex == *selectedFrameIndex)
		{
			return frame;
		}
	}
	if (currentFrame.has_value() && currentFrame->frameIndex == *selectedFrameIndex)
	{
		return currentFrame;
	}
	return std::nullopt;
}

std::optional<std::filesystem::path> Profiler::GetSourcePath() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return document.sourcePath;
}

std::string Profiler::GetLastError() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return lastError;
}

std::string Profiler::GetStatusText() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return statusText;
}

bool Profiler::IsCapturing() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return state == ProfilerSessionState::Recording || state == ProfilerSessionState::Paused;
}

double Profiler::NowMs() const
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sessionStart).count();
}

void Profiler::AggregateSamplesForFrame(ProfilerFrameRecord& frame) const
{
	for (ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		module.totalMs = 0.0;
	}

	for (const ProfilerSample& sample : frame.samples)
	{
		if (ProfilerModuleSummary* module = FindModule(frame.moduleSummaries, sample.moduleId))
		{
			module->available = true;
			module->totalMs += sample.durationMs;
		}
	}

	if (ProfilerModuleSummary* engineCpu = FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine))
	{
		engineCpu->available = settings.cpuEngineEnabled;
		AddOrReplaceMetric(*engineCpu, "Frame duration", frame.durationMs, "ms");
		AddOrReplaceMetric(*engineCpu, "Engine sample count", static_cast<double>(std::count_if(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& sample) {
			return sample.moduleId == ProfilerModuleId::CpuEngine;
		})), "count");
	}
	if (ProfilerModuleSummary* tasks = FindModule(frame.moduleSummaries, ProfilerModuleId::TaskExecution))
	{
		tasks->available = settings.taskExecutionEnabled;
		AddOrReplaceMetric(*tasks, "Queued tasks", static_cast<double>(queuedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Started tasks", static_cast<double>(startedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Finished tasks", static_cast<double>(finishedTaskCount), "count");
	}

	std::sort(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& left, const ProfilerSample& right) {
		if (left.startMs == right.startMs)
		{
			return left.depth < right.depth;
		}
		return left.startMs < right.startMs;
	});
}
}
