// Profiler.cpp — Contains the Profiler file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/Profiler.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RenderTypes.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace TerraEngine::Profiling
{
namespace
{
// struct ThreadSampleState groups related state and operations so callers have one documented contract.
struct ThreadSampleState
{
	std::vector<ProfilerSample> openSamples;
	std::unordered_set<std::uint64_t> fullyExcludedSamples;
	std::chrono::steady_clock::time_point editorWorkStart{};
	std::uint32_t editorWorkDepth = 0u;
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

// MaterializeAnimationDiagnosticsMetrics restores the established capture
// schema on an outward-facing copy. Retained dormant frames deliberately keep
// these facts typed so recording them never allocates metric strings or scans
// the module metric vector.
void MaterializeAnimationDiagnosticsMetrics(ProfilerFrameRecord& frame)
{
	if (!frame.animationDiagnostics.has_value())
	{
		return;
	}

	ProfilerModuleSummary* module = FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine);
	if (module == nullptr)
	{
		return;
	}

	const ProfilerAnimationDiagnosticsFacts& facts = *frame.animationDiagnostics;
	AddOrReplaceMetric(*module, "Animation.SchedulerMembershipChecks", static_cast<double>(facts.schedulerMembershipChecks), "count");
	AddOrReplaceMetric(*module, "Animation.Dormant", facts.dormant ? 1.0 : 0.0, "boolean");
	AddOrReplaceMetric(*module, "Animation.BaselineIdentityMatched", facts.baselineIdentityMatched ? 1.0 : 0.0, "boolean");
}

// MaterializeAnimationDiagnosticsMetrics makes a public document self
// contained before it is returned or saved, without changing compact retained
// frames in the profiler session.
void MaterializeAnimationDiagnosticsMetrics(ProfilerCaptureDocument& document)
{
	for (ProfilerFrameRecord& frame : document.frames)
	{
		MaterializeAnimationDiagnosticsMetrics(frame);
	}
}

ProfilerTimingStreamSample MakeTimingSample(
	std::string id,
	std::string label,
	ProfilerTimingStreamScope scope,
	ProfilerTimingStreamSource source,
	ProfilerTimingStreamStatus status,
	double milliseconds,
	std::string reason = {})
{
	ProfilerTimingStreamSample sample;
	sample.id = std::move(id);
	sample.label = std::move(label);
	sample.scope = scope;
	sample.source = source;
	sample.status = status;
	sample.milliseconds = status == ProfilerTimingStreamStatus::Valid ? milliseconds : 0.0;
	sample.statusReason = std::move(reason);
	return sample;
}

TelemetryDuration AvailableTelemetryDuration(std::uint64_t nanoseconds)
{
	return {TelemetryAvailability::Available, nanoseconds, {}};
}

TelemetryDuration UnavailableTelemetryDuration(std::string reason)
{
	return {TelemetryAvailability::Unavailable, 0u, std::move(reason)};
}

std::uint64_t MillisecondsToNanoseconds(double milliseconds)
{
	return static_cast<std::uint64_t>(std::max(0.0, milliseconds) * 1000000.0 + 0.5);
}

// SetAnimationStage converts the existing millisecond diagnostics clock into
// the profiler's integer nanosecond representation. The source snapshot has
// already been validated, so a zero is still an available measurement.
void SetAnimationStage(
	ProfilerAnimationFrameSummary& summary,
	ProfilerAnimationStage stage,
	double milliseconds)
{
	ProfilerAnimationStageTiming& timing = summary.stageTimings[static_cast<std::size_t>(stage)];
	timing.nanoseconds = MillisecondsToNanoseconds(milliseconds);
	timing.available = true;
	timing.sampled = false;
}

// MakeAnimationTelemetry adapts the foundation diagnostics snapshot without
// inventing counters that its producer does not expose. Availability bits name
// the complete counter families present in this older summary contract.
ProfilerAnimationFrameTelemetry MakeAnimationTelemetry(
	const TerraEngine::Animation::AnimationDiagnosticsSnapshot& snapshot)
{
	const TerraEngine::Animation::AnimationDiagnosticsCounters& counters = snapshot.counters;
	ProfilerAnimationFrameTelemetry telemetry;
	ProfilerAnimationFrameSummary& summary = telemetry.summary;
	summary.availableEvidenceMask = kProfilerAnimationVolumeEvidence | kProfilerAnimationTransferEvidence;
	summary.schedulerMembershipChecks = counters.schedulerMembershipChecks;
	summary.activeAnimators = counters.activeAnimators;
	summary.evaluatedAnimators = counters.evaluatedAnimators;
	summary.cpuAnimators = counters.cpuFallbackAnimators;
	summary.gpuVisualAnimators = counters.gpuVisualAnimators;
	summary.hybridAnimators = counters.hybridAuthoritativeAnimators;
	summary.workerJobs = counters.workerJobs;
	summary.sampledJoints = counters.cpuSampledJoints;
	summary.gpuEvaluatedJoints = counters.gpuEvaluatedJoints;
	summary.gpuBatches = counters.gpuBatches;
	summary.gpuTransferBytes = counters.gpuTransferBytes;
	summary.gpuReadbackBytes = counters.gpuReadbackBytes;
	summary.completePoseReadbackBytes = counters.completePoseReadbackBytes;
	summary.boundedMetadataReadbackBytes = counters.boundedMetadataReadbackBytes;
	summary.gpuWaitCount = counters.gpuWaits;
	summary.residencyHits = counters.residencyHits;
	summary.residencyMisses = counters.residencyMisses;
	SetAnimationStage(summary, ProfilerAnimationStage::WorkPlanning, snapshot.timings.preparationMs);
	SetAnimationStage(summary, ProfilerAnimationStage::CpuGraphEvaluation, snapshot.timings.evaluationMs);
	SetAnimationStage(summary, ProfilerAnimationStage::CpuAuthority, snapshot.timings.cpuAuthorityMs);
	SetAnimationStage(summary, ProfilerAnimationStage::GpuPlanning, snapshot.timings.gpuPlanningMs);
	SetAnimationStage(summary, ProfilerAnimationStage::GpuEvaluation, snapshot.timings.gpuExecutionMs);
	SetAnimationStage(summary, ProfilerAnimationStage::Deformation, snapshot.timings.deformationMs);
	SetAnimationStage(summary, ProfilerAnimationStage::ProtectedPublication, snapshot.timings.publicationMs);
	SetAnimationStage(summary, ProfilerAnimationStage::Presentation, snapshot.timings.presentationMs);
	return telemetry;
}

bool ShouldRetainAnimationDetails(
	const ProfilerCaptureSettings& captureSettings,
	std::uint64_t frameIndex)
{
	if (captureSettings.detailLevel == ProfilerCaptureDetailLevel::Detailed)
	{
		return true;
	}
	if (captureSettings.detailLevel == ProfilerCaptureDetailLevel::Sampled)
	{
		return frameIndex % std::max(1U, captureSettings.sampleIntervalFrames) == 0U;
	}
	return false;
}

// AddAnimationTelemetryMetrics exposes the aggregate counters to existing UI
// consumers. The typed record remains authoritative for availability and stage
// attribution; these names are presentation aliases, not acceptance evidence.
void AddAnimationTelemetryMetrics(
	ProfilerModuleSummary& module,
	const ProfilerAnimationFrameSummary& summary)
{
	AddOrReplaceMetric(module, "Animation.AvailableEvidenceMask", static_cast<double>(summary.availableEvidenceMask), "bitmask");
	AddOrReplaceMetric(module, "Animation.ActiveAnimators", static_cast<double>(summary.activeAnimators), "count");
	AddOrReplaceMetric(module, "Animation.EvaluatedAnimators", static_cast<double>(summary.evaluatedAnimators), "count");
	AddOrReplaceMetric(module, "Animation.CpuAnimators", static_cast<double>(summary.cpuAnimators), "count");
	AddOrReplaceMetric(module, "Animation.GpuVisualAnimators", static_cast<double>(summary.gpuVisualAnimators), "count");
	AddOrReplaceMetric(module, "Animation.HybridAuthoritativeAnimators", static_cast<double>(summary.hybridAnimators), "count");
	AddOrReplaceMetric(module, "Animation.WorkerCount", static_cast<double>(summary.workerCount), "count");
	AddOrReplaceMetric(module, "Animation.WorkerJobs", static_cast<double>(summary.workerJobs), "count");
	AddOrReplaceMetric(module, "Animation.WorkerUsefulItems", static_cast<double>(summary.workerUsefulItems), "count");
	AddOrReplaceMetric(module, "Animation.WorkerBusy", static_cast<double>(summary.workerBusyNanoseconds), "nanoseconds");
	AddOrReplaceMetric(module, "Animation.WorkerCapacity", static_cast<double>(summary.workerCapacityNanoseconds), "nanoseconds");
	AddOrReplaceMetric(module, "Animation.SampledJoints", static_cast<double>(summary.sampledJoints), "count");
	AddOrReplaceMetric(module, "Animation.BlendedTracks", static_cast<double>(summary.blendedTracks), "count");
	AddOrReplaceMetric(module, "Animation.EvaluatedGraphNodes", static_cast<double>(summary.evaluatedGraphNodes), "count");
	AddOrReplaceMetric(module, "Animation.MotionMatchingCandidates", static_cast<double>(summary.motionMatchingCandidates), "count");
	AddOrReplaceMetric(module, "Animation.IkChains", static_cast<double>(summary.ikChains), "count");
	AddOrReplaceMetric(module, "Animation.RagdollBodies", static_cast<double>(summary.ragdollBodies), "count");
	AddOrReplaceMetric(module, "Animation.GpuEvaluatedJoints", static_cast<double>(summary.gpuEvaluatedJoints), "count");
	AddOrReplaceMetric(module, "Animation.GpuBatches", static_cast<double>(summary.gpuBatches), "count");
	AddOrReplaceMetric(module, "Animation.FrameArenaBytes", static_cast<double>(summary.frameArenaBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.FrameArenaHighWaterBytes", static_cast<double>(summary.frameArenaHighWaterBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.PoseCacheBytes", static_cast<double>(summary.poseCacheBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.PoseCacheHighWaterBytes", static_cast<double>(summary.poseCacheHighWaterBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.GpuResidentBytes", static_cast<double>(summary.gpuResidentBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.GpuResidentHighWaterBytes", static_cast<double>(summary.gpuResidentHighWaterBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.GpuTransferBytes", static_cast<double>(summary.gpuTransferBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.GpuReadbackBytes", static_cast<double>(summary.gpuReadbackBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.CompletePoseReadbackBytes", static_cast<double>(summary.completePoseReadbackBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.BoundedMetadataReadbackBytes", static_cast<double>(summary.boundedMetadataReadbackBytes), "bytes");
	AddOrReplaceMetric(module, "Animation.GpuWaits", static_cast<double>(summary.gpuWaitCount), "count");
	AddOrReplaceMetric(module, "Animation.SchedulerWaits", static_cast<double>(summary.schedulerWaitCount), "count");
	AddOrReplaceMetric(module, "Animation.ProtectedBoundaryStalls", static_cast<double>(summary.protectedBoundaryStallCount), "count");
	AddOrReplaceMetric(module, "Animation.PoseCacheHits", static_cast<double>(summary.poseCacheHits), "count");
	AddOrReplaceMetric(module, "Animation.PoseCacheMisses", static_cast<double>(summary.poseCacheMisses), "count");
	AddOrReplaceMetric(module, "Animation.ClipCacheHits", static_cast<double>(summary.clipCacheHits), "count");
	AddOrReplaceMetric(module, "Animation.ClipCacheMisses", static_cast<double>(summary.clipCacheMisses), "count");
	AddOrReplaceMetric(module, "Animation.ResidencyHits", static_cast<double>(summary.residencyHits), "count");
	AddOrReplaceMetric(module, "Animation.ResidencyMisses", static_cast<double>(summary.residencyMisses), "count");
}

// Viewport CPU timing must come from profiler scopes instead of renderer frame
// stats. RenderStatsSnapshot::frameTimeMs mirrors the view's frame delta and can
// be much larger than the CPU work done by the viewport submission itself.
bool IsViewportCpuSubmitSample(const ProfilerSample& sample)
{
	if (sample.moduleId != ProfilerModuleId::CpuEngine)
	{
		return false;
	}
	return sample.name == "RenderSystem: Submit Scene View"
		|| sample.name == "RenderSystem: Submit Game View Cameras"
		|| sample.name == "RenderSystem: Submit Blank Game View";
}

bool TryMeasureViewportCpuMilliseconds(const ProfilerFrameRecord& frame, double& viewportCpuMs)
{
	viewportCpuMs = 0.0;
	bool foundViewportSubmit = false;
	for (const ProfilerSample& sample : frame.samples)
	{
		if (!IsViewportCpuSubmitSample(sample))
		{
			continue;
		}
		foundViewportSubmit = true;
		viewportCpuMs += std::max(0.0, sample.durationMs);
	}
	return foundViewportSubmit;
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
	currentExcludedEditorMs = 0.0;
	nextFrameIndex = 0u;
	nextSampleId = 1u;
	queuedTaskCount = 0u;
	startedTaskCount = 0u;
	finishedTaskCount = 0u;
	lastError.clear();
	statusText = "Recording";
	state = ProfilerSessionState::Recording;
	UpdateFastRecordingFlagsLocked();
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
		UpdateFastRecordingFlagsLocked();
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
		UpdateFastRecordingFlagsLocked();
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
	UpdateFastRecordingFlagsLocked();
	ProfilerCaptureDocument snapshot = document;
	MaterializeAnimationDiagnosticsMetrics(snapshot);
	return snapshot;
}

// Profiler::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	document = {};
	currentFrame.reset();
	selectedFrameIndex.reset();
	t_sampleState.openSamples.clear();
	t_sampleState.fullyExcludedSamples.clear();
	t_sampleState.editorWorkDepth = 0u;
	currentExcludedEditorMs = 0.0;
	state = ProfilerSessionState::Idle;
	statusText = "Idle";
	lastError.clear();
	UpdateFastRecordingFlagsLocked();
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
	UpdateFastRecordingFlagsLocked();
}

// Profiler::BeginFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::BeginFrame(bool playModeActive)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording)
	{
		return;
	}
	if (!playModeActive)
	{
		// Record remains armed outside Play Mode, but no partial editor-preview
		// frame or hot-path instrumentation is allowed to survive this boundary.
		currentFrame.reset();
		t_sampleState.openSamples.clear();
		t_sampleState.fullyExcludedSamples.clear();
		t_sampleState.editorWorkDepth = 0u;
		currentExcludedEditorMs = 0.0;
		statusText = "Recording (waiting for Play Mode)";
		UpdateFastRecordingFlagsLocked();
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
	currentExcludedEditorMs = 0.0;
	ProfilerFrameRecord frame;
	frame.frameIndex = nextFrameIndex++;
	frame.frameStartMs = NowMs();
	frame.complete = true;
	frame.moduleSummaries = MakeDefaultModuleSummaries(settings);
	currentFrame = std::move(frame);
	t_sampleState.openSamples.clear();
	t_sampleState.fullyExcludedSamples.clear();
	t_sampleState.editorWorkDepth = 0u;
	statusText = "Recording";
	UpdateFastRecordingFlagsLocked();
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

	const double wallDurationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - currentFrameStart).count();
	const double excludedEditorMs = std::clamp(currentExcludedEditorMs, 0.0, wallDurationMs);
	// Profiler frame statistics intentionally describe the runnable game/engine
	// portion of the frame. Editor UI time is internal subtraction bookkeeping.
	currentFrame->durationMs = std::max(0.0, wallDurationMs - excludedEditorMs);
	AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
		"engine_cpu",
		"Engine CPU",
		ProfilerTimingStreamScope::EngineCpu,
		ProfilerTimingStreamSource::ProfilerClock,
		ProfilerTimingStreamStatus::Valid,
		currentFrame->durationMs));
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (!t_sampleState.openSamples.empty())
	{
		ProfilerSample sample = t_sampleState.openSamples.back();
		t_sampleState.openSamples.pop_back();
		sample.complete = false;
		const bool fullyExcluded = t_sampleState.fullyExcludedSamples.erase(sample.sampleId) != 0u;
		// Sample starts use the same compressed, editor-excluded clock as the
		// frame duration, so incomplete samples cannot extend past the frame.
		sample.durationMs = std::max(0.0, currentFrame->durationMs - sample.startMs);
		if (fullyExcluded)
		{
			continue;
		}
		currentFrame->samples.push_back(std::move(sample));
	}
	double viewportCpuMs = 0.0;
	if (TryMeasureViewportCpuMilliseconds(*currentFrame, viewportCpuMs))
	{
		AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
			"viewport_cpu",
			"Viewport CPU",
			ProfilerTimingStreamScope::ViewportCpu,
			ProfilerTimingStreamSource::ProfilerClock,
			ProfilerTimingStreamStatus::Valid,
			viewportCpuMs));
	}
	// Capture compact immutable physics diagnostics at the ordinary profiler
	// frame boundary. This producer is independent of whether the optional
	// Physics Diagnostics editor window has ever been opened.
	if (const auto physics = TerraEngine::Physics::AcquirePhysicsDiagnosticsSnapshot())
	{
		ProfilerPhysicsFrameCounters counters;
		counters.tick = physics->tick;
		counters.bodyCount = physics->counters.bodyCount;
		counters.contactCount = physics->counters.contactCount;
		counters.queryCount = physics->counters.queryCount;
		counters.eventCount = physics->counters.eventCount;
		counters.rejectedCount = physics->counters.rejectedCount;
		counters.staleCount = physics->counters.staleCount;
		counters.commandQueueDepth = physics->queues.commandDepth;
		counters.shapeCacheBytes = physics->cache.payloadBytes;
		counters.shapeCacheHits = physics->cache.hitCount;
		counters.shapeCacheMisses = physics->cache.missCount;
		counters.snapshotBytes = physics->memory.snapshotBytes;
		counters.snapshotHighWaterBytes = physics->memory.highWaterBytes;
		counters.providerAllocations = physics->memory.outstandingProviderAllocations;
		counters.providerAllocationHighWater = physics->memory.highWaterProviderAllocations;
		counters.prepareMs = physics->timings.prepareMs;
		counters.commitMs = physics->timings.commitMs;
		counters.simulateMs = physics->timings.simulateMs;
		counters.queryMs = physics->timings.queryMs;
		counters.publishMs = physics->timings.publishMs;
		currentFrame->physics = counters;

		const auto appendPhysicsStage = [this](const char* name, double milliseconds, std::uint64_t items) {
			PerformanceStageRecord stage;
			stage.domain = PerformanceStageDomain::Physics;
			stage.stageName = name;
			stage.workloadName = "PhysicsSystem";
			stage.workItemCount = items;
			stage.duration = AvailableTelemetryDuration(MillisecondsToNanoseconds(milliseconds));
			currentFrame->performanceStages.push_back(std::move(stage));
		};
		// These independently named records preserve the physics-pipeline shape
		// even when a backend cannot expose a finer clock for one substage.
		appendPhysicsStage("Physics.Preparation", physics->timings.prepareMs, physics->preparation.estimatedWorkItems);
		appendPhysicsStage("Physics.ProviderRead", physics->timings.simulateMs, physics->extraction.extractedBodies);
		appendPhysicsStage("Physics.StateValidation", physics->timings.commitMs, physics->extraction.convertedBodies);
		appendPhysicsStage("Physics.SnapshotConstruction", physics->timings.queryMs, physics->extraction.changedBodies);
		appendPhysicsStage("Physics.TransactionConstruction", physics->timings.publishMs, physics->publication.currentEntries);
		currentFrame->physicsWorkVolume = PhysicsWorkVolumeRecord{
			physics->tick, physics->extraction.totalBodies, physics->extraction.activeBodies,
			physics->extraction.examinedBodies, physics->extraction.extractedBodies,
			physics->extraction.convertedBodies, physics->extraction.changedBodies,
			std::min(physics->extraction.changedBodies, physics->publication.currentEntries)};
	}

	// Scheduler records are published only after a completed frame-workload pass.
	// The copy keeps profiler storage independent of workload callbacks and live
	// system state while preserving queue/worker/boundary diagnostics.
	currentFrame->workloadExecutions = TerraEngine::Scheduling::LatestFrameWorkloadExecutionRecordsForProfiling();
	for (const WorkloadExecutionRecord& execution : currentFrame->workloadExecutions)
	{
		ProtectedBoundaryRecord boundary;
		boundary.boundaryName = execution.phase.empty() ? execution.workloadName : execution.phase;
		boundary.queueDuration = execution.queueDuration;
		boundary.ownershipWaitDuration = execution.ownershipWaitDuration;
		boundary.executionDuration = execution.runDuration;
		boundary.flushDuration = execution.flushDuration;
		boundary.publicationDuration = execution.publicationDuration;
		if (ValidateProtectedBoundaryRecord(boundary).valid)
		{
			currentFrame->protectedBoundaries.push_back(std::move(boundary));
		}
	}
	if (const auto workerSnapshot = ThreadPool::instance().GetTelemetrySnapshot())
	{
		for (const ThreadPoolWorkerTelemetry& worker : workerSnapshot->workers)
		{
			currentFrame->workerTelemetry.push_back({
				static_cast<std::uint32_t>(worker.workerIndex), worker.latestQueueDepthAtStart,
				worker.dispatchCount, worker.usefulItemCount, AvailableTelemetryDuration(worker.runNanoseconds),
				AvailableTelemetryDuration(worker.waitNanoseconds), AvailableTelemetryDuration(worker.idleNanoseconds)});
		}
	}
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = TerraEngine::Renderer::LatestRenderDiagnostics();
	currentFrame->presentationWaits = renderDiagnostics.presentationWaits;
	if (currentFrame->presentationWaits.empty())
	{
		// Preserve explicit availability when this frame reported no unavoidable
		// presentation wait. An absent vector cannot distinguish "not measured"
		// from a genuine zero-wait observation in later summaries.
		PresentationWaitRecord unavailable;
		unavailable.kind = PresentationWaitKind::UiComposition;
		unavailable.duration.availability = TelemetryAvailability::Unavailable;
		unavailable.duration.unavailableReason = "No presentation-wait timing was published for this frame.";
		unavailable.detail = "renderer presentation telemetry unavailable";
		currentFrame->presentationWaits.push_back(std::move(unavailable));
	}
	currentFrame->responsiveness = FrameResponsivenessRecord{
		currentFrame->frameIndex,
		UnavailableTelemetryDuration("Input timestamp was not supplied by the active platform backend."),
		AvailableTelemetryDuration(MillisecondsToNanoseconds(currentFrame->durationMs)),
		renderDiagnostics.frameId};
	AggregateSamplesForFrame(*currentFrame);
	document.frames.push_back(*currentFrame);
	TrimFrameBuffer(document.frames, settings);
	currentFrame.reset();
	UpdateFastRecordingFlagsLocked();
}

void Profiler::RecordDormantAnimationFacts(std::uint64_t schedulerMembershipChecks)
{
	// Most frames are not being captured. The fast flag keeps a static project
	// out of the profiler mutex entirely during ordinary play.
	if(!frameRecordingActive.load(std::memory_order_acquire))
	{
		return;
	}
	if(schedulerMembershipChecks > 1U)
	{
		// Dormant diagnostics permit at most the single bounded membership check.
		// Reject contradictory caller input instead of retaining misleading proof.
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if(state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	// Dormancy is established by the scheduler's zero membership result. Store
	// only the fixed typed facts; public snapshot APIs materialize the established
	// three metrics later without retaining a dynamic per-frame diagnostics tree.
	currentFrame->animationDiagnostics = ProfilerAnimationDiagnosticsFacts{
		schedulerMembershipChecks,
		true,
		true};
}

void Profiler::RecordAnimationTelemetry(const ProfilerAnimationFrameTelemetry& telemetry)
{
	// Validation can inspect bounded copied detail without taking the profiler
	// mutex. Idle sessions still leave before that work or any allocation.
	if (!frameRecordingActive.load(std::memory_order_acquire)
		|| !ValidateProfilerAnimationFrameTelemetry(telemetry))
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	// Copy the fixed summary on every active captured frame. Per-instance rows
	// obey the capture's existing bounded detail policy and are never retained by
	// Aggregate mode or by an off-cadence Sampled frame.
	auto retained = std::make_shared<ProfilerAnimationFrameTelemetry>();
	retained->summary = telemetry.summary;
	if (ShouldRetainAnimationDetails(settings, currentFrame->frameIndex))
	{
		retained->details = telemetry.details;
	}

	ProfilerModuleSummary* module = FindModule(currentFrame->moduleSummaries, ProfilerModuleId::CpuEngine);
	if (module != nullptr)
	{
		AddAnimationTelemetryMetrics(*module, retained->summary);
	}
	currentFrame->animationDiagnostics = ProfilerAnimationDiagnosticsFacts{
		retained->summary.schedulerMembershipChecks,
		false,
		true};
	currentFrame->animationTelemetry = std::move(retained);
}

void Profiler::RecordAnimationDiagnostics(const TerraEngine::Animation::AnimationDiagnosticsSnapshot& snapshot)
{
	// The atomic flag avoids the profiler mutex and every dynamic metric string
	// allocation when no capture is active, including the zero-animator path.
	if (!frameRecordingActive.load(std::memory_order_acquire)
		|| !TerraEngine::Animation::ValidateAnimationDiagnosticsSnapshot(snapshot))
	{
		return;
	}
	std::optional<ProfilerAnimationFrameTelemetry> activeTelemetry;
	if (!snapshot.dormant)
	{
		activeTelemetry.emplace(MakeAnimationTelemetry(snapshot));
		if (!ValidateProfilerAnimationFrameTelemetry(*activeTelemetry))
		{
			return;
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}
	// A dormant frame retains only the three facts needed to prove that animation
	// performed its bounded membership check. Copying every zero-valued active
	// counter into a long detailed capture would multiply string/vector storage by
	// the frame count and make an otherwise static project pay animation memory.
	const auto& counters = snapshot.counters;
	currentFrame->animationDiagnostics = ProfilerAnimationDiagnosticsFacts{
		counters.schedulerMembershipChecks,
		snapshot.dormant,
		true};
	if (snapshot.dormant)
	{
		return;
	}
	// The diagnostics adapter contains aggregate evidence only. A producer that
	// has opt-in per-instance rows uses RecordAnimationTelemetry at the same
	// protected boundary instead.
	currentFrame->animationTelemetry =
		std::make_shared<const ProfilerAnimationFrameTelemetry>(std::move(*activeTelemetry));

	ProfilerModuleSummary* module = FindModule(currentFrame->moduleSummaries, ProfilerModuleId::CpuEngine);
	if (module == nullptr)
	{
		return;
	}
	AddAnimationTelemetryMetrics(*module, currentFrame->animationTelemetry->summary);

	// Active captures retain the full fixed summary. Detailed per-instance history
	// remains a separate opt-in path and cannot grow this bounded metric set.
	AddOrReplaceMetric(*module, "Animation.SchedulerMembershipChecks", static_cast<double>(counters.schedulerMembershipChecks), "count");
	AddOrReplaceMetric(*module, "Animation.Dormant", 0.0, "boolean");
	AddOrReplaceMetric(*module, "Animation.BaselineIdentityMatched", 1.0, "boolean");
	AddOrReplaceMetric(*module, "Animation.ActiveAnimators", static_cast<double>(counters.activeAnimators), "count");
	AddOrReplaceMetric(*module, "Animation.EvaluatedAnimators", static_cast<double>(counters.evaluatedAnimators), "count");
	AddOrReplaceMetric(*module, "Animation.WorkerJobs", static_cast<double>(counters.workerJobs), "count");
	AddOrReplaceMetric(*module, "Animation.TransientAllocations", static_cast<double>(counters.transientAllocations), "count");
	AddOrReplaceMetric(*module, "Animation.PoseAllocations", static_cast<double>(counters.poseAllocations), "count");
	AddOrReplaceMetric(*module, "Animation.ResidencyRequests", static_cast<double>(counters.residencyRequests), "count");
	AddOrReplaceMetric(*module, "Animation.GpuDispatches", static_cast<double>(counters.gpuDispatches), "count");
	AddOrReplaceMetric(*module, "Animation.GpuUploadBytes", static_cast<double>(counters.gpuUploadBytes), "bytes");
	AddOrReplaceMetric(*module, "Animation.GpuReadbacks", static_cast<double>(counters.gpuReadbacks), "count");
	AddOrReplaceMetric(*module, "Animation.GpuWaits", static_cast<double>(counters.gpuWaits), "count");
	AddOrReplaceMetric(*module, "Animation.GpuVisualAnimators", static_cast<double>(counters.gpuVisualAnimators), "count");
	AddOrReplaceMetric(*module, "Animation.HybridAuthoritativeAnimators", static_cast<double>(counters.hybridAuthoritativeAnimators), "count");
	AddOrReplaceMetric(*module, "Animation.CpuFallbackAnimators", static_cast<double>(counters.cpuFallbackAnimators), "count");
	AddOrReplaceMetric(*module, "Animation.CpuSampledJoints", static_cast<double>(counters.cpuSampledJoints), "count");
	AddOrReplaceMetric(*module, "Animation.GpuEvaluatedJoints", static_cast<double>(counters.gpuEvaluatedJoints), "count");
	AddOrReplaceMetric(*module, "Animation.GpuBatches", static_cast<double>(counters.gpuBatches), "count");
	AddOrReplaceMetric(*module, "Animation.GpuTransferBytes", static_cast<double>(counters.gpuTransferBytes), "bytes");
	AddOrReplaceMetric(*module, "Animation.GpuReadbackBytes", static_cast<double>(counters.gpuReadbackBytes), "bytes");
	AddOrReplaceMetric(*module, "Animation.CompletePoseReadbackBytes", static_cast<double>(counters.completePoseReadbackBytes), "bytes");
	AddOrReplaceMetric(*module, "Animation.BoundedMetadataReadbackBytes", static_cast<double>(counters.boundedMetadataReadbackBytes), "bytes");
	AddOrReplaceMetric(*module, "Animation.ResidencyHits", static_cast<double>(counters.residencyHits), "count");
	AddOrReplaceMetric(*module, "Animation.ResidencyMisses", static_cast<double>(counters.residencyMisses), "count");
	AddOrReplaceMetric(*module, "Animation.Fallbacks", static_cast<double>(counters.fallbackCount), "count");
	AddOrReplaceMetric(*module, "Animation.ModeSwitches", static_cast<double>(counters.modeSwitchCount), "count");
	AddOrReplaceMetric(*module, "Animation.PhysicsHandoffs", static_cast<double>(counters.physicsHandoffs), "count");
	AddOrReplaceMetric(*module, "Animation.ManagedCallbacks", static_cast<double>(counters.managedCallbacks), "count");
	AddOrReplaceMetric(*module, "Animation.DetailedRecords", static_cast<double>(counters.detailedRecords), "count");
	AddOrReplaceMetric(*module, "Animation.Preparation", snapshot.timings.preparationMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.Evaluation", snapshot.timings.evaluationMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.Publication", snapshot.timings.publicationMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.Presentation", snapshot.timings.presentationMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.CpuAuthority", snapshot.timings.cpuAuthorityMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.GpuPlanning", snapshot.timings.gpuPlanningMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.GpuExecution", snapshot.timings.gpuExecutionMs, "milliseconds");
	AddOrReplaceMetric(*module, "Animation.Deformation", snapshot.timings.deformationMs, "milliseconds");
}

void Profiler::RecordSceneViewStages(std::span<const PerformanceStageRecord> stages)
{
	if (!frameRecordingActive.load(std::memory_order_acquire) || stages.empty())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	// The marker pipeline has five stable preparation stages plus an optional
	// hit-test record. Keep a defensive per-frame bound for malformed producers.
	constexpr std::size_t kMaximumSceneViewStageRecordsPerFrame = 16u;
	std::size_t published = 0u;
	for (const PerformanceStageRecord& stage : stages)
	{
		if (published >= kMaximumSceneViewStageRecordsPerFrame)
		{
			break;
		}
		if (stage.domain != PerformanceStageDomain::SceneView || !ValidatePerformanceStageRecord(stage).valid)
		{
			continue;
		}
		currentFrame->performanceStages.push_back(stage);
		++published;
	}
}

bool Profiler::BeginEditorWork()
{
	// The atomic check keeps the normal non-recording editor path lock-free.
	if (!frameRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return false;
	}
	if (t_sampleState.editorWorkDepth++ == 0u)
	{
		t_sampleState.editorWorkStart = std::chrono::steady_clock::now();
		if (!t_sampleState.openSamples.empty()
			&& t_sampleState.openSamples.back().taskLabel == "EditorSystem")
		{
			// The immediate enclosing scheduler scope represents EditorSystem itself.
			// Drop that wrapper completely; its parent remains an engine sample whose
			// duration is reduced by the excluded editor interval below.
			t_sampleState.fullyExcludedSamples.insert(t_sampleState.openSamples.back().sampleId);
		}
	}
	return true;
}

void Profiler::EndEditorWork(bool active)
{
	if (!active)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (t_sampleState.editorWorkDepth == 0u)
	{
		return;
	}
	if (--t_sampleState.editorWorkDepth != 0u)
	{
		return;
	}
	const double durationMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t_sampleState.editorWorkStart).count();
	if (state == ProfilerSessionState::Recording && currentFrame.has_value())
	{
		currentExcludedEditorMs += std::max(0.0, durationMs);
	}
}

// Profiler::BeginSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel)
{
	// Most frames run with profiling idle. Check the cheap flag first so the
	// many RAII scope markers compiled into engine systems avoid a mutex round
	// trip until a real capture is recording.
	if (!markerRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}
	if (t_sampleState.editorWorkDepth != 0u)
	{
		// Editor work is outside the game profiler. Do not let nested ImGui,
		// managed editor, or editor-presentation scopes enter CPU Engine tables.
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value() || !settings.recordMarkers)
	{
		return false;
	}

	ProfilerSample sample;
	sample.sampleId = nextSampleId++;
	sample.parentSampleId = t_sampleState.openSamples.empty() ? 0u : t_sampleState.openSamples.back().sampleId;
	sample.moduleId = moduleId;
	sample.name = name != nullptr ? name : "Unnamed Sample";
	sample.threadLabel = CurrentThreadLabel();
	sample.taskLabel = taskLabel != nullptr ? taskLabel : "";
	const double wallStartMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - currentFrameStart).count();
	// Compress the timeline by editor time already completed this frame. Every
	// sample start and duration then shares the same game-only frame clock.
	sample.startMs = std::max(0.0, wallStartMs - currentExcludedEditorMs);
	sample.depth = static_cast<std::uint32_t>(t_sampleState.openSamples.size());
	t_sampleState.openSamples.push_back(std::move(sample));
	return true;
}

// Profiler::EndSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndSample()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!currentFrame.has_value() || t_sampleState.openSamples.empty())
	{
		return;
	}

	ProfilerSample sample = t_sampleState.openSamples.back();
	t_sampleState.openSamples.pop_back();
	const bool fullyExcluded = t_sampleState.fullyExcludedSamples.erase(sample.sampleId) != 0u;
	// If recording stopped while a worker-owned scope was open, discard that
	// unfinished sample after popping it so thread-local stacks do not leak
	// stale parent ids into a later capture.
	if (state != ProfilerSessionState::Recording)
	{
		return;
	}
	const double wallEndMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - currentFrameStart).count();
	const double gameEndMs = std::max(0.0, wallEndMs - currentExcludedEditorMs);
	sample.durationMs = std::max(0.0, gameEndMs - sample.startMs);
	if (fullyExcluded)
	{
		return;
	}
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
	// RenderStatsSnapshot::frameTimeMs is the input delta copied into a delayed,
	// completed renderer snapshot. It does not measure CPU work in this profiler
	// frame, so exposing it here would create two unrelated "frame time" values.
	// The immediate viewport overlays remain the owner of that renderer diagnostic.
	AddOrReplaceMetric(*engineCpu, "Game draw calls", static_cast<double>(gameStats.drawCalls), "count");
	AddOrReplaceMetric(*engineCpu, "Scene draw calls", static_cast<double>(sceneStats.drawCalls), "count");
	AddOrReplaceMetric(*engineCpu, "Game triangles", static_cast<double>(gameStats.triangleCount), "count");
	AddOrReplaceMetric(*engineCpu, "Scene triangles", static_cast<double>(sceneStats.triangleCount), "count");

	const bool gameGpuValid = gameStats.valid && gameStats.gpuTimestampValid;
	const bool sceneGpuValid = sceneStats.valid && sceneStats.gpuTimestampValid;
	const bool viewportGpuValid = gameGpuValid || sceneGpuValid;
	const double viewportGpuMs =
		(gameGpuValid ? static_cast<double>(gameStats.gpuTimestampMs) : 0.0)
		+ (sceneGpuValid ? static_cast<double>(sceneStats.gpuTimestampMs) : 0.0);
	std::string viewportGpuReason;
	if (!viewportGpuValid)
	{
		const auto appendReason = [&viewportGpuReason](const std::string& reason) {
			if (reason.empty())
			{
				return;
			}
			if (!viewportGpuReason.empty())
			{
				viewportGpuReason += " ";
			}
			viewportGpuReason += reason;
		};
		appendReason(gameStats.valid ? gameStats.gpuTimestampReason : std::string{});
		appendReason(sceneStats.valid ? sceneStats.gpuTimestampReason : std::string{});
		if (viewportGpuReason.empty())
		{
			viewportGpuReason = "No viewport GPU timestamp was published for this frame.";
		}
	}
	AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
		"viewport_gpu",
		"Viewport GPU",
		ProfilerTimingStreamScope::ViewportGpu,
		viewportGpuValid ? ProfilerTimingStreamSource::BackendGpuTimestamp : ProfilerTimingStreamSource::Unavailable,
		viewportGpuValid ? ProfilerTimingStreamStatus::Valid : ProfilerTimingStreamStatus::Unavailable,
		viewportGpuMs,
		viewportGpuReason));
}

// Profiler::RecordTaskQueued is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskQueued()
{
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !settings.taskExecutionEnabled)
	{
		return;
	}
	++queuedTaskCount;
}

// Profiler::RecordTaskStarted is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::RecordTaskStarted(const char* taskLabel)
{
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (state != ProfilerSessionState::Recording || !settings.taskExecutionEnabled)
		{
			return false;
		}
		++startedTaskCount;
	}
	return BeginSample("ThreadPool Task", ProfilerModuleId::TaskExecution, taskLabel);
}

// Profiler::RecordTaskFinished is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::RecordTaskFinished(bool sampleActive)
{
	if (sampleActive)
	{
		EndSample();
	}
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (state != ProfilerSessionState::Recording || !settings.taskExecutionEnabled)
	{
		return;
	}
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
	ProfilerCaptureDocument snapshot;
	{
		// Copy under the mutex, then materialize the optional public metric strings
		// after releasing it so a long dormant capture cannot block recording.
		std::lock_guard<std::mutex> lock(mutex);
		snapshot = document;
		if (currentFrame.has_value())
		{
			snapshot.frames.push_back(*currentFrame);
		}
	}
	MaterializeAnimationDiagnosticsMetrics(snapshot);
	return snapshot;
}

// Profiler::GetFrameListSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<ProfilerFrameListItem> Profiler::GetFrameListSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<ProfilerFrameListItem> frames;
	frames.reserve(document.frames.size() + (currentFrame.has_value() ? 1u : 0u));
	auto appendFrame = [&frames](const ProfilerFrameRecord& frame) {
		ProfilerFrameListItem item;
		item.frameIndex = frame.frameIndex;
		item.durationMs = frame.durationMs;
		item.complete = frame.complete;
		frames.push_back(item);
	};
	for (const ProfilerFrameRecord& frame : document.frames)
	{
		appendFrame(frame);
	}
	if (currentFrame.has_value())
	{
		appendFrame(*currentFrame);
	}
	return frames;
}

// Profiler::GetFramesSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<ProfilerFrameRecord> Profiler::GetFramesSnapshot() const
{
	std::vector<ProfilerFrameRecord> frames;
	{
		// Copy the retained records while synchronized. String materialization below
		// is intentionally outside this lock because it is presentation-only work.
		std::lock_guard<std::mutex> lock(mutex);
		frames = document.frames;
		if (currentFrame.has_value())
		{
			frames.push_back(*currentFrame);
		}
	}
	for (ProfilerFrameRecord& frame : frames)
	{
		MaterializeAnimationDiagnosticsMetrics(frame);
	}
	return frames;
}

// Profiler::GetSelectedFrameSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<ProfilerFrameRecord> Profiler::GetSelectedFrameSnapshot() const
{
	std::optional<ProfilerFrameRecord> snapshot;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!selectedFrameIndex.has_value())
		{
			if (!document.frames.empty())
			{
				snapshot = document.frames.back();
			}
			else if (currentFrame.has_value())
			{
				snapshot = *currentFrame;
			}
		}
		else
		{
			for (const ProfilerFrameRecord& frame : document.frames)
			{
				if (frame.frameIndex == *selectedFrameIndex)
				{
					snapshot = frame;
					break;
				}
			}
			if (!snapshot.has_value() && currentFrame.has_value() && currentFrame->frameIndex == *selectedFrameIndex)
			{
				snapshot = *currentFrame;
			}
		}
	}
	if (snapshot.has_value())
	{
		MaterializeAnimationDiagnosticsMetrics(*snapshot);
	}
	return snapshot;
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

// Profiler::UpdateFastRecordingFlagsLocked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::UpdateFastRecordingFlagsLocked()
{
	// An armed recording outside Play Mode owns no current frame. Keeping all
	// fast flags false there prevents editor-preview instrumentation from paying
	// profiler synchronization costs or leaking counters into the next play frame.
	const bool frameActive = state == ProfilerSessionState::Recording && currentFrame.has_value();
	const bool retainOptionalDetails = frameActive
		&& (settings.detailLevel == ProfilerCaptureDetailLevel::Detailed
			|| (settings.detailLevel == ProfilerCaptureDetailLevel::Sampled
				&& currentFrame->frameIndex % std::max(1u, settings.sampleIntervalFrames) == 0u));
	frameRecordingActive.store(frameActive, std::memory_order_release);
	// Optional scopes span every worker and contend on the profiler's capture
	// mutex. Apply the selected detail level during collection instead of paying
	// that cost on every frame and discarding samples only during serialization.
	markerRecordingActive.store(retainOptionalDetails && settings.recordMarkers, std::memory_order_release);
	taskRecordingActive.store(retainOptionalDetails && settings.taskExecutionEnabled, std::memory_order_release);
}
}
