// ProfilerCapture.cpp — Contains the Profiler Capture file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/ProfilerCapture.h"
#include "Profiling/ProfilerJournalV1.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
	// Windows.h otherwise injects min/max macros that corrupt the typed
	// std::min/std::max bounds checks used by untrusted capture readers.
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#endif

namespace TerraEngine::Profiling
{
namespace
{
// ModuleIdToStableName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ModuleIdToStableName(ProfilerModuleId moduleId)
{
	switch (moduleId)
	{
	case ProfilerModuleId::CpuEngine:
		return "cpu_engine";
	case ProfilerModuleId::TaskExecution:
		return "task_execution";
	}
	return "cpu_engine";
}

// TryModuleIdFromStableName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<ProfilerModuleId> TryModuleIdFromStableName(const std::string& value)
{
	if (value == "cpu_engine")
	{
		return ProfilerModuleId::CpuEngine;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (value == "task_execution")
	{
		return ProfilerModuleId::TaskExecution;
	}
	return std::nullopt;
}

std::string TimingStatusToStableName(ProfilerTimingStreamStatus status)
{
	switch (status)
	{
	case ProfilerTimingStreamStatus::Valid:
		return "valid";
	case ProfilerTimingStreamStatus::Unavailable:
		return "unavailable";
	case ProfilerTimingStreamStatus::Stale:
		return "stale";
	case ProfilerTimingStreamStatus::Delayed:
		return "delayed";
	case ProfilerTimingStreamStatus::Invalid:
		return "invalid";
	}
	return "unavailable";
}

ProfilerTimingStreamStatus TimingStatusFromStableName(const std::string& value)
{
	if (value == "valid")
	{
		return ProfilerTimingStreamStatus::Valid;
	}
	if (value == "stale")
	{
		return ProfilerTimingStreamStatus::Stale;
	}
	if (value == "delayed")
	{
		return ProfilerTimingStreamStatus::Delayed;
	}
	if (value == "invalid")
	{
		return ProfilerTimingStreamStatus::Invalid;
	}
	return ProfilerTimingStreamStatus::Unavailable;
}

std::string TimingScopeToStableName(ProfilerTimingStreamScope scope)
{
	switch (scope)
	{
	case ProfilerTimingStreamScope::EngineCpu:
		return "engine_cpu";
	case ProfilerTimingStreamScope::ViewportCpu:
		return "viewport_cpu";
	case ProfilerTimingStreamScope::ViewportGpu:
		return "viewport_gpu";
	}
	return "engine_cpu";
}

std::optional<ProfilerTimingStreamScope> TryTimingScopeFromStableName(const std::string& value)
{
	if (value == "engine_cpu")
	{
		return ProfilerTimingStreamScope::EngineCpu;
	}
	if (value == "viewport_cpu")
	{
		return ProfilerTimingStreamScope::ViewportCpu;
	}
	if (value == "viewport_gpu")
	{
		return ProfilerTimingStreamScope::ViewportGpu;
	}
	return std::nullopt;
}

std::string TimingSourceToStableName(ProfilerTimingStreamSource source)
{
	switch (source)
	{
	case ProfilerTimingStreamSource::ProfilerClock:
		return "profiler_clock";
	case ProfilerTimingStreamSource::RenderStats:
		return "render_stats";
	case ProfilerTimingStreamSource::BackendGpuTimestamp:
		return "backend_gpu_timestamp";
	case ProfilerTimingStreamSource::Unavailable:
		return "unavailable";
	}
	return "unavailable";
}

ProfilerTimingStreamSource TimingSourceFromStableName(const std::string& value)
{
	if (value == "profiler_clock")
	{
		return ProfilerTimingStreamSource::ProfilerClock;
	}
	if (value == "render_stats")
	{
		return ProfilerTimingStreamSource::RenderStats;
	}
	if (value == "backend_gpu_timestamp")
	{
		return ProfilerTimingStreamSource::BackendGpuTimestamp;
	}
	return ProfilerTimingStreamSource::Unavailable;
}

double PercentileFromSortedValues(const std::vector<double>& sortedValues, double percentile)
{
	if (sortedValues.empty())
	{
		return 0.0;
	}
	if (sortedValues.size() == 1u)
	{
		return sortedValues.front();
	}
	const double scaledIndex = std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sortedValues.size() - 1u);
	const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(scaledIndex));
	const std::size_t upperIndex = std::min(lowerIndex + 1u, sortedValues.size() - 1u);
	const double blend = scaledIndex - static_cast<double>(lowerIndex);
	return sortedValues[lowerIndex] + (sortedValues[upperIndex] - sortedValues[lowerIndex]) * blend;
}

// ToLocalTime is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::tm ToLocalTime(std::time_t timeValue)
{
	std::tm result{};
#ifdef _WIN32
	localtime_s(&result, &timeValue);
#else
	localtime_r(&timeValue, &result);
#endif
	return result;
}

}

// ToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const char* ToString(ProfilerSessionState state)
{
	switch (state)
	{
	case ProfilerSessionState::Idle:
		return "Idle";
	case ProfilerSessionState::Recording:
		return "Recording";
	case ProfilerSessionState::Paused:
		return "Paused";
	case ProfilerSessionState::Finalizing:
		return "Finalizing";
	case ProfilerSessionState::Saving:
		return "Saving";
	case ProfilerSessionState::OpeningSavedCapture:
		return "Opening Saved Capture";
	case ProfilerSessionState::Loaded:
		return "Loaded";
	case ProfilerSessionState::Error:
		return "Error";
	}
	return "Unknown";
}

// ToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const char* ToString(ProfilerModuleId moduleId)
{
	switch (moduleId)
	{
	case ProfilerModuleId::CpuEngine:
		return "CPU/Engine";
	case ProfilerModuleId::TaskExecution:
		return "Task Execution";
	}
	return "CPU/Engine";
}

const char* ToString(ProfilerTimingStreamStatus status)
{
	switch (status)
	{
	case ProfilerTimingStreamStatus::Valid:
		return "valid";
	case ProfilerTimingStreamStatus::Unavailable:
		return "unavailable";
	case ProfilerTimingStreamStatus::Stale:
		return "stale";
	case ProfilerTimingStreamStatus::Delayed:
		return "delayed";
	case ProfilerTimingStreamStatus::Invalid:
		return "invalid";
	}
	return "unavailable";
}

const char* ToString(ProfilerTimingStreamScope scope)
{
	switch (scope)
	{
	case ProfilerTimingStreamScope::EngineCpu:
		return "Engine CPU";
	case ProfilerTimingStreamScope::ViewportCpu:
		return "Viewport CPU";
	case ProfilerTimingStreamScope::ViewportGpu:
		return "Viewport GPU";
	}
	return "Engine CPU";
}

const char* ToString(ProfilerTimingStreamSource source)
{
	switch (source)
	{
	case ProfilerTimingStreamSource::ProfilerClock:
		return "profiler clock";
	case ProfilerTimingStreamSource::RenderStats:
		return "render stats";
	case ProfilerTimingStreamSource::BackendGpuTimestamp:
		return "GPU timestamp";
	case ProfilerTimingStreamSource::Unavailable:
		return "unavailable";
	}
	return "unavailable";
}

const char* ToString(ProfilerCaptureDetailLevel level)
{
	switch (level)
	{
	case ProfilerCaptureDetailLevel::Detailed:
		return "detailed";
	case ProfilerCaptureDetailLevel::Sampled:
		return "sampled";
	case ProfilerCaptureDetailLevel::Aggregate:
		return "aggregate";
	}
	return "detailed";
}

const char* ToString(ProfilerAnimationStage stage)
{
	switch (stage)
	{
	case ProfilerAnimationStage::SchedulerMembership:
		return "Animation.SchedulerMembership";
	case ProfilerAnimationStage::WorkPlanning:
		return "Animation.WorkPlanning";
	case ProfilerAnimationStage::CpuGraphEvaluation:
		return "Animation.CpuGraphEvaluation";
	case ProfilerAnimationStage::MotionMatching:
		return "Animation.MotionMatching";
	case ProfilerAnimationStage::IkSolve:
		return "Animation.IkSolve";
	case ProfilerAnimationStage::RagdollHandoff:
		return "Animation.RagdollHandoff";
	case ProfilerAnimationStage::CpuAuthority:
		return "Animation.CpuAuthority";
	case ProfilerAnimationStage::GpuPlanning:
		return "Animation.GpuPlanning";
	case ProfilerAnimationStage::GpuEvaluation:
		return "Animation.GpuEvaluation";
	case ProfilerAnimationStage::Deformation:
		return "Animation.Deformation";
	case ProfilerAnimationStage::ProtectedPublication:
		return "Animation.ProtectedPublication";
	case ProfilerAnimationStage::Presentation:
		return "Animation.Presentation";
	case ProfilerAnimationStage::Count:
		break;
	}
	return "Animation.Unknown";
}

bool ValidateProfilerAnimationFrameTelemetry(const ProfilerAnimationFrameTelemetry& telemetry) noexcept
{
	const ProfilerAnimationFrameSummary& summary = telemetry.summary;
	if (summary.schemaVersion != 1U
		|| (summary.availableEvidenceMask & ~kProfilerAnimationAllEvidence) != 0U
		|| summary.activeAnimators == 0U
		|| summary.evaluatedAnimators > summary.activeAnimators
		|| telemetry.details.size() > kMaximumProfilerAnimationDetailRecordsPerFrame
		|| telemetry.details.size() > summary.activeAnimators)
	{
		return false;
	}

	// Mode buckets may omit suspended work but must never claim more instances
	// than the active set. Subtraction avoids an overflow in the three-way sum.
	if (summary.cpuAnimators > summary.activeAnimators
		|| summary.gpuVisualAnimators > summary.activeAnimators - summary.cpuAnimators
		|| summary.hybridAnimators > summary.activeAnimators - summary.cpuAnimators - summary.gpuVisualAnimators)
	{
		return false;
	}
	if (((summary.availableEvidenceMask & kProfilerAnimationWorkerEvidence) != 0U
			&& summary.workerJobs != 0U && summary.workerCount == 0U)
		|| summary.workerBusyNanoseconds > summary.workerCapacityNanoseconds
		|| summary.frameArenaBytes > summary.frameArenaHighWaterBytes
		|| summary.poseCacheBytes > summary.poseCacheHighWaterBytes
		|| summary.gpuResidentBytes > summary.gpuResidentHighWaterBytes
		|| summary.completePoseReadbackBytes > summary.gpuReadbackBytes
		|| summary.boundedMetadataReadbackBytes > summary.gpuReadbackBytes)
	{
		return false;
	}

	for (const ProfilerAnimationStageTiming& timing : summary.stageTimings)
	{
		// A number without a clock provenance is invalid, and a sampled record must
		// also be available. A measured stage may legitimately take zero time.
		if ((!timing.available && timing.nanoseconds != 0U) || (timing.sampled && !timing.available))
		{
			return false;
		}
	}
	for (std::size_t index = 0U; index < telemetry.details.size(); ++index)
	{
		if (telemetry.details[index].stableInstanceId == 0U)
		{
			return false;
		}
		for (std::size_t prior = 0U; prior < index; ++prior)
		{
			if (telemetry.details[prior].stableInstanceId == telemetry.details[index].stableInstanceId)
			{
				return false;
			}
		}
	}
	return true;
}

bool ValidateProfilerAnimationOptimizationEvidence(
	const ProfilerAnimationOptimizationEvidence& evidence) noexcept
{
	const bool fallbackReasonsEmpty = std::all_of(
		evidence.fallbackReasonCounts.begin(), evidence.fallbackReasonCounts.end(),
		[](std::uint64_t value) { return value == 0U; });
	const bool timingsEmpty = std::all_of(
		evidence.timingNanoseconds.begin(), evidence.timingNanoseconds.end(),
		[](std::uint64_t value) { return value == 0U; });
	if (!evidence.valid)
	{
		// Invalid means unavailable, never an older frame's counters with a false
		// label. This rule lets the editor distinguish unavailable from measured zero.
		return evidence.frameIndex == 0U && evidence.candidateGeneration == 0U
			&& evidence.acceptedGeneration == 0U && evidence.rejectedGeneration == 0U
			&& evidence.topologyGeneration == 0U && evidence.presentationGeneration == 0U
			&& evidence.rendererGeneration == 0U && evidence.sceneGeneration == 0U
			&& evidence.activeAnimators == 0U && evidence.suspendedAnimators == 0U
			&& evidence.compactEligibleAnimators == 0U && evidence.compactProcessedAnimators == 0U
			&& evidence.completeCapturedAnimators == 0U && evidence.completeProcessedAnimators == 0U
			&& evidence.compactToCompleteTransitions == 0U && evidence.completeToCompactTransitions == 0U
			&& evidence.retainedTopologyHits == 0U && evidence.retainedTopologyMisses == 0U
			&& evidence.compactPlanGroups == 0U && evidence.compactDispatches == 0U
			&& evidence.rejectedCandidateCount == 0U && fallbackReasonsEmpty && timingsEmpty;
	}

	// Compact and complete processing are disjoint. Every processed record must
	// originate in its corresponding accepted range, and dispatched groups cannot
	// exceed the number of compact animators they describe.
	const bool compactGenerationsComplete = evidence.compactProcessedAnimators == 0U
		|| (evidence.topologyGeneration != 0U && evidence.presentationGeneration != 0U
			&& evidence.rendererGeneration != 0U);
	return evidence.frameIndex != 0U && evidence.candidateGeneration != 0U
		&& evidence.acceptedGeneration != 0U && compactGenerationsComplete
		&& evidence.sceneGeneration != 0U && evidence.activeAnimators != 0U
		&& evidence.suspendedAnimators <= evidence.activeAnimators
		&& evidence.compactEligibleAnimators <= evidence.activeAnimators
		&& evidence.compactProcessedAnimators <= evidence.compactEligibleAnimators
		&& evidence.completeProcessedAnimators <= evidence.completeCapturedAnimators
		&& evidence.completeCapturedAnimators <= evidence.activeAnimators
		&& evidence.compactProcessedAnimators
			<= evidence.activeAnimators - evidence.completeProcessedAnimators
		&& evidence.compactPlanGroups <= evidence.compactProcessedAnimators
		&& evidence.compactDispatches <= evidence.compactProcessedAnimators;
}

bool ValidateProfilerRenderOptimizationEvidence(const ProfilerRenderOptimizationEvidence& evidence) noexcept
{
	for (const ProfilerRenderOptimizationViewSummary& view : evidence.views)
	{
		if (!view.valid)
		{
			// An unavailable view carries no stale optimization result. Its view type
			// may still identify which slot was unavailable to the editor.
			if (view.frameGeneration != 0U || view.topologyGeneration != 0U
				|| view.viewGeneration != 0U || view.rendererGeneration != 0U
				|| view.topologyBuildNanoseconds != 0U
				|| view.dynamicMaterializationNanoseconds != 0U
				|| view.instanceStreamUploadNanoseconds != 0U
				|| view.groupedCommandRecordingNanoseconds != 0U
				|| view.visibleInstanceCount != 0U || view.groupedInstanceCount != 0U
				|| view.fallbackInstanceCount != 0U || view.groupedSubmissionCount != 0U
				|| view.fallbackSubmissionCount != 0U || view.avoidedSubmissionCount != 0U
				|| view.instanceStreamBytes != 0U || view.instanceStreamCapacityBytes != 0U
				|| view.topologyReuseHits != 0U || view.topologyInvalidations != 0U
				|| view.viewEvidenceReuseHits != 0U || view.viewEvidenceInvalidations != 0U
				|| view.rejectedIncompleteKeyCount != 0U || view.rejectedUnsupportedVariantCount != 0U
				|| view.rejectedOutOfRangeCount != 0U || view.rejectedStaleGenerationCount != 0U
				|| view.fallbackAllocationFailureCount != 0U || view.fallbackRecoveryCount != 0U)
			{
				return false;
			}
			continue;
		}

		// Conservation and capacity checks make the typed evidence useful for
		// diagnosis without trusting independently incremented presentation rows.
		if (view.frameGeneration == 0U || view.topologyGeneration == 0U
			|| view.viewGeneration == 0U || view.rendererGeneration == 0U
			|| view.groupedInstanceCount > view.visibleInstanceCount
			|| view.fallbackInstanceCount != view.visibleInstanceCount - view.groupedInstanceCount
			|| view.groupedSubmissionCount > view.groupedInstanceCount
			|| view.fallbackSubmissionCount > view.fallbackInstanceCount
			|| view.avoidedSubmissionCount > view.visibleInstanceCount
			|| view.instanceStreamBytes > view.instanceStreamCapacityBytes)
		{
			return false;
		}
	}

	if (evidence.views[0].valid && evidence.views[1].valid
		&& evidence.views[0].viewType == evidence.views[1].viewType)
	{
		return false;
	}
	return true;
}

bool ValidateProfilerFramePublicationEvidence(const ProfilerFramePublicationEvidence& evidence) noexcept
{
	if (evidence.schemaVersion != ProfilerFramePublicationEvidence::kSchemaVersion
		|| evidence.viewCount > ProfilerFramePublicationEvidence::kMaximumViews)
	{
		return false;
	}
	for (std::size_t index = 0; index < evidence.viewCount; ++index)
	{
		if (evidence.views[index].valid && evidence.views[index].frameGeneration != evidence.frameGeneration)
		{
			return false;
		}
	}
	return true;
}

bool ValidateProfilerDisjointTimingBuckets(const std::array<std::uint64_t, 8U>& buckets, const std::uint64_t frameNanoseconds, const std::uint64_t toleranceNanoseconds) noexcept
{
	std::uint64_t total = 0U;
	for (const std::uint64_t bucket : buckets)
	{
		if (bucket > frameNanoseconds || total > frameNanoseconds - bucket)
		{
			return false;
		}
		total += bucket;
	}
	return total <= frameNanoseconds && frameNanoseconds - total <= toleranceNanoseconds;
}

// MakeTimestampForFileName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string MakeTimestampForFileName()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
	const std::tm localTime = ToLocalTime(timeValue);
	std::ostringstream stream;
	stream << std::put_time(&localTime, "%Y%m%d-%H%M%S");
	return stream.str();
}

// MakeReadableTimestamp is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string MakeReadableTimestamp()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
	const std::tm localTime = ToLocalTime(timeValue);
	std::ostringstream stream;
	stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
	return stream.str();
}

// AnyModuleEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AnyModuleEnabled(const ProfilerCaptureSettings& settings)
{
	return settings.cpuEngineEnabled || settings.taskExecutionEnabled;
}

// MakeDefaultModuleSummaries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<ProfilerModuleSummary> MakeDefaultModuleSummaries(const ProfilerCaptureSettings& settings)
{
	std::vector<ProfilerModuleSummary> modules;
	auto addModule = [&modules](ProfilerModuleId moduleId, bool enabled) {
		ProfilerModuleSummary summary{};
		summary.moduleId = moduleId;
		summary.available = enabled;
		modules.push_back(std::move(summary));
	};
	addModule(ProfilerModuleId::CpuEngine, settings.cpuEngineEnabled);
	addModule(ProfilerModuleId::TaskExecution, settings.taskExecutionEnabled);
	return modules;
}

// TrimFrameBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void TrimFrameBuffer(std::vector<ProfilerFrameRecord>& frames, const ProfilerCaptureSettings& settings)
{
	// A zero frame limit is intentionally unlimited. Explicit non-zero limits
	// retain the rolling-buffer behavior used by bounded tools and tests.
	const std::size_t maxFrames = settings.maxFrames;
	if (maxFrames > 0u && frames.size() > maxFrames)
	{
		frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(frames.size() - maxFrames));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (settings.maxDurationSeconds > 0.0 && !frames.empty())
	{
		const double newestStart = frames.back().frameStartMs;
		const double cutoff = newestStart - settings.maxDurationSeconds * 1000.0;
		const auto firstKept = std::find_if(frames.begin(), frames.end(), [cutoff](const ProfilerFrameRecord& frame) {
			return frame.frameStartMs >= cutoff;
		});
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (firstKept != frames.begin())
		{
			frames.erase(frames.begin(), firstKept);
		}
	}
}

// EstimateProfilerFrameOwnedBytes counts the value-owned payload retained by a
// sealed frame.  It deliberately counts logical string/vector contents rather
// than allocator capacity, because capacity is implementation-specific and
// would make queue-pressure decisions differ between otherwise identical runs.
std::uint64_t EstimateProfilerFrameOwnedBytes(const ProfilerFrameRecord& frame) noexcept
{
	auto stringBytes = [](const std::string& value) -> std::uint64_t {
		return static_cast<std::uint64_t>(value.size());
	};
	std::uint64_t bytes = sizeof(ProfilerFrameRecord);
	for (const ProfilerTimingStreamSample& sample : frame.timingStreams)
	{
		bytes += sizeof(sample) + stringBytes(sample.id) + stringBytes(sample.label)
			+ stringBytes(sample.statusReason);
	}
	for (const ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		bytes += sizeof(module);
		for (const ProfilerMetric& metric : module.metrics)
		{
			bytes += sizeof(metric) + stringBytes(metric.name) + stringBytes(metric.unit);
		}
	}
	for (const ProfilerSample& sample : frame.samples)
	{
		bytes += sizeof(sample) + stringBytes(sample.name) + stringBytes(sample.threadLabel)
			+ stringBytes(sample.taskLabel);
	}
	for (const PerformanceStageRecord& stage : frame.performanceStages)
	{
		bytes += sizeof(stage) + stringBytes(stage.stageName) + stringBytes(stage.workloadName)
			+ stringBytes(stage.duration.unavailableReason);
	}
	for (const ProtectedBoundaryRecord& boundary : frame.protectedBoundaries)
	{
		bytes += sizeof(boundary) + stringBytes(boundary.boundaryName)
			+ stringBytes(boundary.queueDuration.unavailableReason)
			+ stringBytes(boundary.ownershipWaitDuration.unavailableReason)
			+ stringBytes(boundary.executionDuration.unavailableReason)
			+ stringBytes(boundary.flushDuration.unavailableReason)
			+ stringBytes(boundary.publicationDuration.unavailableReason);
	}
	for (const WorkerTelemetryRecord& worker : frame.workerTelemetry)
	{
		bytes += sizeof(worker) + stringBytes(worker.runDuration.unavailableReason)
			+ stringBytes(worker.waitDuration.unavailableReason)
			+ stringBytes(worker.idleDuration.unavailableReason);
	}
	for (const WorkloadExecutionRecord& workload : frame.workloadExecutions)
	{
		bytes += sizeof(workload) + stringBytes(workload.frameId)
			+ stringBytes(workload.descriptorId) + stringBytes(workload.ownerSystem)
			+ stringBytes(workload.phase) + stringBytes(workload.workloadName)
			+ stringBytes(workload.activationPredicateKey)
			+ stringBytes(workload.queueDuration.unavailableReason)
			+ stringBytes(workload.runDuration.unavailableReason)
			+ stringBytes(workload.coordinatorWaitDuration.unavailableReason)
			+ stringBytes(workload.ownershipWaitDuration.unavailableReason)
			+ stringBytes(workload.flushDuration.unavailableReason)
			+ stringBytes(workload.publicationDuration.unavailableReason);
	}
	for (const PresentationWaitRecord& wait : frame.presentationWaits)
	{
		bytes += sizeof(wait) + stringBytes(wait.detail)
			+ stringBytes(wait.duration.unavailableReason);
	}
	if (frame.responsiveness.has_value())
	{
		bytes += sizeof(FrameResponsivenessRecord)
			+ stringBytes(frame.responsiveness->inputToPresentationDuration.unavailableReason)
			+ stringBytes(frame.responsiveness->frameDuration.unavailableReason);
	}
	if (frame.animationTelemetry)
	{
		bytes += sizeof(ProfilerAnimationFrameTelemetry)
			+ static_cast<std::uint64_t>(frame.animationTelemetry->details.size()
				* sizeof(ProfilerAnimationDetailRecord));
	}
	return bytes;
}

bool ValidateProfilerSealedFrame(const ProfilerSealedFrame& sealed, std::string& outError) noexcept
{
	outError.clear();
	if (!std::isfinite(sealed.frame.durationMs) || sealed.frame.durationMs < 0.0)
	{
		outError = "Profiler sealed frame has an invalid duration.";
		return false;
	}
	if (sealed.ownedByteCount < EstimateProfilerFrameOwnedBytes(sealed.frame))
	{
		outError = "Profiler sealed frame under-reports its owned bytes.";
		return false;
	}
	std::uint64_t previousLastFrame = 0u;
	bool havePrevious = false;
	for (const ProfilerCaptureLossRange& loss : sealed.losses)
	{
		if (loss.lossClass == ProfilerCaptureLossClass::None
			|| loss.reason == ProfilerCaptureLossReason::None
			|| loss.firstFrameIndex > loss.lastFrameIndex
			|| loss.firstFrameIndex != sealed.frame.frameIndex
			|| loss.lastFrameIndex != sealed.frame.frameIndex)
		{
			outError = "Profiler sealed frame has an invalid loss range.";
			return false;
		}
		if (havePrevious && loss.firstFrameIndex <= previousLastFrame)
		{
			outError = "Profiler sealed frame loss ranges are not strictly ordered.";
			return false;
		}
		previousLastFrame = loss.lastFrameIndex;
		havePrevious = true;
	}
	return true;
}

std::span<const ProfilerCaptureLossClass> GetProfilerOptionalDetailSheddingOrder() noexcept
{
	// Keep this typed array beside validation rather than as a local profiler
	// lambda. Tests and future producers therefore share one stable policy and
	// cannot accidentally reorder loss classes while refactoring the queue.
	static constexpr std::array<ProfilerCaptureLossClass, 4u> order = {
		ProfilerCaptureLossClass::AnimationDetails,
		ProfilerCaptureLossClass::SampleLeaves,
		ProfilerCaptureLossClass::TaskContext,
		ProfilerCaptureLossClass::WorkloadRanges};
	return order;
}

void CoalesceProfilerCaptureLossRange(
	std::vector<ProfilerCaptureLossRange>& ranges,
	const ProfilerCaptureLossRange& next)
{
	if (next.lossClass == ProfilerCaptureLossClass::None
		|| next.reason == ProfilerCaptureLossReason::None
		|| next.firstFrameIndex > next.lastFrameIndex)
	{
		return;
	}
	if (!ranges.empty())
	{
		ProfilerCaptureLossRange& previous = ranges.back();
		if (previous.lossClass == next.lossClass && previous.reason == next.reason
			&& previous.lastFrameIndex != std::numeric_limits<std::uint64_t>::max()
			&& previous.lastFrameIndex + 1u == next.firstFrameIndex)
		{
			previous.lastFrameIndex = next.lastFrameIndex;
			previous.discardedRecordCount += next.discardedRecordCount;
			previous.discardedByteCount += next.discardedByteCount;
			return;
		}
	}
	ranges.push_back(next);
}

void AddOrReplaceTimingStreamSample(ProfilerFrameRecord& frame, ProfilerTimingStreamSample sample)
{
	if (sample.id.empty())
	{
		sample.id = TimingScopeToStableName(sample.scope);
	}
	if (sample.label.empty())
	{
		sample.label = ToString(sample.scope);
	}

	const auto found = std::find_if(frame.timingStreams.begin(), frame.timingStreams.end(), [&sample](const ProfilerTimingStreamSample& existing) {
		return existing.id == sample.id;
	});
	if (found == frame.timingStreams.end())
	{
		frame.timingStreams.push_back(std::move(sample));
		return;
	}
	*found = std::move(sample);
}

std::vector<ProfilerTimingStreamAggregate> BuildTimingStreamAggregates(const std::vector<ProfilerFrameRecord>& frames, const std::optional<ProfilerFrameRecord>& selectedFrame)
{
	std::vector<ProfilerTimingStreamAggregate> aggregates;
	const auto findAggregate = [&aggregates](const ProfilerTimingStreamSample& sample) -> ProfilerTimingStreamAggregate& {
		const auto found = std::find_if(aggregates.begin(), aggregates.end(), [&sample](const ProfilerTimingStreamAggregate& aggregate) {
			return aggregate.id == sample.id;
		});
		if (found != aggregates.end())
		{
			return *found;
		}

		ProfilerTimingStreamAggregate aggregate;
		aggregate.id = sample.id;
		aggregate.label = sample.label.empty() ? ToString(sample.scope) : sample.label;
		aggregate.scope = sample.scope;
		aggregate.source = sample.source;
		aggregates.push_back(std::move(aggregate));
		return aggregates.back();
	};

	for (const ProfilerFrameRecord& frame : frames)
	{
		for (const ProfilerTimingStreamSample& sample : frame.timingStreams)
		{
			findAggregate(sample);
		}
	}

	for (ProfilerTimingStreamAggregate& aggregate : aggregates)
	{
		std::vector<double> values;
		values.reserve(frames.size());
		for (const ProfilerFrameRecord& frame : frames)
		{
			const auto sample = std::find_if(frame.timingStreams.begin(), frame.timingStreams.end(), [&aggregate](const ProfilerTimingStreamSample& candidate) {
				return candidate.id == aggregate.id;
			});
			if (sample == frame.timingStreams.end())
			{
				++aggregate.missingCount;
				continue;
			}
			if (sample->status == ProfilerTimingStreamStatus::Valid && std::isfinite(sample->milliseconds))
			{
				values.push_back(sample->milliseconds);
			}
			else
			{
				++aggregate.missingCount;
			}
		}

		aggregate.sampleCount = values.size();
		if (!values.empty())
		{
			const double sum = std::accumulate(values.begin(), values.end(), 0.0);
			aggregate.meanMs = sum / static_cast<double>(values.size());
			double varianceSum = 0.0;
			for (double value : values)
			{
				const double delta = value - aggregate.meanMs;
				varianceSum += delta * delta;
			}
			aggregate.stdDevMs = std::sqrt(varianceSum / static_cast<double>(values.size()));
			std::sort(values.begin(), values.end());
			aggregate.minMs = values.front();
			aggregate.maxMs = values.back();
			aggregate.medianMs = PercentileFromSortedValues(values, 0.50);
			aggregate.p90Ms = PercentileFromSortedValues(values, 0.90);
			aggregate.p95Ms = PercentileFromSortedValues(values, 0.95);
			aggregate.p99Ms = PercentileFromSortedValues(values, 0.99);
		}

		if (selectedFrame.has_value())
		{
			const auto selectedSample = std::find_if(selectedFrame->timingStreams.begin(), selectedFrame->timingStreams.end(), [&aggregate](const ProfilerTimingStreamSample& sample) {
				return sample.id == aggregate.id;
			});
			if (selectedSample != selectedFrame->timingStreams.end())
			{
				aggregate.selectedStatus = selectedSample->status;
				aggregate.selectedMilliseconds = selectedSample->milliseconds;
				aggregate.selectedStatusReason = selectedSample->statusReason;
			}
		}
	}

	std::sort(aggregates.begin(), aggregates.end(), [](const ProfilerTimingStreamAggregate& left, const ProfilerTimingStreamAggregate& right) {
		return static_cast<std::uint8_t>(left.scope) < static_cast<std::uint8_t>(right.scope);
	});
	return aggregates;
}

// SaveCaptureDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool SaveCaptureDocument(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError)
{
	// Documents use the same append-only journal as live recording.  One writer
	// prevents an offline export from silently reintroducing a second format.
	for (const ProfilerFrameRecord& frame : document.frames)
	{
		if (frame.animationOptimization.has_value()
			&& !ValidateProfilerAnimationOptimizationEvidence(*frame.animationOptimization))
		{
			outError = "Profiler capture contains invalid animation optimization evidence.";
			return false;
		}
		if (frame.renderOptimization.has_value()
			&& !ValidateProfilerRenderOptimizationEvidence(*frame.renderOptimization))
		{
			outError = "Profiler capture contains invalid renderer optimization evidence.";
			return false;
		}
		if (frame.framePublication.has_value()
			&& !ValidateProfilerFramePublicationEvidence(*frame.framePublication))
		{
			outError = "Profiler capture contains invalid frame-publication evidence.";
			return false;
		}
		if (frame.animationTelemetry
			&& (!ValidateProfilerAnimationFrameTelemetry(*frame.animationTelemetry)
			|| !frame.animationDiagnostics.has_value() || frame.animationDiagnostics->dormant
			|| frame.animationDiagnostics->schedulerMembershipChecks
				!= frame.animationTelemetry->summary.schedulerMembershipChecks))
		{
			outError = "Profiler capture contains invalid or conflicting animation telemetry.";
			return false;
		}
	}
	ProfilerJournalV1Writer writer;
	ProfilerCaptureMetadata metadata = document.metadata;
	metadata.settings.captureMode = ProfilerCaptureMode::LiveRolling;
	if (writer.Open(path, metadata, outError) != ProfilerJournalV1ResultCode::Ok)
	{
		return false;
	}
	const std::size_t framesPerSegment = std::max<std::size_t>(1u, metadata.settings.maximumChunkFrames);
	for (std::size_t first = 0u; first < document.frames.size(); first += framesPerSegment)
	{
		const std::size_t count = std::min(framesPerSegment, document.frames.size() - first);
		std::vector<ProfilerSealedFrame> sealed;
		sealed.reserve(count);
		for (std::size_t offset = 0u; offset < count; ++offset)
		{
			ProfilerSealedFrame item;
			item.frame = document.frames[first + offset];
			item.ownedByteCount = EstimateProfilerFrameOwnedBytes(item.frame);
			sealed.push_back(std::move(item));
		}
		if (writer.AppendCommittedSegment(sealed, outError) != ProfilerJournalV1ResultCode::Ok)
		{
			return false;
		}
	}
	return writer.Finalize(document.complete, outError) == ProfilerJournalV1ResultCode::Ok;
	}

// LoadCaptureDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool LoadCaptureDocument(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError)
{
	ProfilerJournalV1OpenResult journalResult;
	const ProfilerJournalV1ResultCode journalCode = OpenProfilerJournalV1(
		path, outDocument, journalResult, outError);
	const bool accepted = journalCode == ProfilerJournalV1ResultCode::Ok
		|| journalCode == ProfilerJournalV1ResultCode::IncompleteTail;
	if (!accepted && outError.empty())
	{
		// Preserve a useful typed failure when a malformed fresh journal reaches a
		// reader path that intentionally has no detailed parser diagnostic. Empty
		// errors turn focused capture regressions into an unactionable failure.
		outError = "Profiler journal V1 reader rejected the capture (code "
			+ std::to_string(static_cast<unsigned int>(journalCode)) + ").";
	}
	return accepted;
}

// TryReadCaptureArtifactMetadata reads the bounded journal catalog without materializing detailed frames.
bool TryReadCaptureArtifactMetadata(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
{
	ProfilerJournalV1OpenResult journalResult;
	std::string journalError;
	const ProfilerJournalV1ResultCode journalCode = ReadProfilerJournalV1Index(path, journalResult, journalError);
	outArtifact = {};
	outArtifact.absolutePath = path;
	outArtifact.fileName = path.filename().string();
	if (journalCode != ProfilerJournalV1ResultCode::Ok
		&& journalCode != ProfilerJournalV1ResultCode::IncompleteTail)
	{
		outArtifact.supported = false;
		outArtifact.status = journalError;
		return false;
	}
	outArtifact.metadata = std::move(journalResult.metadata);
	outArtifact.formatIdentifier = std::string(kProfilerJournalV1FormatIdentifier);
	outArtifact.formatVersion = kProfilerJournalV1SchemaVersion;
	outArtifact.frameCount = journalResult.summary.frameCount;
	outArtifact.committedFrameCount = journalResult.summary.frameCount;
	outArtifact.complete = journalResult.complete;
	outArtifact.recoveredIncompleteTail = journalResult.recoveredIncompleteTail;
	outArtifact.supported = true;
	outArtifact.status = journalResult.recoveredIncompleteTail ? "Recovered incomplete tail" : "Ready (journal index)";
	outArtifact.moduleIds = {ProfilerModuleId::CpuEngine, ProfilerModuleId::TaskExecution};
	return true;
	}

// TryReadCaptureArtifact is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
{
	ProfilerCaptureDocument document;
	std::string error;
	outArtifact = {};
	outArtifact.absolutePath = path;
	outArtifact.fileName = path.filename().string();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!LoadCaptureDocument(path, document, error))
	{
		outArtifact.supported = false;
		outArtifact.status = error;
		return false;
	}

	outArtifact.metadata = document.metadata;
	outArtifact.frameCount = static_cast<std::uint64_t>(document.frames.size());
	outArtifact.formatIdentifier = document.formatIdentifier;
	outArtifact.formatVersion = document.formatVersion;
	outArtifact.committedFrameCount = document.committedFrameCount == 0u
		? static_cast<std::uint64_t>(document.frames.size())
		: document.committedFrameCount;
	outArtifact.complete = document.complete;
	outArtifact.recoveredIncompleteTail = document.recoveredIncompleteTail;
	outArtifact.supported = true;
	outArtifact.status = "Ready";
	outArtifact.moduleIds = {
		ProfilerModuleId::CpuEngine,
		ProfilerModuleId::TaskExecution
	};
	return true;
}

namespace
{
// The journal frame codec uses bounded local tables, so a decoded frame never
// depends on a global table accumulated across a long recording session.
constexpr std::uint32_t kJournalMaximumStringCount = 1u << 20u;
constexpr std::uint32_t kJournalMaximumFramesPerSegment = 4096u;

// IsPublicAnimationDiagnosticsMetric identifies the three legacy-facing
// profiler metrics represented by the compact typed journal animation field.
// Keeping the names here makes the writer and reader agree on exactly which
// records must not be stored twice for every dormant frame.
bool IsPublicAnimationDiagnosticsMetric(std::string_view name)
{
	return name == "Animation.SchedulerMembershipChecks"
		|| name == "Animation.Dormant"
		|| name == "Animation.BaselineIdentityMatched";
}

// AddOrReplaceMetric restores the public profiler surface after the reader has
// decoded compact animation facts. The file format retains typed scalars while
// callers continue to receive the established string-named metrics.
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

// AddAnimationTelemetryMetrics exposes the fixed typed animation summary as
// the stable metric names used by existing capture readers. It runs only after
// a frame was copied for presentation or reopened from the journal; recording
// itself retains the compact numeric record without per-frame string churn.
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

// MaterializeProfilerAnimationPresentationMetrics removes any legacy duplicate
// records and recreates public aliases from the authoritative compact facts.
// It intentionally runs after a copy leaves the protected recording boundary.
void MaterializeProfilerAnimationPresentationMetricsImpl(ProfilerFrameRecord& frame)
{
	if (!frame.animationDiagnostics.has_value() && !frame.animationTelemetry)
	{
		return;
	}

	for (ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		if (module.moduleId != ProfilerModuleId::CpuEngine)
		{
			continue;
		}

		if (frame.animationDiagnostics.has_value())
		{
			std::erase_if(module.metrics, [](const ProfilerMetric& metric) {
				return IsPublicAnimationDiagnosticsMetric(metric.name);
			});
			const ProfilerAnimationDiagnosticsFacts& facts = *frame.animationDiagnostics;
			AddOrReplaceMetric(module, "Animation.SchedulerMembershipChecks", static_cast<double>(facts.schedulerMembershipChecks), "count");
			AddOrReplaceMetric(module, "Animation.Dormant", facts.dormant ? 1.0 : 0.0, "boolean");
			AddOrReplaceMetric(module, "Animation.BaselineIdentityMatched", facts.baselineIdentityMatched ? 1.0 : 0.0, "boolean");
		}
		if (frame.animationTelemetry)
		{
			AddAnimationTelemetryMetrics(module, frame.animationTelemetry->summary);
		}
		return;
	}
}

std::uint64_t ToNanoseconds(double milliseconds)
{
	return static_cast<std::uint64_t>(std::max(0.0, milliseconds) * 1000000.0 + 0.5);
}

double ToMilliseconds(std::uint64_t nanoseconds)
{
	return static_cast<double>(nanoseconds) / 1000000.0;
}

std::uint32_t Fnv1a32(const std::string& value)
{
	std::uint32_t hash = 2166136261u;
	for (const unsigned char byte : value)
	{
		hash ^= byte;
		hash *= 16777619u;
	}
	return hash;
}

class JournalStringTable final
{
public:
	JournalStringTable()
	{
		Intern({});
	}

	std::uint32_t Intern(const std::string& value)
	{
		const auto existing = indices.find(value);
		if (existing != indices.end())
		{
			return existing->second;
		}
		const std::uint32_t index = static_cast<std::uint32_t>(values.size());
		indices.emplace(value, index);
		values.push_back(value);
		return index;
	}

	[[nodiscard]] const std::vector<std::string>& Values() const { return values; }

private:
	std::unordered_map<std::string, std::uint32_t> indices;
	std::vector<std::string> values;
};

void CollectDuration(JournalStringTable& table, const TelemetryDuration& duration)
{
	table.Intern(duration.unavailableReason);
}

bool KeepOptionalJournalData(
	const ProfilerFrameRecord& frame,
	const ProfilerCaptureSettings& settings);

void CollectFrameStrings(
	JournalStringTable& table,
	const ProfilerFrameRecord& frame,
	const ProfilerCaptureSettings& settings)
{
	const bool includeOptional = KeepOptionalJournalData(frame, settings);
	for (const ProfilerTimingStreamSample& stream : frame.timingStreams)
	{
		table.Intern(stream.id); table.Intern(stream.label); table.Intern(stream.statusReason);
	}
	if (includeOptional)
	{
		for (const ProfilerModuleSummary& module : frame.moduleSummaries)
		{
			for (const ProfilerMetric& metric : module.metrics)
			{
				// Typed facts replace these public metric records in a journal payload, so
				// their strings do not belong in the artifact string table either.
				if (frame.animationDiagnostics.has_value() && IsPublicAnimationDiagnosticsMetric(metric.name))
				{
					continue;
				}
				table.Intern(metric.name); table.Intern(metric.unit);
			}
		}
	}
	for (const ProfilerSample& sample : frame.samples)
	{
		table.Intern(sample.name); table.Intern(sample.threadLabel); table.Intern(sample.taskLabel);
	}
	for (const PerformanceStageRecord& stage : frame.performanceStages)
	{
		table.Intern(stage.stageName); table.Intern(stage.workloadName); CollectDuration(table, stage.duration);
	}
	for (const ProtectedBoundaryRecord& boundary : frame.protectedBoundaries)
	{
		table.Intern(boundary.boundaryName);
		CollectDuration(table, boundary.queueDuration); CollectDuration(table, boundary.ownershipWaitDuration);
		CollectDuration(table, boundary.executionDuration); CollectDuration(table, boundary.flushDuration);
		CollectDuration(table, boundary.publicationDuration);
	}
	for (const WorkerTelemetryRecord& worker : frame.workerTelemetry)
	{
		CollectDuration(table, worker.runDuration); CollectDuration(table, worker.waitDuration); CollectDuration(table, worker.idleDuration);
	}
	for (const PresentationWaitRecord& wait : frame.presentationWaits)
	{
		table.Intern(wait.detail); CollectDuration(table, wait.duration);
	}
	if (frame.responsiveness.has_value())
	{
		CollectDuration(table, frame.responsiveness->inputToPresentationDuration);
		CollectDuration(table, frame.responsiveness->frameDuration);
	}
}

nlohmann::json PackDuration(const TelemetryDuration& duration, JournalStringTable& table)
{
	return nlohmann::json::array({
		static_cast<std::uint8_t>(duration.availability), duration.nanoseconds, table.Intern(duration.unavailableReason)});
}

bool UnpackDuration(const nlohmann::json& json, const std::vector<std::string>& strings, TelemetryDuration& out, std::string& error)
{
	if (!json.is_array() || json.size() != 3u || !json[0].is_number_unsigned() || !json[1].is_number_unsigned() || !json[2].is_number_unsigned())
	{
		error = "Profiler journal V1 frame contains a malformed duration.";
		return false;
	}
	const std::size_t reasonIndex = json[2].get<std::size_t>();
	if (reasonIndex >= strings.size())
	{
		error = "Profiler journal V1 frame duration references an out-of-range string.";
		return false;
	}
	out.availability = static_cast<TelemetryAvailability>(json[0].get<std::uint8_t>());
	out.nanoseconds = json[1].get<std::uint64_t>();
	out.unavailableReason = strings[reasonIndex];
	if (!ValidateTelemetryDuration(out).valid)
	{
		error = "Profiler journal V1 frame contains an invalid duration.";
		return false;
	}
	return true;
}

const std::string* StringAt(const std::vector<std::string>& strings, const nlohmann::json& index, std::string& error)
{
	if (!index.is_number_unsigned())
	{
		error = "Profiler journal V1 frame contains a non-numeric string index.";
		return nullptr;
	}
	const std::size_t value = index.get<std::size_t>();
	if (value >= strings.size())
	{
		error = "Profiler journal V1 frame contains an out-of-range string index.";
		return nullptr;
	}
	return &strings[value];
}

bool KeepOptionalJournalData(const ProfilerFrameRecord& frame, const ProfilerCaptureSettings& settings)
{
	if (settings.detailLevel == ProfilerCaptureDetailLevel::Detailed)
	{
		return true;
	}
	if (settings.detailLevel == ProfilerCaptureDetailLevel::Sampled)
	{
		return frame.frameIndex % std::max(1u, settings.sampleIntervalFrames) == 0u;
	}
	return false;
}

nlohmann::json PackAnimationTelemetryForJournal(
	const ProfilerAnimationFrameTelemetry& telemetry,
	bool includeDetails)
{
	const ProfilerAnimationFrameSummary& summary = telemetry.summary;
	nlohmann::json stages = nlohmann::json::array();
	for (const ProfilerAnimationStageTiming& timing : summary.stageTimings)
	{
		stages.push_back(nlohmann::json::array({timing.nanoseconds, timing.available, timing.sampled}));
	}
	nlohmann::json result = {
		{"v", summary.schemaVersion},
		{"e", summary.availableEvidenceMask},
		{"c", nlohmann::json::array({
			summary.schedulerMembershipChecks, summary.activeAnimators, summary.evaluatedAnimators,
			summary.cpuAnimators, summary.gpuVisualAnimators, summary.hybridAnimators,
			summary.workerCount, summary.workerJobs, summary.workerUsefulItems,
			summary.workerBusyNanoseconds, summary.workerCapacityNanoseconds,
			summary.sampledJoints, summary.blendedTracks, summary.evaluatedGraphNodes,
			summary.motionMatchingCandidates, summary.ikChains, summary.ragdollBodies,
			summary.gpuEvaluatedJoints, summary.gpuBatches,
			summary.frameArenaBytes, summary.frameArenaHighWaterBytes,
			summary.poseCacheBytes, summary.poseCacheHighWaterBytes,
			summary.gpuResidentBytes, summary.gpuResidentHighWaterBytes,
			summary.gpuTransferBytes, summary.gpuReadbackBytes,
			summary.completePoseReadbackBytes, summary.boundedMetadataReadbackBytes,
			summary.gpuWaitCount, summary.schedulerWaitCount, summary.protectedBoundaryStallCount,
			summary.poseCacheHits, summary.poseCacheMisses,
			summary.clipCacheHits, summary.clipCacheMisses,
			summary.residencyHits, summary.residencyMisses})},
		{"s", std::move(stages)}
	};
	if (includeDetails && !telemetry.details.empty())
	{
		result["d"] = nlohmann::json::array();
		for (const ProfilerAnimationDetailRecord& detail : telemetry.details)
		{
			result["d"].push_back(nlohmann::json::array({
				detail.stableInstanceId, detail.executionMode, detail.batchOrdinal,
				detail.sampledJoints, detail.evaluatedGraphNodes, detail.transferBytes,
				detail.readbackBytes, detail.evaluationNanoseconds, detail.poseCacheReused}));
		}
	}
	return result;
}

bool UnpackAnimationTelemetryForJournal(
	const nlohmann::json& json,
	ProfilerAnimationFrameTelemetry& out,
	std::string& error)
{
	if (!json.is_object() || !json.contains("v") || !json.at("v").is_number_unsigned()
		|| !json.contains("e") || !json.at("e").is_number_unsigned()
		|| !json.contains("c") || !json.at("c").is_array() || json.at("c").size() != 38U
		|| !json.contains("s") || !json.at("s").is_array()
		|| json.at("s").size() != kProfilerAnimationStageCount)
	{
		error = "Profiler journal V1 frame has malformed animation telemetry.";
		return false;
	}
	for (const nlohmann::json& value : json.at("c"))
	{
		if (!value.is_number_unsigned())
		{
			error = "Profiler journal V1 frame animation telemetry has a non-unsigned counter.";
			return false;
		}
	}

	ProfilerAnimationFrameSummary& summary = out.summary;
	summary.schemaVersion = json.at("v").get<std::uint32_t>();
	summary.availableEvidenceMask = json.at("e").get<std::uint32_t>();
	const nlohmann::json& counters = json.at("c");
	std::size_t index = 0U;
	auto next = [&counters, &index]() { return counters.at(index++).get<std::uint64_t>(); };
	summary.schedulerMembershipChecks = next(); summary.activeAnimators = next(); summary.evaluatedAnimators = next();
	summary.cpuAnimators = next(); summary.gpuVisualAnimators = next(); summary.hybridAnimators = next();
	summary.workerCount = next(); summary.workerJobs = next(); summary.workerUsefulItems = next();
	summary.workerBusyNanoseconds = next(); summary.workerCapacityNanoseconds = next();
	summary.sampledJoints = next(); summary.blendedTracks = next(); summary.evaluatedGraphNodes = next();
	summary.motionMatchingCandidates = next(); summary.ikChains = next(); summary.ragdollBodies = next();
	summary.gpuEvaluatedJoints = next(); summary.gpuBatches = next();
	summary.frameArenaBytes = next(); summary.frameArenaHighWaterBytes = next();
	summary.poseCacheBytes = next(); summary.poseCacheHighWaterBytes = next();
	summary.gpuResidentBytes = next(); summary.gpuResidentHighWaterBytes = next();
	summary.gpuTransferBytes = next(); summary.gpuReadbackBytes = next();
	summary.completePoseReadbackBytes = next(); summary.boundedMetadataReadbackBytes = next();
	summary.gpuWaitCount = next(); summary.schedulerWaitCount = next(); summary.protectedBoundaryStallCount = next();
	summary.poseCacheHits = next(); summary.poseCacheMisses = next();
	summary.clipCacheHits = next(); summary.clipCacheMisses = next();
	summary.residencyHits = next(); summary.residencyMisses = next();

	for (std::size_t stageIndex = 0U; stageIndex < kProfilerAnimationStageCount; ++stageIndex)
	{
		const nlohmann::json& stage = json.at("s").at(stageIndex);
		if (!stage.is_array() || stage.size() != 3U || !stage.at(0).is_number_unsigned()
			|| !stage.at(1).is_boolean() || !stage.at(2).is_boolean())
		{
			error = "Profiler journal V1 frame has malformed animation stage timing.";
			return false;
		}
		summary.stageTimings[stageIndex] = ProfilerAnimationStageTiming{
			stage.at(0).get<std::uint64_t>(), stage.at(1).get<bool>(), stage.at(2).get<bool>()};
	}

	if (const auto details = json.find("d"); details != json.end())
	{
		if (!details->is_array() || details->size() > kMaximumProfilerAnimationDetailRecordsPerFrame)
		{
			error = "Profiler journal V1 frame animation detail exceeds its bounded schema.";
			return false;
		}
		out.details.reserve(details->size());
		for (const nlohmann::json& value : *details)
		{
			if (!value.is_array() || value.size() != 9U
				|| !value.at(0).is_number_unsigned() || !value.at(1).is_number_unsigned()
				|| !value.at(2).is_number_unsigned() || !value.at(3).is_number_unsigned()
				|| !value.at(4).is_number_unsigned() || !value.at(5).is_number_unsigned()
				|| !value.at(6).is_number_unsigned() || !value.at(7).is_number_unsigned()
				|| !value.at(8).is_boolean())
			{
				error = "Profiler journal V1 frame has malformed animation detail.";
				return false;
			}
			out.details.push_back(ProfilerAnimationDetailRecord{
				value.at(0).get<std::uint64_t>(), value.at(1).get<std::uint32_t>(),
				value.at(2).get<std::uint32_t>(), value.at(3).get<std::uint64_t>(),
				value.at(4).get<std::uint64_t>(), value.at(5).get<std::uint64_t>(),
				value.at(6).get<std::uint64_t>(), value.at(7).get<std::uint64_t>(),
				value.at(8).get<bool>()});
		}
	}
	if (!ValidateProfilerAnimationFrameTelemetry(out))
	{
		error = "Profiler journal V1 frame contains invalid animation telemetry.";
		return false;
	}
	return true;
}

nlohmann::json PackAnimationOptimizationForJournal(
	const ProfilerAnimationOptimizationEvidence& evidence)
{
	nlohmann::json result = nlohmann::json::array({
		evidence.frameIndex, evidence.candidateGeneration, evidence.acceptedGeneration,
		evidence.rejectedGeneration, evidence.topologyGeneration, evidence.presentationGeneration,
		evidence.rendererGeneration, evidence.sceneGeneration, evidence.activeAnimators,
		evidence.suspendedAnimators, evidence.compactEligibleAnimators,
		evidence.compactProcessedAnimators, evidence.completeCapturedAnimators,
		evidence.completeProcessedAnimators, evidence.compactToCompleteTransitions,
		evidence.completeToCompactTransitions, evidence.retainedTopologyHits,
		evidence.retainedTopologyMisses, evidence.compactPlanGroups,
		evidence.compactDispatches, evidence.rejectedCandidateCount});
	for (std::uint64_t count : evidence.fallbackReasonCounts)
	{
		result.push_back(count);
	}
	for (std::uint64_t timing : evidence.timingNanoseconds)
	{
		result.push_back(timing);
	}
	result.push_back(evidence.valid);
	return result;
}

bool UnpackAnimationOptimizationForJournal(
	const nlohmann::json& json,
	ProfilerAnimationOptimizationEvidence& out,
	std::string& error)
{
	constexpr std::size_t kScalarCount = 21U;
	constexpr std::size_t kValueCount = kScalarCount
		+ ProfilerAnimationOptimizationEvidence::kFallbackReasonCount
		+ ProfilerAnimationOptimizationEvidence::kTimingCount + 1U;
	if (!json.is_array() || json.size() != kValueCount || !json.back().is_boolean())
	{
		error = "Profiler journal V1 frame has malformed animation optimization evidence.";
		return false;
	}
	for (std::size_t index = 0U; index + 1U < json.size(); ++index)
	{
		if (!json.at(index).is_number_unsigned())
		{
			error = "Profiler journal V1 frame animation optimization evidence has a non-unsigned value.";
			return false;
		}
	}

	std::size_t index = 0U;
	auto next = [&json, &index]() { return json.at(index++).get<std::uint64_t>(); };
	out.frameIndex = next();
	out.candidateGeneration = next();
	out.acceptedGeneration = next();
	out.rejectedGeneration = next();
	out.topologyGeneration = next();
	out.presentationGeneration = next();
	out.rendererGeneration = next();
	out.sceneGeneration = next();
	out.activeAnimators = next();
	out.suspendedAnimators = next();
	out.compactEligibleAnimators = next();
	out.compactProcessedAnimators = next();
	out.completeCapturedAnimators = next();
	out.completeProcessedAnimators = next();
	out.compactToCompleteTransitions = next();
	out.completeToCompactTransitions = next();
	out.retainedTopologyHits = next();
	out.retainedTopologyMisses = next();
	out.compactPlanGroups = next();
	out.compactDispatches = next();
	out.rejectedCandidateCount = next();
	for (std::uint64_t& count : out.fallbackReasonCounts)
	{
		count = next();
	}
	for (std::uint64_t& timing : out.timingNanoseconds)
	{
		timing = next();
	}
	out.valid = json.at(index).get<bool>();
	if (!ValidateProfilerAnimationOptimizationEvidence(out))
	{
		error = "Profiler journal V1 frame contains inconsistent animation optimization evidence.";
		return false;
	}
	return true;
}

nlohmann::json PackRenderOptimizationForJournal(const ProfilerRenderOptimizationEvidence& evidence)
{
	nlohmann::json result = nlohmann::json::array();
	for (const ProfilerRenderOptimizationViewSummary& view : evidence.views)
	{
		result.push_back(nlohmann::json::array({
			view.viewType, view.frameGeneration, view.topologyGeneration, view.viewGeneration,
			view.rendererGeneration, view.topologyBuildNanoseconds,
			view.dynamicMaterializationNanoseconds, view.instanceStreamUploadNanoseconds,
			view.groupedCommandRecordingNanoseconds, view.visibleInstanceCount, view.groupedInstanceCount,
			view.fallbackInstanceCount, view.groupedSubmissionCount, view.fallbackSubmissionCount,
			view.avoidedSubmissionCount, view.instanceStreamBytes, view.instanceStreamCapacityBytes,
			view.topologyReuseHits, view.topologyInvalidations, view.viewEvidenceReuseHits,
			view.viewEvidenceInvalidations, view.rejectedIncompleteKeyCount,
			view.rejectedUnsupportedVariantCount, view.rejectedOutOfRangeCount,
			view.rejectedStaleGenerationCount, view.fallbackAllocationFailureCount,
			view.fallbackRecoveryCount, view.valid}));
	}
	return result;
}

bool UnpackRenderOptimizationForJournal(
	const nlohmann::json& json,
	ProfilerRenderOptimizationEvidence& out,
	std::string& error)
{
	if (!json.is_array() || json.size() != out.views.size())
	{
		error = "Profiler journal V1 frame has malformed renderer optimization evidence.";
		return false;
	}
	for (std::size_t viewIndex = 0U; viewIndex < out.views.size(); ++viewIndex)
	{
		const nlohmann::json& values = json.at(viewIndex);
		const bool hasTimings = values.is_array() && values.size() == 28U;
		if (!values.is_array() || (values.size() != 24U && !hasTimings)
			|| !values.back().is_boolean())
		{
			error = "Profiler journal V1 frame has malformed renderer optimization view evidence.";
			return false;
		}
		for (std::size_t index = 0U; index + 1U < values.size(); ++index)
		{
			if (!values.at(index).is_number_unsigned())
			{
				error = "Profiler journal V1 frame renderer optimization evidence has a non-unsigned value.";
				return false;
			}
		}

		ProfilerRenderOptimizationViewSummary& view = out.views[viewIndex];
		std::size_t index = 0U;
		view.viewType = values.at(index++).get<std::uint32_t>();
		view.frameGeneration = values.at(index++).get<std::uint64_t>();
		view.topologyGeneration = values.at(index++).get<std::uint64_t>();
		view.viewGeneration = values.at(index++).get<std::uint64_t>();
		view.rendererGeneration = values.at(index++).get<std::uint64_t>();
		if (hasTimings)
		{
			view.topologyBuildNanoseconds = values.at(index++).get<std::uint64_t>();
			view.dynamicMaterializationNanoseconds = values.at(index++).get<std::uint64_t>();
			view.instanceStreamUploadNanoseconds = values.at(index++).get<std::uint64_t>();
			view.groupedCommandRecordingNanoseconds = values.at(index++).get<std::uint64_t>();
		}
		auto nextCounter = [&values, &index]() { return values.at(index++).get<std::uint32_t>(); };
		view.visibleInstanceCount = nextCounter();
		view.groupedInstanceCount = nextCounter();
		view.fallbackInstanceCount = nextCounter();
		view.groupedSubmissionCount = nextCounter();
		view.fallbackSubmissionCount = nextCounter();
		view.avoidedSubmissionCount = nextCounter();
		view.instanceStreamBytes = nextCounter();
		view.instanceStreamCapacityBytes = nextCounter();
		view.topologyReuseHits = nextCounter();
		view.topologyInvalidations = nextCounter();
		view.viewEvidenceReuseHits = nextCounter();
		view.viewEvidenceInvalidations = nextCounter();
		view.rejectedIncompleteKeyCount = nextCounter();
		view.rejectedUnsupportedVariantCount = nextCounter();
		view.rejectedOutOfRangeCount = nextCounter();
		view.rejectedStaleGenerationCount = nextCounter();
		view.fallbackAllocationFailureCount = nextCounter();
		view.fallbackRecoveryCount = nextCounter();
		view.valid = values.at(index).get<bool>();
	}
	if (!ValidateProfilerRenderOptimizationEvidence(out))
	{
		error = "Profiler journal V1 frame contains inconsistent renderer optimization evidence.";
		return false;
	}
	return true;
}

nlohmann::json PackJournalFrame(
	const ProfilerFrameRecord& frame,
	const ProfilerCaptureSettings& settings,
	JournalStringTable& table,
	bool includeSampleHierarchy)
{
	nlohmann::json result = {
		{"i", frame.frameIndex}, {"s", ToNanoseconds(frame.frameStartMs)},
		{"d", ToNanoseconds(frame.durationMs)}, {"c", frame.complete}
	};
	if (frame.animationDiagnostics.has_value())
	{
		// The typed facts are critical bounded evidence, not optional detail. They
		// use one compact array instead of three dynamically named CPU metrics.
		const ProfilerAnimationDiagnosticsFacts& facts = *frame.animationDiagnostics;
		result["x"] = nlohmann::json::array({facts.schedulerMembershipChecks, facts.dormant, facts.baselineIdentityMatched});
	}
	const bool includeOptional = KeepOptionalJournalData(frame, settings);
	if (frame.animationTelemetry)
	{
		// Aggregate counters and stage availability are critical every-frame proof;
		// only the bounded per-instance rows follow the optional detail policy.
		result["y"] = PackAnimationTelemetryForJournal(*frame.animationTelemetry, includeOptional);
	}
	if (frame.animationOptimization.has_value())
	{
		// Compact/complete conservation is aggregate evidence and therefore remains
		// available even when a sampled capture omits per-instance detail rows.
		result["n"] = PackAnimationOptimizationForJournal(*frame.animationOptimization);
	}
	if (frame.renderOptimization.has_value())
	{
		// Aggregate renderer evidence is small and required on every detail level;
		// optional per-group rows remain outside this fixed capture contract.
		result["o"] = PackRenderOptimizationForJournal(*frame.renderOptimization);
	}
	if (frame.framePublication.has_value())
	{
		const auto& e = *frame.framePublication;
		nlohmann::json packed = nlohmann::json::array({e.schemaVersion,e.availabilityMask,e.frameGeneration,e.sceneGeneration,
			e.renderSessionGeneration,e.topologyGeneration,e.dynamicGeneration,e.authorityGeneration,e.presentationGeneration,
			e.renderOutcome,e.animationOutcome,e.rejectionReason,e.persistentReuseCount,e.persistentRebuildCount,e.dynamicReuseCount,
			e.dynamicRebuildCount,e.authorityReuseCount,e.authorityRebuildCount,e.viewSelectionReuseCount,e.viewSelectionRebuildCount,e.viewCount});
		packed.push_back(e.disjointTimingNanoseconds);
		result["f"] = std::move(packed);
	}
	// Timing lanes are compact, bounded frame evidence. Persist them at every
	// detail level so an aggregate or unsampled frame remains useful after the
	// capture is saved and loaded.
	result["t"] = nlohmann::json::array();
	for (const ProfilerTimingStreamSample& stream : frame.timingStreams)
	{
		result["t"].push_back(nlohmann::json::array({table.Intern(stream.id), table.Intern(stream.label),
			static_cast<std::uint8_t>(stream.scope), static_cast<std::uint8_t>(stream.source),
			static_cast<std::uint8_t>(stream.status), ToNanoseconds(stream.milliseconds), table.Intern(stream.statusReason)}));
	}
	// Module identity, availability, and total time are compact frame-root
	// evidence. Keep them at every level so Terminal's incremental subsystem
	// summary can be verified after reopening an aggregate capture; only the
	// string-heavy presentation metrics follow the optional policy.
	result["m"] = nlohmann::json::array();
	for (const ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		nlohmann::json metrics = nlohmann::json::array();
		if (includeOptional)
		{
			for (const ProfilerMetric& metric : module.metrics)
			{
				// The compact typed field above is authoritative whenever it exists.
				// Do not serialize the public presentation aliases a second time.
				if (frame.animationDiagnostics.has_value() && IsPublicAnimationDiagnosticsMetric(metric.name))
				{
					continue;
				}
				metrics.push_back(nlohmann::json::array({table.Intern(metric.name), metric.value, table.Intern(metric.unit)}));
			}
		}
		result["m"].push_back(nlohmann::json::array({static_cast<std::uint8_t>(module.moduleId), module.available, ToNanoseconds(module.totalMs), metrics}));
	}
	const bool hasAggregateRoot = std::any_of(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& sample) {
		return sample.retainInAggregate;
	});
	if (includeSampleHierarchy && (includeOptional || hasAggregateRoot))
	{
		// Optional scopes follow the detail policy, while the single fixed
		// aggregate root is allowed through on otherwise compact frames.
		result["a"] = nlohmann::json::array();
		for (const ProfilerSample& sample : frame.samples)
		{
			if (!includeOptional && !sample.retainInAggregate)
			{
				continue;
			}
			result["a"].push_back(nlohmann::json::array({sample.sampleId, sample.parentSampleId,
				static_cast<std::uint8_t>(sample.moduleId), table.Intern(sample.name), table.Intern(sample.threadLabel),
				table.Intern(sample.taskLabel), ToNanoseconds(sample.startMs), ToNanoseconds(sample.durationMs),
				sample.depth, sample.occurrenceCount, sample.complete, sample.retainInAggregate}));
		}
	}
	if (frame.physics.has_value())
	{
		const ProfilerPhysicsFrameCounters& value = *frame.physics;
		result["p"] = nlohmann::json::array({value.tick, value.bodyCount, value.contactCount, value.queryCount, value.eventCount,
			value.rejectedCount, value.staleCount, value.commandQueueDepth, value.shapeCacheBytes, value.shapeCacheHits,
			value.shapeCacheMisses, value.snapshotBytes, value.snapshotHighWaterBytes, value.providerAllocations,
			value.providerAllocationHighWater, ToNanoseconds(value.prepareMs), ToNanoseconds(value.commitMs),
			ToNanoseconds(value.simulateMs), ToNanoseconds(value.queryMs), ToNanoseconds(value.publishMs)});
	}
	// Critical paths and correctness records deliberately bypass detail filtering.
	result["g"] = nlohmann::json::array();
	for (const PerformanceStageRecord& stage : frame.performanceStages)
	{
		result["g"].push_back(nlohmann::json::array({static_cast<std::uint8_t>(stage.domain), table.Intern(stage.stageName),
			PackDuration(stage.duration, table), table.Intern(stage.workloadName), stage.workItemCount, stage.sampled}));
	}
	result["b"] = nlohmann::json::array();
	for (const ProtectedBoundaryRecord& boundary : frame.protectedBoundaries)
	{
		result["b"].push_back(nlohmann::json::array({table.Intern(boundary.boundaryName), PackDuration(boundary.queueDuration, table),
			PackDuration(boundary.ownershipWaitDuration, table), PackDuration(boundary.executionDuration, table),
			PackDuration(boundary.flushDuration, table), PackDuration(boundary.publicationDuration, table)}));
	}
	result["w"] = nlohmann::json::array();
	for (const WorkerTelemetryRecord& worker : frame.workerTelemetry)
	{
		result["w"].push_back(nlohmann::json::array({worker.workerId, worker.queueDepth, worker.dispatchCount,
			worker.usefulWorkItemCount, PackDuration(worker.runDuration, table), PackDuration(worker.waitDuration, table), PackDuration(worker.idleDuration, table)}));
	}
	result["q"] = nlohmann::json::array();
	for (const PresentationWaitRecord& wait : frame.presentationWaits)
	{
		result["q"].push_back(nlohmann::json::array({static_cast<std::uint8_t>(wait.kind), PackDuration(wait.duration, table), table.Intern(wait.detail)}));
	}
	if (frame.responsiveness.has_value())
	{
		const FrameResponsivenessRecord& value = *frame.responsiveness;
		result["r"] = nlohmann::json::array({value.frameIndex, PackDuration(value.inputToPresentationDuration, table), PackDuration(value.frameDuration, table), value.presentationSequence});
	}
	if (frame.physicsWorkVolume.has_value())
	{
		const PhysicsWorkVolumeRecord& value = *frame.physicsWorkVolume;
		result["v"] = nlohmann::json::array({value.tick, value.totalBodyCount, value.activeBodyCount, value.examinedBodyCount,
			value.extractedBodyCount, value.convertedBodyCount, value.changedBodyCount, value.publishedBodyCount});
	}
	return result;
}

bool ReadArray(const nlohmann::json& parent, const char* name, std::size_t minimum, const nlohmann::json*& result, std::string& error)
{
	const auto found = parent.find(name);
	if (found == parent.end() || !found->is_array() || found->size() < minimum)
	{
		error = std::string("Profiler journal V1 frame contains a malformed '") + name + "' field.";
		return false;
	}
	result = &*found;
	return true;
}

bool UnpackJournalFrame(const nlohmann::json& json, const std::vector<std::string>& strings, ProfilerFrameRecord& out, std::string& error)
{
	if (!json.is_object() || !json.contains("i") || !json.contains("s") || !json.contains("d") || !json.contains("c"))
	{
		error = "Profiler journal V1 frame contains a malformed frame.";
		return false;
	}
	try
	{
		out.frameIndex = json.at("i").get<std::uint64_t>();
		out.frameStartMs = ToMilliseconds(json.at("s").get<std::uint64_t>());
		out.durationMs = ToMilliseconds(json.at("d").get<std::uint64_t>());
		out.complete = json.at("c").get<bool>();
		if (const auto it = json.find("x"); it != json.end())
		{
			// Reject malformed typed facts before exposing a partially reconstructed
			// frame. Optional journal evidence can omit this additive field.
			if (!it->is_array() || it->size() != 3u || !(*it)[0].is_number_unsigned()
				|| !(*it)[1].is_boolean() || !(*it)[2].is_boolean())
			{
				error = "Profiler journal V1 frame has malformed compact animation diagnostics.";
				return false;
			}
			out.animationDiagnostics = ProfilerAnimationDiagnosticsFacts{
				(*it)[0].get<std::uint64_t>(), (*it)[1].get<bool>(), (*it)[2].get<bool>()};
		}
		if (const auto it = json.find("y"); it != json.end())
		{
			ProfilerAnimationFrameTelemetry telemetry;
			if (!UnpackAnimationTelemetryForJournal(*it, telemetry, error))
			{
				return false;
			}
			// A typed active record and the compact common facts must agree. New
			// writers always emit both; rejecting a mismatch prevents split evidence.
			if (!out.animationDiagnostics.has_value()
				|| out.animationDiagnostics->dormant
				|| out.animationDiagnostics->schedulerMembershipChecks != telemetry.summary.schedulerMembershipChecks)
			{
				error = "Profiler journal V1 frame animation telemetry conflicts with compact diagnostics.";
				return false;
			}
			out.animationTelemetry =
				std::make_shared<const ProfilerAnimationFrameTelemetry>(std::move(telemetry));
		}
		if (const auto it = json.find("n"); it != json.end())
		{
			ProfilerAnimationOptimizationEvidence evidence;
			if (!UnpackAnimationOptimizationForJournal(*it, evidence, error))
			{
				return false;
			}
			out.animationOptimization = std::move(evidence);
		}
		if (const auto it = json.find("o"); it != json.end())
		{
			ProfilerRenderOptimizationEvidence evidence;
			if (!UnpackRenderOptimizationForJournal(*it, evidence, error))
			{
				return false;
			}
			out.renderOptimization = std::move(evidence);
		}
		if (const auto it = json.find("f"); it != json.end())
		{
			if (!it->is_array() || it->size() != 22u || !(*it)[21].is_array() || (*it)[21].size() != 8u)
			{
				error = "Profiler journal V1 frame has malformed frame-publication evidence.";
				return false;
			}
			ProfilerFramePublicationEvidence e;
			e.schemaVersion=(*it)[0].get<std::uint32_t>(); e.availabilityMask=(*it)[1].get<std::uint64_t>();
			e.frameGeneration=(*it)[2].get<std::uint64_t>(); e.sceneGeneration=(*it)[3].get<std::uint64_t>();
			e.renderSessionGeneration=(*it)[4].get<std::uint64_t>(); e.topologyGeneration=(*it)[5].get<std::uint64_t>();
			e.dynamicGeneration=(*it)[6].get<std::uint64_t>(); e.authorityGeneration=(*it)[7].get<std::uint64_t>();
			e.presentationGeneration=(*it)[8].get<std::uint64_t>(); e.renderOutcome=(*it)[9].get<std::uint32_t>();
			e.animationOutcome=(*it)[10].get<std::uint32_t>(); e.rejectionReason=(*it)[11].get<std::uint32_t>();
			e.persistentReuseCount=(*it)[12].get<std::uint32_t>(); e.persistentRebuildCount=(*it)[13].get<std::uint32_t>();
			e.dynamicReuseCount=(*it)[14].get<std::uint32_t>(); e.dynamicRebuildCount=(*it)[15].get<std::uint32_t>();
			e.authorityReuseCount=(*it)[16].get<std::uint32_t>(); e.authorityRebuildCount=(*it)[17].get<std::uint32_t>();
			e.viewSelectionReuseCount=(*it)[18].get<std::uint32_t>(); e.viewSelectionRebuildCount=(*it)[19].get<std::uint32_t>();
			e.viewCount=(*it)[20].get<std::uint32_t>();
			for (std::size_t index=0; index<8; ++index) e.disjointTimingNanoseconds[index]=(*it)[21][index].get<std::uint64_t>();
			if (!ValidateProfilerFramePublicationEvidence(e)) { error="Profiler journal V1 frame contains invalid frame-publication evidence."; return false; }
			out.framePublication=std::move(e);
		}
		if (const auto it = json.find("t"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 7u) { error = "Profiler journal V1 frame has malformed timing data."; return false; }
			const std::string* id = StringAt(strings, value[0], error); const std::string* label = StringAt(strings, value[1], error); const std::string* reason = StringAt(strings, value[6], error);
			if (!id || !label || !reason) return false;
			ProfilerTimingStreamSample stream{*id, *label, static_cast<ProfilerTimingStreamScope>(value[2].get<std::uint8_t>()), static_cast<ProfilerTimingStreamSource>(value[3].get<std::uint8_t>()), static_cast<ProfilerTimingStreamStatus>(value[4].get<std::uint8_t>()), ToMilliseconds(value[5].get<std::uint64_t>()), *reason};
			out.timingStreams.push_back(std::move(stream));
		}
		if (const auto it = json.find("m"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 4u || !value[3].is_array()) { error = "Profiler journal V1 frame has malformed module data."; return false; }
			ProfilerModuleSummary module{static_cast<ProfilerModuleId>(value[0].get<std::uint8_t>()), value[1].get<bool>(), ToMilliseconds(value[2].get<std::uint64_t>()), {}};
			for (const auto& metric : value[3]) { if (!metric.is_array() || metric.size() != 3u) { error = "Profiler journal V1 frame has malformed metric data."; return false; } const std::string* name = StringAt(strings, metric[0], error); const std::string* unit = StringAt(strings, metric[2], error); if (!name || !unit) return false; module.metrics.push_back({*name, metric[1].get<double>(), *unit}); }
			out.moduleSummaries.push_back(std::move(module));
		}
		if (const auto it = json.find("a"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || (value.size() != 11u && value.size() != 12u)) { error = "Profiler journal V1 frame has malformed sample data."; return false; }
			const std::string* name = StringAt(strings, value[3], error); const std::string* thread = StringAt(strings, value[4], error); const std::string* task = StringAt(strings, value[5], error); if (!name || !thread || !task) return false;
			ProfilerSample sample{value[0].get<std::uint64_t>(), value[1].get<std::uint64_t>(), static_cast<ProfilerModuleId>(value[2].get<std::uint8_t>()), *name, *thread, *task, ToMilliseconds(value[6].get<std::uint64_t>()), ToMilliseconds(value[7].get<std::uint64_t>()), value[8].get<std::uint32_t>(), value[9].get<std::uint32_t>(), value[10].get<bool>()};
			// Eleven-field records predate the aggregate frame root and therefore
			// remain optional when an older capture is loaded and saved again.
			sample.retainInAggregate = value.size() == 12u && value[11].get<bool>();
			out.samples.push_back(std::move(sample));
		}
		if (const auto it = json.find("p"); it != json.end())
		{
			if (!it->is_array() || it->size() != 20u) { error = "Profiler journal V1 frame has malformed physics counters."; return false; }
			ProfilerPhysicsFrameCounters physics;
			physics.tick = (*it)[0].get<std::uint64_t>(); physics.bodyCount = (*it)[1].get<std::uint64_t>(); physics.contactCount = (*it)[2].get<std::uint64_t>(); physics.queryCount = (*it)[3].get<std::uint64_t>(); physics.eventCount = (*it)[4].get<std::uint64_t>(); physics.rejectedCount = (*it)[5].get<std::uint64_t>(); physics.staleCount = (*it)[6].get<std::uint64_t>(); physics.commandQueueDepth = (*it)[7].get<std::uint64_t>(); physics.shapeCacheBytes = (*it)[8].get<std::uint64_t>(); physics.shapeCacheHits = (*it)[9].get<std::uint64_t>(); physics.shapeCacheMisses = (*it)[10].get<std::uint64_t>(); physics.snapshotBytes = (*it)[11].get<std::uint64_t>(); physics.snapshotHighWaterBytes = (*it)[12].get<std::uint64_t>(); physics.providerAllocations = (*it)[13].get<std::uint64_t>(); physics.providerAllocationHighWater = (*it)[14].get<std::uint64_t>(); physics.prepareMs = ToMilliseconds((*it)[15].get<std::uint64_t>()); physics.commitMs = ToMilliseconds((*it)[16].get<std::uint64_t>()); physics.simulateMs = ToMilliseconds((*it)[17].get<std::uint64_t>()); physics.queryMs = ToMilliseconds((*it)[18].get<std::uint64_t>()); physics.publishMs = ToMilliseconds((*it)[19].get<std::uint64_t>()); out.physics = physics;
		}
		if (const auto it = json.find("g"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 6u) { error = "Profiler journal V1 frame has malformed stage data."; return false; } const std::string* name = StringAt(strings, value[1], error); const std::string* workload = StringAt(strings, value[3], error); if (!name || !workload) return false; PerformanceStageRecord stage; stage.domain = static_cast<PerformanceStageDomain>(value[0].get<std::uint8_t>()); stage.stageName = *name; stage.workloadName = *workload; stage.workItemCount = value[4].get<std::uint64_t>(); stage.sampled = value[5].get<bool>(); if (!UnpackDuration(value[2], strings, stage.duration, error) || !ValidatePerformanceStageRecord(stage).valid) return false; out.performanceStages.push_back(std::move(stage));
		}
		if (const auto it = json.find("b"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 6u) { error = "Profiler journal V1 frame has malformed protected-boundary data."; return false; } const std::string* name = StringAt(strings, value[0], error); if (!name) return false; ProtectedBoundaryRecord boundary; boundary.boundaryName = *name; if (!UnpackDuration(value[1], strings, boundary.queueDuration, error) || !UnpackDuration(value[2], strings, boundary.ownershipWaitDuration, error) || !UnpackDuration(value[3], strings, boundary.executionDuration, error) || !UnpackDuration(value[4], strings, boundary.flushDuration, error) || !UnpackDuration(value[5], strings, boundary.publicationDuration, error) || !ValidateProtectedBoundaryRecord(boundary).valid) return false; out.protectedBoundaries.push_back(std::move(boundary));
		}
		if (const auto it = json.find("w"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 7u) { error = "Profiler journal V1 frame has malformed worker telemetry."; return false; } WorkerTelemetryRecord worker; worker.workerId = value[0].get<std::uint32_t>(); worker.queueDepth = value[1].get<std::uint64_t>(); worker.dispatchCount = value[2].get<std::uint64_t>(); worker.usefulWorkItemCount = value[3].get<std::uint64_t>(); if (!UnpackDuration(value[4], strings, worker.runDuration, error) || !UnpackDuration(value[5], strings, worker.waitDuration, error) || !UnpackDuration(value[6], strings, worker.idleDuration, error) || !ValidateWorkerTelemetryRecord(worker).valid) return false; out.workerTelemetry.push_back(std::move(worker));
		}
		if (const auto it = json.find("q"); it != json.end()) for (const auto& value : *it)
		{
			if (!value.is_array() || value.size() != 3u) { error = "Profiler journal V1 frame has malformed presentation wait data."; return false; } const std::string* detail = StringAt(strings, value[2], error); if (!detail) return false; PresentationWaitRecord wait; wait.kind = static_cast<PresentationWaitKind>(value[0].get<std::uint8_t>()); wait.detail = *detail; if (!UnpackDuration(value[1], strings, wait.duration, error) || !ValidatePresentationWaitRecord(wait).valid) return false; out.presentationWaits.push_back(std::move(wait));
		}
		if (const auto it = json.find("r"); it != json.end()) { if (!it->is_array() || it->size() != 4u) { error = "Profiler journal V1 frame has malformed responsiveness data."; return false; } FrameResponsivenessRecord record; record.frameIndex = (*it)[0].get<std::uint64_t>(); record.presentationSequence = (*it)[3].get<std::uint64_t>(); if (!UnpackDuration((*it)[1], strings, record.inputToPresentationDuration, error) || !UnpackDuration((*it)[2], strings, record.frameDuration, error) || !ValidateFrameResponsivenessRecord(record).valid) return false; out.responsiveness = std::move(record); }
		if (const auto it = json.find("v"); it != json.end()) { if (!it->is_array() || it->size() != 8u) { error = "Profiler journal V1 frame has malformed physics-volume data."; return false; } PhysicsWorkVolumeRecord volume{(*it)[0].get<std::uint64_t>(), (*it)[1].get<std::uint64_t>(), (*it)[2].get<std::uint64_t>(), (*it)[3].get<std::uint64_t>(), (*it)[4].get<std::uint64_t>(), (*it)[5].get<std::uint64_t>(), (*it)[6].get<std::uint64_t>(), (*it)[7].get<std::uint64_t>()}; if (!ValidatePhysicsWorkVolumeRecord(volume).valid) { error = "Profiler journal V1 frame has invalid physics-volume data."; return false; } out.physicsWorkVolume = volume; }
		MaterializeProfilerAnimationPresentationMetricsImpl(out);
		return true;
	}
	catch (const nlohmann::json::exception& exception)
	{
		error = exception.what();
		return false;
	}
}

bool EncodeProfilerJournalFrameImpl(
	const ProfilerFrameRecord& frame,
	const ProfilerCaptureSettings& settings,
	std::string& outPayload,
	std::string& outError)
{
	constexpr std::array<char, 4u> kFrameMagic = {'F', 'R', 'M', '1'};
	const auto appendU32 = [](std::string& output, std::uint32_t value) {
		for (std::size_t index = 0u; index < sizeof(value); ++index)
		{
			output.push_back(static_cast<char>((value >> (index * 8u)) & 0xffu));
		}
	};
	const auto appendU64 = [](std::string& output, std::uint64_t value) {
		for (std::size_t index = 0u; index < sizeof(value); ++index)
		{
			output.push_back(static_cast<char>((value >> (index * 8u)) & 0xffu));
		}
	};
	try
	{
		JournalStringTable strings;
		CollectFrameStrings(strings, frame, settings);
		if (strings.Values().size() > kJournalMaximumStringCount)
		{
			outError = "Profiler journal frame has too many local strings.";
			return false;
		}
		nlohmann::json envelope;
		envelope["strings"] = strings.Values();
		// Samples dominate detailed captures. Keep the smaller heterogeneous frame
		// facts in the typed CBOR envelope, but write the regular sample rows below
		// as fixed binary records instead of allocating one JSON array per scope.
		envelope["frame"] = PackJournalFrame(frame, settings, strings, false);
		// Journal V1 is a binary format. Encoding the already-typed frame DOM as
		// CBOR avoids formatting and reparsing hundreds of thousands of decimal
		// numbers per segment, while preserving the same strict typed schema and
		// segment-level Zstd/integrity envelope. There is intentionally no text-JSON
		// compatibility path: pre-feature captures are unsupported by contract.
		const std::vector<std::uint8_t> binary = nlohmann::json::to_cbor(envelope);
		outPayload.assign(kFrameMagic.data(), kFrameMagic.size());
		appendU64(outPayload, static_cast<std::uint64_t>(binary.size()));
		outPayload.append(
			reinterpret_cast<const char*>(binary.data()), binary.size());

		const bool includeOptional = KeepOptionalJournalData(frame, settings);
		const std::uint32_t sampleCount = static_cast<std::uint32_t>(std::count_if(
			frame.samples.begin(), frame.samples.end(), [includeOptional](const ProfilerSample& sample) {
				return includeOptional || sample.retainInAggregate;
			}));
		appendU32(outPayload, sampleCount);
		for (const ProfilerSample& sample : frame.samples)
		{
			if (!includeOptional && !sample.retainInAggregate)
			{
				continue;
			}
			appendU64(outPayload, sample.sampleId);
			appendU64(outPayload, sample.parentSampleId);
			outPayload.push_back(static_cast<char>(sample.moduleId));
			appendU32(outPayload, strings.Intern(sample.name));
			appendU32(outPayload, strings.Intern(sample.threadLabel));
			appendU32(outPayload, strings.Intern(sample.taskLabel));
			appendU64(outPayload, ToNanoseconds(sample.startMs));
			appendU64(outPayload, ToNanoseconds(sample.durationMs));
			appendU32(outPayload, sample.depth);
			appendU32(outPayload, sample.occurrenceCount);
			outPayload.push_back(sample.complete ? '\1' : '\0');
			outPayload.push_back(sample.retainInAggregate ? '\1' : '\0');
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		outError = exception.what();
		return false;
	}
}

bool DecodeProfilerJournalFrameImpl(
	std::string_view payload,
	ProfilerFrameRecord& outFrame,
	std::string& outError)
{
	constexpr std::array<char, 4u> kFrameMagic = {'F', 'R', 'M', '1'};
	const auto consumeU32 = [](std::string_view& input, std::uint32_t& value) {
		if (input.size() < sizeof(value))
		{
			return false;
		}
		value = 0u;
		for (std::size_t index = 0u; index < sizeof(value); ++index)
		{
			value |= static_cast<std::uint32_t>(
				static_cast<unsigned char>(input[index])) << (index * 8u);
		}
		input.remove_prefix(sizeof(value));
		return true;
	};
	const auto consumeU64 = [](std::string_view& input, std::uint64_t& value) {
		if (input.size() < sizeof(value))
		{
			return false;
		}
		value = 0u;
		for (std::size_t index = 0u; index < sizeof(value); ++index)
		{
			value |= static_cast<std::uint64_t>(
				static_cast<unsigned char>(input[index])) << (index * 8u);
		}
		input.remove_prefix(sizeof(value));
		return true;
	};
	try
	{
		if (payload.size() < kFrameMagic.size()
			|| !std::equal(kFrameMagic.begin(), kFrameMagic.end(), payload.begin()))
		{
			outError = "Profiler journal frame has an unsupported V1 payload identity.";
			return false;
		}
		payload.remove_prefix(kFrameMagic.size());
		std::uint64_t cborBytes = 0u;
		if (!consumeU64(payload, cborBytes) || cborBytes > payload.size())
		{
			outError = "Profiler journal frame has a truncated CBOR envelope.";
			return false;
		}
		const std::string_view cborPayload = payload.substr(0u, static_cast<std::size_t>(cborBytes));
		payload.remove_prefix(static_cast<std::size_t>(cborBytes));
		const nlohmann::json envelope = nlohmann::json::from_cbor(
			cborPayload.begin(), cborPayload.end(), true, true);
		const auto stringsJson = envelope.find("strings");
		const auto frameJson = envelope.find("frame");
		if (stringsJson == envelope.end() || frameJson == envelope.end()
			|| !stringsJson->is_array() || stringsJson->size() > kJournalMaximumStringCount)
		{
			outError = "Profiler journal frame has an invalid local string table.";
			return false;
		}
		std::vector<std::string> strings;
		strings.reserve(stringsJson->size());
		for (const nlohmann::json& value : *stringsJson)
		{
			if (!value.is_string())
			{
				outError = "Profiler journal frame local string table contains a non-string.";
				return false;
			}
			strings.push_back(value.get<std::string>());
		}
		if (!UnpackJournalFrame(*frameJson, strings, outFrame, outError))
		{
			return false;
		}
		std::uint32_t sampleCount = 0u;
		if (!consumeU32(payload, sampleCount) || sampleCount > kJournalMaximumStringCount)
		{
			outError = "Profiler journal frame has an invalid sample count.";
			return false;
		}
		outFrame.samples.reserve(outFrame.samples.size() + sampleCount);
		for (std::uint32_t sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
		{
			ProfilerSample sample;
			std::uint32_t nameIndex = 0u;
			std::uint32_t threadIndex = 0u;
			std::uint32_t taskIndex = 0u;
			std::uint64_t startNanoseconds = 0u;
			std::uint64_t durationNanoseconds = 0u;
			if (!consumeU64(payload, sample.sampleId)
				|| !consumeU64(payload, sample.parentSampleId) || payload.empty())
			{
				outError = "Profiler journal frame has a truncated sample identity.";
				return false;
			}
			sample.moduleId = static_cast<ProfilerModuleId>(
				static_cast<unsigned char>(payload.front()));
			payload.remove_prefix(1u);
			if (!consumeU32(payload, nameIndex) || !consumeU32(payload, threadIndex)
				|| !consumeU32(payload, taskIndex)
				|| !consumeU64(payload, startNanoseconds)
				|| !consumeU64(payload, durationNanoseconds)
				|| !consumeU32(payload, sample.depth)
				|| !consumeU32(payload, sample.occurrenceCount)
				|| payload.size() < 2u || nameIndex >= strings.size()
				|| threadIndex >= strings.size() || taskIndex >= strings.size())
			{
				outError = "Profiler journal frame contains malformed sample data.";
				return false;
			}
			sample.name = strings[nameIndex];
			sample.threadLabel = strings[threadIndex];
			sample.taskLabel = strings[taskIndex];
			sample.startMs = ToMilliseconds(startNanoseconds);
			sample.durationMs = ToMilliseconds(durationNanoseconds);
			sample.complete = payload[0] != '\0';
			sample.retainInAggregate = payload[1] != '\0';
			payload.remove_prefix(2u);
			outFrame.samples.push_back(std::move(sample));
		}
		if (!payload.empty())
		{
			outError = "Profiler journal frame has trailing sample bytes.";
			return false;
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		outError = exception.what();
		return false;
	}
}

} // namespace

void MaterializeProfilerAnimationPresentationMetrics(ProfilerFrameRecord& frame)
{
	MaterializeProfilerAnimationPresentationMetricsImpl(frame);
}

void MaterializeProfilerAnimationPresentationMetrics(ProfilerCaptureDocument& document)
{
	for (ProfilerFrameRecord& frame : document.frames)
	{
		MaterializeProfilerAnimationPresentationMetricsImpl(frame);
	}
}

bool EncodeProfilerJournalFrame(
	const ProfilerFrameRecord& frame,
	const ProfilerCaptureSettings& settings,
	std::string& outPayload,
	std::string& outError)
{
	return EncodeProfilerJournalFrameImpl(frame, settings, outPayload, outError);
}

bool DecodeProfilerJournalFrame(
	std::string_view payload,
	ProfilerFrameRecord& outFrame,
	std::string& outError)
{
	return DecodeProfilerJournalFrameImpl(payload, outFrame, outError);
}
}
