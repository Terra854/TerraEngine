// Profiler.cpp — Contains the Profiler file-specific types, helper functions, and module integration code implementation for runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#include "Profiling/Profiler.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RenderTypes.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace TerraEngine::Profiling
{
namespace
{
constexpr std::size_t kProfilerThreadSampleCapacity = 1024u;
constexpr std::size_t kProfilerMaximumOpenSampleDepth = 64u;
constexpr std::size_t kProfilerMaximumThreadBuffers = 128u;
constexpr std::size_t kProfilerSampleNameCapacity = 128u;
constexpr std::size_t kProfilerTaskLabelCapacity = 128u;

std::int64_t SteadyNowNanoseconds()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

double NanosecondsToMilliseconds(std::int64_t nanoseconds)
{
	return static_cast<double>(std::max<std::int64_t>(0, nanoseconds)) / 1000000.0;
}

std::string CurrentThreadLabel()
{
	std::ostringstream stream;
	stream << std::this_thread::get_id();
	return stream.str();
}

// MergeWorkloadWallTimeNanoseconds converts partition-level scheduler evidence
// into one coordinator wall span. Absolute endpoints are clipped to the current
// profiler frame, then overlapping workers are unioned instead of summed.
std::uint64_t MergeWorkloadWallTimeNanoseconds(
	const std::vector<WorkloadExecutionRecord>& records,
	std::initializer_list<std::string_view> workloadNames,
	std::uint64_t frameBeginNanoseconds,
	std::uint64_t frameEndNanoseconds)
{
	std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
	for (const WorkloadExecutionRecord& record : records)
	{
		const bool selected = std::any_of(workloadNames.begin(), workloadNames.end(),
			[&record](const std::string_view name)
			{
				return record.workloadName == name;
			});
		if (!selected || record.runStartNanoseconds == 0U
			|| record.runEndNanoseconds <= record.runStartNanoseconds)
		{
			continue;
		}
		const std::uint64_t begin = std::max(frameBeginNanoseconds,
			record.runStartNanoseconds);
		const std::uint64_t end = std::min(frameEndNanoseconds,
			record.runEndNanoseconds);
		if (end > begin)
		{
			intervals.emplace_back(begin, end);
		}
	}
	std::sort(intervals.begin(), intervals.end());
	std::uint64_t total = 0U;
	if (intervals.empty())
	{
		return total;
	}
	std::uint64_t mergedBegin = intervals.front().first;
	std::uint64_t mergedEnd = intervals.front().second;
	for (std::size_t index = 1U; index < intervals.size(); ++index)
	{
		if (intervals[index].first <= mergedEnd)
		{
			mergedEnd = std::max(mergedEnd, intervals[index].second);
			continue;
		}
		total += mergedEnd - mergedBegin;
		mergedBegin = intervals[index].first;
		mergedEnd = intervals[index].second;
	}
	return total + mergedEnd - mergedBegin;
}

// CalculateSampleCoverageMs merges nested and overlapping samples before
// reporting a total. Grouping by thread produces CPU work (parallel lanes add),
// while one shared group produces elapsed wall coverage for the frame.
double CalculateSampleCoverageMs(
	const std::vector<ProfilerSample>& samples,
	ProfilerModuleId moduleId,
	bool groupByThread)
{
	std::unordered_map<std::string, std::vector<std::pair<double, double>>> intervalsByLane;
	for (const ProfilerSample& sample : samples)
	{
		if (sample.moduleId != moduleId || !std::isfinite(sample.startMs)
			|| !std::isfinite(sample.durationMs) || sample.durationMs <= 0.0)
		{
			continue;
		}
		intervalsByLane[groupByThread ? sample.threadLabel : std::string{}].emplace_back(
			sample.startMs, sample.startMs + sample.durationMs);
	}

	double coveredMs = 0.0;
	for (auto& [lane, intervals] : intervalsByLane)
	{
		(void)lane;
		std::sort(intervals.begin(), intervals.end());
		if (intervals.empty())
		{
			continue;
		}
		double mergedBeginMs = intervals.front().first;
		double mergedEndMs = intervals.front().second;
		for (std::size_t index = 1u; index < intervals.size(); ++index)
		{
			if (intervals[index].first <= mergedEndMs)
			{
				mergedEndMs = std::max(mergedEndMs, intervals[index].second);
				continue;
			}
			coveredMs += mergedEndMs - mergedBeginMs;
			mergedBeginMs = intervals[index].first;
			mergedEndMs = intervals[index].second;
		}
		coveredMs += mergedEndMs - mergedBeginMs;
	}
	return coveredMs;
}

template <std::size_t Capacity>
bool CopyInlineText(std::array<char, Capacity>& destination, const char* source, const char* fallback)
{
	static_assert(Capacity >= 2u);
	const char* selected = source != nullptr ? source : fallback;
	std::size_t index = 0u;
	while (index + 1u < Capacity && selected[index] != '\0')
	{
		destination[index] = selected[index];
		++index;
	}
	const bool truncated = selected[index] != '\0';
	destination[index] = '\0';
	return truncated;
}

struct BufferedProfilerSample
{
	std::uint64_t sessionGeneration = 0u;
	std::uint64_t frameToken = 0u;
	std::uint64_t sampleId = 0u;
	std::uint64_t parentSampleId = 0u;
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	std::array<char, kProfilerSampleNameCapacity> name{};
	std::array<char, kProfilerTaskLabelCapacity> taskLabel{};
	std::int64_t startNanoseconds = 0;
	std::int64_t durationNanoseconds = 0;
	std::uint32_t depth = 0u;
	bool complete = true;
};

struct OpenProfilerSample
{
	BufferedProfilerSample sample;
	bool fullyExcluded = false;
};

struct ProfilerThreadBuffer
{
	// Only the owning producer thread reads or writes the open stack. Completed
	// entries use a bounded single-producer/single-consumer ring so EndFrame can
	// merge published records without taking a lock in BeginSample/EndSample.
	std::array<OpenProfilerSample, kProfilerMaximumOpenSampleDepth> openSamples{};
	std::size_t openSampleCount = 0u;
	std::array<BufferedProfilerSample, kProfilerThreadSampleCapacity> completedSamples{};
	std::atomic_uint64_t writeSequence = 0u;
	std::atomic_uint64_t readSequence = 0u;
	std::atomic_uint64_t droppedSamples = 0u;
	std::atomic_uint64_t truncatedLabels = 0u;
	std::atomic_uint64_t lateSamples = 0u;
	std::atomic_uint64_t queuedTasks = 0u;
	std::atomic_uint64_t startedTasks = 0u;
	std::atomic_uint64_t finishedTasks = 0u;
	// Compact counters and diagnostics carry the unique frame token that owns
	// them. A producer already past the fast flag during Clear/Start can finish
	// safely without its stale atomic increment entering a later session.
	std::atomic_uint64_t counterFrameToken = 0u;
	std::atomic_uint64_t diagnosticFrameToken = 0u;
	std::uint64_t boundSessionGeneration = 0u;
	std::uint64_t boundFrameToken = 0u;
	std::int64_t editorWorkStartNanoseconds = 0;
	std::uint32_t editorWorkDepth = 0u;
	std::string threadLabel;
};
}

struct ProfilerHotPathState
{
	// Registration occurs once per participating thread. Normal scope recording
	// uses only the thread-owned buffer and never touches this mutex.
	std::mutex registryMutex;
	// Keep enrolled buffers alive after short-lived workers exit so their
	// release-published samples survive until the protected EndFrame merge.
	std::vector<std::shared_ptr<ProfilerThreadBuffer>> registeredBuffers;
	std::atomic_uint64_t sessionGeneration = 1u;
	std::atomic_uint64_t activeFrameToken = 0u;
	std::atomic_uint64_t nextFrameToken = 1u;
	std::atomic_int64_t activeFrameStartNanoseconds = 0;
	std::atomic_int64_t excludedEditorNanoseconds = 0;
	std::atomic_uint64_t nextSampleId = 1u;
	std::atomic_uint64_t rejectedThreadBuffers = 0u;
	std::atomic_uint64_t rejectedThreadBufferFrameToken = 0u;
};

namespace
{
struct ProfilerThreadBinding
{
	ProfilerHotPathState* owner = nullptr;
	// A live thread owns one strong reference. EndFrame can therefore distinguish
	// it from an exited transient worker after the worker's published ring drains.
	std::shared_ptr<ProfilerThreadBuffer> buffer;
	bool registrationRejected = false;
	std::uint64_t rejectedFrameToken = 0u;
};

thread_local ProfilerThreadBinding t_profilerThreadBinding;

ProfilerThreadBuffer* AcquireThreadBuffer(ProfilerHotPathState& state, std::uint64_t frameToken)
{
	if (t_profilerThreadBinding.owner == &state && t_profilerThreadBinding.buffer)
	{
		return t_profilerThreadBinding.buffer.get();
	}
	if (t_profilerThreadBinding.owner == &state
		&& t_profilerThreadBinding.registrationRejected
		&& t_profilerThreadBinding.rejectedFrameToken == frameToken)
	{
		return {};
	}

	std::shared_ptr<ProfilerThreadBuffer> buffer;
	{
		std::lock_guard<std::mutex> lock(state.registryMutex);
		if (state.activeFrameToken.load(std::memory_order_acquire) != frameToken)
		{
			// A caller can pass the hot flag immediately before Pause/Clear. Once
			// registration reaches the serialized registry, reject that stale
			// attempt without retagging the current frame's shared diagnostics.
			return nullptr;
		}
		// EndFrame normally removes drained transient workers. This small
		// registration-time sweep also recovers slots after a Clear or another
		// protected operation that consumed their final published records.
		state.registeredBuffers.erase(
			std::remove_if(
				state.registeredBuffers.begin(),
				state.registeredBuffers.end(),
				[](const std::shared_ptr<ProfilerThreadBuffer>& registeredBuffer) {
					return registeredBuffer.use_count() == 1u
						&& registeredBuffer->readSequence.load(std::memory_order_acquire)
							== registeredBuffer->writeSequence.load(std::memory_order_acquire);
				}),
			state.registeredBuffers.end());
		if (state.registeredBuffers.size() >= kProfilerMaximumThreadBuffers)
		{
			if (state.rejectedThreadBufferFrameToken.load(std::memory_order_relaxed) != frameToken)
			{
				state.rejectedThreadBuffers.store(0u, std::memory_order_relaxed);
				state.rejectedThreadBufferFrameToken.store(frameToken, std::memory_order_release);
			}
			state.rejectedThreadBuffers.fetch_add(1u, std::memory_order_relaxed);
			t_profilerThreadBinding.owner = &state;
			t_profilerThreadBinding.buffer.reset();
			t_profilerThreadBinding.registrationRejected = true;
			t_profilerThreadBinding.rejectedFrameToken = frameToken;
			return nullptr;
		}
		buffer = std::make_shared<ProfilerThreadBuffer>();
		buffer->threadLabel = CurrentThreadLabel();
		state.registeredBuffers.push_back(buffer);
	}
	t_profilerThreadBinding.owner = &state;
	t_profilerThreadBinding.buffer = std::move(buffer);
	t_profilerThreadBinding.registrationRejected = false;
	t_profilerThreadBinding.rejectedFrameToken = 0u;
	return t_profilerThreadBinding.buffer.get();
}

void PrepareThreadBufferForFrame(
	ProfilerThreadBuffer& buffer,
	std::uint64_t sessionGeneration,
	std::uint64_t frameToken)
{
	if (buffer.counterFrameToken.load(std::memory_order_acquire) != frameToken)
	{
		buffer.queuedTasks.store(0u, std::memory_order_relaxed);
		buffer.startedTasks.store(0u, std::memory_order_relaxed);
		buffer.finishedTasks.store(0u, std::memory_order_relaxed);
		buffer.counterFrameToken.store(frameToken, std::memory_order_release);
	}
	if (buffer.diagnosticFrameToken.load(std::memory_order_acquire) != frameToken)
	{
		buffer.droppedSamples.store(0u, std::memory_order_relaxed);
		buffer.truncatedLabels.store(0u, std::memory_order_relaxed);
		buffer.lateSamples.store(0u, std::memory_order_relaxed);
		buffer.diagnosticFrameToken.store(frameToken, std::memory_order_release);
	}
	if (buffer.boundSessionGeneration == sessionGeneration && buffer.boundFrameToken == frameToken)
	{
		return;
	}
	if (buffer.openSampleCount != 0u || buffer.editorWorkDepth != 0u)
	{
		// A stale open scope cannot be attached to a new capture/frame. Count it
		// and reset only producer-owned stack state; published ring entries remain
		// available for the consumer to classify at the protected merge boundary.
		buffer.lateSamples.fetch_add(
			static_cast<std::uint64_t>(buffer.openSampleCount) + (buffer.editorWorkDepth != 0u ? 1u : 0u),
			std::memory_order_relaxed);
	}
	buffer.openSampleCount = 0u;
	buffer.editorWorkDepth = 0u;
	buffer.boundSessionGeneration = sessionGeneration;
	buffer.boundFrameToken = frameToken;
}

bool PublishCompletedSample(ProfilerThreadBuffer& buffer, BufferedProfilerSample sample)
{
	const std::uint64_t writeSequence = buffer.writeSequence.load(std::memory_order_relaxed);
	const std::uint64_t readSequence = buffer.readSequence.load(std::memory_order_acquire);
	if (writeSequence - readSequence >= kProfilerThreadSampleCapacity)
	{
		buffer.droppedSamples.fetch_add(1u, std::memory_order_relaxed);
		return false;
	}

	buffer.completedSamples[writeSequence % kProfilerThreadSampleCapacity] = std::move(sample);
	buffer.writeSequence.store(writeSequence + 1u, std::memory_order_release);
	return true;
}

void DiscardPublishedThreadData(ProfilerHotPathState& state)
{
	std::vector<std::shared_ptr<ProfilerThreadBuffer>> buffers;
	{
		std::lock_guard<std::mutex> lock(state.registryMutex);
		buffers = state.registeredBuffers;
	}
	for (const std::shared_ptr<ProfilerThreadBuffer>& buffer : buffers)
	{
		// Session replacement is a protected control operation. Consume any
		// already-published old-session entries and counters so they cannot appear
		// in the first frame of the next recording.
		const std::uint64_t writeSequence = buffer->writeSequence.load(std::memory_order_acquire);
		buffer->readSequence.store(writeSequence, std::memory_order_release);
		buffer->queuedTasks.exchange(0u, std::memory_order_acq_rel);
		buffer->startedTasks.exchange(0u, std::memory_order_acq_rel);
		buffer->finishedTasks.exchange(0u, std::memory_order_acq_rel);
		buffer->droppedSamples.exchange(0u, std::memory_order_acq_rel);
		buffer->truncatedLabels.exchange(0u, std::memory_order_acq_rel);
		buffer->lateSamples.exchange(0u, std::memory_order_acq_rel);
	}
	state.rejectedThreadBuffers.exchange(0u, std::memory_order_acq_rel);
	state.rejectedThreadBufferFrameToken.store(0u, std::memory_order_release);

	// A dead transient thread has no binding reference, leaving only the
	// registry and this inspection snapshot. Remove it after its ring is empty;
	// live fixed workers keep their TLS-owned reference and reuse the allocation.
	std::lock_guard<std::mutex> lock(state.registryMutex);
	state.registeredBuffers.erase(
		std::remove_if(
			state.registeredBuffers.begin(),
			state.registeredBuffers.end(),
			[&buffers](const std::shared_ptr<ProfilerThreadBuffer>& registeredBuffer) {
				const bool inspected = std::any_of(
					buffers.begin(),
					buffers.end(),
					[&registeredBuffer](const std::shared_ptr<ProfilerThreadBuffer>& candidate) {
						return candidate.get() == registeredBuffer.get();
					});
				return inspected
					&& registeredBuffer.use_count() == 2u
					&& registeredBuffer->readSequence.load(std::memory_order_acquire)
						== registeredBuffer->writeSequence.load(std::memory_order_acquire);
			}),
		state.registeredBuffers.end());
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

ProfilerRenderOptimizationViewSummary MakeProfilerRenderOptimizationViewSummary(
	const RenderStatsSnapshot& stats)
{
	ProfilerRenderOptimizationViewSummary result;
	result.viewType = static_cast<std::uint32_t>(stats.viewType);
	if (!stats.valid || stats.optimization.valid == 0U)
	{
		return result;
	}

	const RenderOptimizationSummary& source = stats.optimization;
	result.frameGeneration = source.frameGeneration;
	result.topologyGeneration = source.topologyGeneration;
	result.viewGeneration = source.viewGeneration;
	result.rendererGeneration = source.rendererGeneration;
	result.topologyBuildNanoseconds = source.topologyBuildNanoseconds;
	result.dynamicMaterializationNanoseconds = source.dynamicMaterializationNanoseconds;
	result.instanceStreamUploadNanoseconds = source.instanceStreamUploadNanoseconds;
	result.groupedCommandRecordingNanoseconds = source.groupedCommandRecordingNanoseconds;
	result.visibleInstanceCount = source.visibleInstanceCount;
	result.groupedInstanceCount = source.groupedInstanceCount;
	result.fallbackInstanceCount = source.fallbackInstanceCount;
	result.groupedSubmissionCount = source.groupedSubmissionCount;
	result.fallbackSubmissionCount = source.fallbackSubmissionCount;
	result.avoidedSubmissionCount = source.avoidedSubmissionCount;
	result.instanceStreamBytes = source.instanceStreamBytes;
	result.instanceStreamCapacityBytes = source.instanceStreamCapacityBytes;
	result.topologyReuseHits = source.topologyReuseHits;
	result.topologyInvalidations = source.topologyInvalidations;
	result.viewEvidenceReuseHits = source.viewEvidenceReuseHits;
	result.viewEvidenceInvalidations = source.viewEvidenceInvalidations;
	result.rejectedIncompleteKeyCount = source.rejectedIncompleteKeyCount;
	result.rejectedUnsupportedVariantCount = source.rejectedUnsupportedVariantCount;
	result.rejectedOutOfRangeCount = source.rejectedOutOfRangeCount;
	result.rejectedStaleGenerationCount = source.rejectedStaleGenerationCount;
	result.fallbackAllocationFailureCount = source.fallbackAllocationFailureCount;
	result.fallbackRecoveryCount = source.fallbackRecoveryCount;
	result.valid = true;
	return result;
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

ProfilerAnimationOptimizationEvidence MakeAnimationOptimizationEvidence(
	const TerraEngine::Animation::AnimationDiagnosticsSnapshot& snapshot)
{
	const TerraEngine::Animation::AnimationDiagnosticsCounters& counters = snapshot.counters;
	ProfilerAnimationOptimizationEvidence evidence;
	evidence.frameIndex = snapshot.frameIndex;
	evidence.candidateGeneration = snapshot.candidateGeneration;
	evidence.acceptedGeneration = snapshot.acceptedGeneration;
	evidence.rejectedGeneration = snapshot.rejectedGeneration;
	evidence.topologyGeneration = snapshot.topologyGeneration;
	evidence.presentationGeneration = snapshot.presentationGeneration;
	evidence.rendererGeneration = snapshot.rendererGeneration;
	evidence.sceneGeneration = snapshot.sceneGeneration;
	evidence.activeAnimators = counters.activeAnimators;
	evidence.suspendedAnimators = counters.suspendedAnimators;
	evidence.compactEligibleAnimators = counters.compactEligibleAnimators;
	evidence.compactProcessedAnimators = counters.compactProcessedAnimators;
	evidence.completeCapturedAnimators = counters.completeCapturedAnimators;
	evidence.completeProcessedAnimators = counters.completeProcessedAnimators;
	evidence.compactToCompleteTransitions = counters.compactToCompleteTransitions;
	evidence.completeToCompactTransitions = counters.completeToCompactTransitions;
	evidence.retainedTopologyHits = counters.retainedTopologyHits;
	evidence.retainedTopologyMisses = counters.retainedTopologyMisses;
	evidence.compactPlanGroups = counters.compactPlanGroups;
	evidence.compactDispatches = counters.compactDispatches;
	evidence.rejectedCandidateCount = counters.rejectedCandidateCount;
	static_assert(evidence.fallbackReasonCounts.size() == counters.fallbackReasonCounts.size());
	std::copy(counters.fallbackReasonCounts.begin(), counters.fallbackReasonCounts.end(),
		evidence.fallbackReasonCounts.begin());
	evidence.timingNanoseconds = {
		MillisecondsToNanoseconds(snapshot.timings.compactCaptureMs),
		MillisecondsToNanoseconds(snapshot.timings.compactEvaluationMs),
		MillisecondsToNanoseconds(snapshot.timings.compactPublicationMs),
		MillisecondsToNanoseconds(snapshot.timings.completeCaptureMs),
		MillisecondsToNanoseconds(snapshot.timings.completeEvaluationMs),
		MillisecondsToNanoseconds(snapshot.timings.completePublicationMs),
		MillisecondsToNanoseconds(snapshot.timings.topologyBuildMs),
		MillisecondsToNanoseconds(snapshot.timings.topologyRefreshMs)};
	evidence.valid = true;
	return evidence;
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

Profiler::Profiler()
	: hotPath(std::make_unique<ProfilerHotPathState>())
{
}

Profiler::~Profiler() = default;

// Profiler::StartRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::StartRecording(const ProfilerCaptureMetadata& metadata, const ProfilerCaptureSettings& requestedSettings)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Stop hot producers before replacing the capture document. An in-flight
	// scope will fail its generation check and be counted instead of writing into
	// the new session.
	frameRecordingActive.store(false, std::memory_order_release);
	markerRecordingActive.store(false, std::memory_order_release);
	taskRecordingActive.store(false, std::memory_order_release);
	hotPath->activeFrameToken.store(0u, std::memory_order_release);
	hotPath->sessionGeneration.fetch_add(1u, std::memory_order_acq_rel);
	hotPath->excludedEditorNanoseconds.store(0, std::memory_order_release);
	hotPath->nextSampleId.store(1u, std::memory_order_release);
	DiscardPublishedThreadData(*hotPath);
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
	retainedFrames.clear();
	document.sourcePath.reset();
	currentFrame.reset();
	selectedFrameIndex.reset();
	sessionStart = std::chrono::steady_clock::now();
	nextFrameIndex = 0u;
	queuedTaskCount = 0u;
	startedTaskCount = 0u;
	finishedTaskCount = 0u;
	droppedSampleCount = 0u;
	truncatedSampleLabelCount = 0u;
	lateSampleCount = 0u;
	lastError.clear();
	statusText = "Recording";
	state = ProfilerSessionState::Recording;
	UpdateFastRecordingFlagsLocked();
	snapshotRevision.fetch_add(1u, std::memory_order_release);
}

// Profiler::PauseRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::PauseRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state == ProfilerSessionState::Recording)
	{
		frameRecordingActive.store(false, std::memory_order_release);
		markerRecordingActive.store(false, std::memory_order_release);
		taskRecordingActive.store(false, std::memory_order_release);
		FinalizePartialFrameLocked();
		state = ProfilerSessionState::Paused;
		statusText = "Paused";
		UpdateFastRecordingFlagsLocked();
		snapshotRevision.fetch_add(1u, std::memory_order_release);
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
		snapshotRevision.fetch_add(1u, std::memory_order_release);
	}
}

// Profiler::FinishRecording is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ProfilerCaptureDocument Profiler::FinishRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	frameRecordingActive.store(false, std::memory_order_release);
	markerRecordingActive.store(false, std::memory_order_release);
	taskRecordingActive.store(false, std::memory_order_release);
	FinalizePartialFrameLocked();

	document.metadata.savedAt = MakeReadableTimestamp();
	document.metadata.settings.captureMode = ProfilerCaptureMode::FinalizingToArtifact;
	state = ProfilerSessionState::Finalizing;
	statusText = "Finalizing capture";
	UpdateFastRecordingFlagsLocked();
	snapshotRevision.fetch_add(1u, std::memory_order_release);
	ProfilerCaptureDocument snapshot = document;
	snapshot.frames.assign(retainedFrames.begin(), retainedFrames.end());
	MaterializeAnimationDiagnosticsMetrics(snapshot);
	return snapshot;
}

// Profiler::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	frameRecordingActive.store(false, std::memory_order_release);
	markerRecordingActive.store(false, std::memory_order_release);
	taskRecordingActive.store(false, std::memory_order_release);
	hotPath->activeFrameToken.store(0u, std::memory_order_release);
	hotPath->sessionGeneration.fetch_add(1u, std::memory_order_acq_rel);
	hotPath->excludedEditorNanoseconds.store(0, std::memory_order_release);
	DiscardPublishedThreadData(*hotPath);
	document = {};
	retainedFrames.clear();
	currentFrame.reset();
	selectedFrameIndex.reset();
	queuedTaskCount = 0u;
	startedTaskCount = 0u;
	finishedTaskCount = 0u;
	droppedSampleCount = 0u;
	truncatedSampleLabelCount = 0u;
	lateSampleCount = 0u;
	state = ProfilerSessionState::Idle;
	statusText = "Idle";
	lastError.clear();
	UpdateFastRecordingFlagsLocked();
	snapshotRevision.fetch_add(1u, std::memory_order_release);
}

// Profiler::LoadDocument is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::LoadDocument(const ProfilerCaptureDocument& loadedDocument)
{
	ProfilerCaptureDocument retainedDocument = loadedDocument;
	LoadDocument(std::move(retainedDocument));
}

// Profiler::LoadDocument adopts a completed capture from an asynchronous store
// without retaining a second copy of its potentially unbounded frame history.
void Profiler::LoadDocument(ProfilerCaptureDocument&& loadedDocument)
{
	std::lock_guard<std::mutex> lock(mutex);
	frameRecordingActive.store(false, std::memory_order_release);
	markerRecordingActive.store(false, std::memory_order_release);
	taskRecordingActive.store(false, std::memory_order_release);
	hotPath->activeFrameToken.store(0u, std::memory_order_release);
	hotPath->sessionGeneration.fetch_add(1u, std::memory_order_acq_rel);
	hotPath->excludedEditorNanoseconds.store(0, std::memory_order_release);
	DiscardPublishedThreadData(*hotPath);
	document = std::move(loadedDocument);
	retainedFrames.clear();
	retainedFrames.insert(
		retainedFrames.end(),
		std::make_move_iterator(document.frames.begin()),
		std::make_move_iterator(document.frames.end()));
	// Capture readers are tolerant of older or recovered artifacts, but every
	// presentation API promises chronological frame-index order. Normalize once
	// on load so the Profiler window can use its no-sort fast path safely.
	std::stable_sort(retainedFrames.begin(), retainedFrames.end(), [](const ProfilerFrameRecord& left, const ProfilerFrameRecord& right) {
		return left.frameIndex < right.frameIndex;
	});
	document.frames.clear();
	settings = document.metadata.settings;
	settings.captureMode = ProfilerCaptureMode::LoadedReadOnly;
	currentFrame.reset();
	selectedFrameIndex = retainedFrames.empty() ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{retainedFrames.front().frameIndex};
	state = ProfilerSessionState::Loaded;
	statusText = "Loaded capture";
	lastError.clear();
	UpdateFastRecordingFlagsLocked();
	snapshotRevision.fetch_add(1u, std::memory_order_release);
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
		frameRecordingActive.store(false, std::memory_order_release);
		markerRecordingActive.store(false, std::memory_order_release);
		taskRecordingActive.store(false, std::memory_order_release);
		hotPath->activeFrameToken.store(0u, std::memory_order_release);
		hotPath->excludedEditorNanoseconds.store(0, std::memory_order_release);
		currentFrame.reset();
		statusText = "Recording (waiting for Play Mode)";
		UpdateFastRecordingFlagsLocked();
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame.has_value())
	{
		MergeThreadSamplesForFrameLocked(
			hotPath->sessionGeneration.load(std::memory_order_acquire),
			hotPath->activeFrameToken.load(std::memory_order_acquire));
		currentFrame->complete = false;
		AggregateSamplesForFrame(*currentFrame);
		CommitCurrentFrameLocked();
	}

	const std::int64_t frameStartNanoseconds = SteadyNowNanoseconds();
	hotPath->excludedEditorNanoseconds.store(0, std::memory_order_release);
	ProfilerFrameRecord frame;
	frame.frameIndex = nextFrameIndex++;
	frame.frameStartMs = NowMs();
	frame.complete = true;
	frame.moduleSummaries = MakeDefaultModuleSummaries(settings);
	// Tokens never repeat across recording sessions. This lets the lock-free
	// counter merge reject a late producer from an earlier Clear/Start cycle.
	const std::uint64_t frameToken =
		hotPath->nextFrameToken.fetch_add(1u, std::memory_order_relaxed);
	currentFrame = std::move(frame);
	queuedTaskCount = 0u;
	startedTaskCount = 0u;
	finishedTaskCount = 0u;
	droppedSampleCount = 0u;
	truncatedSampleLabelCount = 0u;
	lateSampleCount = 0u;
	hotPath->activeFrameStartNanoseconds.store(frameStartNanoseconds, std::memory_order_release);
	hotPath->activeFrameToken.store(frameToken, std::memory_order_release);
	statusText = "Recording";
	UpdateFastRecordingFlagsLocked();
}

// Profiler::EndFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndFrame()
{
	// External producers publish immutable snapshots independently of the
	// profiler document. Capture those values before taking the profiler mutex so
	// a window snapshot or FinishRecording call never waits on subsystem copies.
	if(!frameRecordingActive.load(std::memory_order_acquire))
	{
		return;
	}
	const auto physicsSnapshot = TerraEngine::Physics::AcquirePhysicsDiagnosticsSnapshot();
	std::vector<WorkloadExecutionRecord> workloadExecutionSnapshot =
		TerraEngine::Scheduling::LatestFrameWorkloadExecutionRecordsForProfiling();
	thread_local std::vector<WorkerTelemetryRecord> workerCounterScratch;
	workerCounterScratch.clear();
	workerCounterScratch.reserve(ThreadPool::instance().getThreadCount());
	ThreadPool::instance().VisitTelemetryCounters(
		[](const ThreadPoolWorkerTelemetry& worker)
		{
			workerCounterScratch.push_back({
				static_cast<std::uint32_t>(worker.workerIndex),
				worker.latestQueueDepthAtStart,
				worker.dispatchCount,
				worker.usefulItemCount,
				AvailableTelemetryDuration(worker.runNanoseconds),
				AvailableTelemetryDuration(worker.waitNanoseconds),
				AvailableTelemetryDuration(worker.idleNanoseconds)});
		});
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics =
		TerraEngine::Renderer::LatestRenderDiagnostics();

	std::lock_guard<std::mutex> lock(mutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	// The scheduler has completed its worker-safe work before this protected
	// boundary. Disarm producers first, then consume their release-published
	// rings without making scope begin/end wait on the document mutex.
	frameRecordingActive.store(false, std::memory_order_release);
	markerRecordingActive.store(false, std::memory_order_release);
	taskRecordingActive.store(false, std::memory_order_release);
	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	const std::int64_t frameStartNanoseconds =
		hotPath->activeFrameStartNanoseconds.load(std::memory_order_acquire);
	const std::int64_t wallDurationNanoseconds =
		std::max<std::int64_t>(0, SteadyNowNanoseconds() - frameStartNanoseconds);
	const std::int64_t excludedEditorNanoseconds = std::clamp<std::int64_t>(
		hotPath->excludedEditorNanoseconds.load(std::memory_order_acquire),
		0,
		wallDurationNanoseconds);
	// Profiler frame statistics intentionally describe the runnable game/engine
	// portion of the frame. Editor UI time is internal subtraction bookkeeping.
	currentFrame->durationMs =
		NanosecondsToMilliseconds(wallDurationNanoseconds - excludedEditorNanoseconds);
	if(currentFrame->framePublication.has_value())
	{
		// Producers report disjoint known spans. The profiler owns the complete
		// frame clock, so it fills the final bucket with the explicit uninstrumented
		// remainder instead of pretending missing work cost zero.
		auto& buckets = currentFrame->framePublication->disjointTimingNanoseconds;
		const std::uint64_t engineNanoseconds = static_cast<std::uint64_t>(
			wallDurationNanoseconds - excludedEditorNanoseconds);
		const std::uint64_t frameBegin = static_cast<std::uint64_t>(
			std::max<std::int64_t>(0, frameStartNanoseconds));
		const std::uint64_t frameEnd = frameBegin + engineNanoseconds;
		// Animation partitions may execute in parallel. Their scheduler endpoints
		// are merged into wall coverage once; worker CPU sums remain nested detail.
		buckets[3] = MergeWorkloadWallTimeNanoseconds(
			workloadExecutionSnapshot,
			{"PlanAnimationEvaluation", "FinalizeAnimationWorkPlan",
				"EvaluateCompleteAnimation", "EvaluateCompactGpuVisualAnimation",
				"SealAnimationAuthority", "CommitPublishedAnimationEvaluationCache"},
			frameBegin, frameEnd);
		const std::uint64_t animationPublicationNanoseconds =
			MergeWorkloadWallTimeNanoseconds(
				workloadExecutionSnapshot,
				{"PublishAnimationAuthority", "FinalizePublishedAnimationAuthority"},
				frameBegin, frameEnd);
		buckets[4] = std::min(engineNanoseconds,
			buckets[4] + std::min(animationPublicationNanoseconds,
				engineNanoseconds - std::min(engineNanoseconds, buckets[4])));
		std::uint64_t measuredNanoseconds = 0u;
		for(std::size_t index = 0u; index + 1u < buckets.size(); ++index)
		{
			if(buckets[index] > engineNanoseconds - std::min(
				engineNanoseconds, measuredNanoseconds))
			{
				measuredNanoseconds = engineNanoseconds;
				break;
			}
			measuredNanoseconds += buckets[index];
		}
		buckets.back() = engineNanoseconds - std::min(
			engineNanoseconds, measuredNanoseconds);
	}
	AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
		"engine_cpu",
		"Engine CPU",
		ProfilerTimingStreamScope::EngineCpu,
		ProfilerTimingStreamSource::ProfilerClock,
		ProfilerTimingStreamStatus::Valid,
		currentFrame->durationMs));
	MergeThreadSamplesForFrameLocked(sessionGeneration, frameToken);
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
	if (physicsSnapshot)
	{
		const auto& physics = physicsSnapshot;
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
	currentFrame->workloadExecutions = std::move(workloadExecutionSnapshot);
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
	currentFrame->workerTelemetry.assign(workerCounterScratch.begin(), workerCounterScratch.end());
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
	if (settings.detailLevel == ProfilerCaptureDetailLevel::Aggregate)
	{
		// Explicit Aggregate captures intentionally disable optional markers.
		// Keep one fixed root sample so a valid frame timing never produces an
		// empty timeline, while system and worker scope contention remains absent.
		ProfilerSample frameRoot;
		frameRoot.sampleId = hotPath->nextSampleId.fetch_add(1u, std::memory_order_relaxed);
		frameRoot.moduleId = ProfilerModuleId::CpuEngine;
		frameRoot.name = "Engine Frame";
		frameRoot.threadLabel = "main";
		frameRoot.durationMs = currentFrame->durationMs;
		frameRoot.retainInAggregate = true;
		currentFrame->samples.push_back(std::move(frameRoot));
	}
	AggregateSamplesForFrame(*currentFrame);
	CommitCurrentFrameLocked();
	hotPath->activeFrameToken.store(0u, std::memory_order_release);
	UpdateFastRecordingFlagsLocked();
	// Publish after the complete frame is immutable so UI readers never need to
	// contend with marker producers for an in-progress sample vector.
	snapshotRevision.fetch_add(1u, std::memory_order_release);
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
	ProfilerAnimationOptimizationEvidence optimization =
		MakeAnimationOptimizationEvidence(snapshot);
	if (ValidateProfilerAnimationOptimizationEvidence(optimization))
	{
		currentFrame->animationOptimization = std::move(optimization);
	}
	// Never mix a valid timing frame with partial or stale optimization counts.
	// Ordinary animation telemetry remains useful when a producer has not yet
	// published the complete generation-coherent optimization envelope.
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
	const bool latestTimingActive = latestEngineFrameStartNanoseconds != 0;
	if (latestTimingActive && latestEditorWorkDepth++ == 0u)
	{
		latestEditorWorkStartNanoseconds = SteadyNowNanoseconds();
	}

	if (!frameRecordingActive.load(std::memory_order_acquire))
	{
		return latestTimingActive;
	}

	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (frameToken == 0u)
	{
		return latestTimingActive;
	}

	ProfilerThreadBuffer* buffer = AcquireThreadBuffer(*hotPath, frameToken);
	if (!buffer)
	{
		return latestTimingActive;
	}
	PrepareThreadBufferForFrame(*buffer, sessionGeneration, frameToken);
	if (buffer->editorWorkDepth++ == 0u)
	{
		buffer->editorWorkStartNanoseconds = SteadyNowNanoseconds();
		if (buffer->openSampleCount != 0u
			&& std::strcmp(
				buffer->openSamples[buffer->openSampleCount - 1u].sample.taskLabel.data(),
				"EditorSystem") == 0)
		{
			// The immediate enclosing scheduler scope represents EditorSystem itself.
			// Drop that wrapper completely; its parent remains an engine sample whose
			// duration is reduced by the excluded editor interval below.
			buffer->openSamples[buffer->openSampleCount - 1u].fullyExcluded = true;
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
	if (latestEditorWorkDepth != 0u && --latestEditorWorkDepth == 0u)
	{
		latestExcludedEditorNanoseconds += std::max<std::int64_t>(
			0,
			SteadyNowNanoseconds() - latestEditorWorkStartNanoseconds);
	}

	if (t_profilerThreadBinding.owner != hotPath.get() || !t_profilerThreadBinding.buffer)
	{
		return;
	}
	ProfilerThreadBuffer& buffer = *t_profilerThreadBinding.buffer;
	if (buffer.editorWorkDepth == 0u || --buffer.editorWorkDepth != 0u)
	{
		return;
	}
	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (buffer.boundSessionGeneration != sessionGeneration
		|| buffer.boundFrameToken != frameToken
		|| frameToken == 0u)
	{
		buffer.lateSamples.fetch_add(1u, std::memory_order_relaxed);
		return;
	}
	const std::int64_t durationNanoseconds =
		std::max<std::int64_t>(0, SteadyNowNanoseconds() - buffer.editorWorkStartNanoseconds);
	hotPath->excludedEditorNanoseconds.fetch_add(durationNanoseconds, std::memory_order_relaxed);
}

void Profiler::RecordFramePublicationEvidence(const ProfilerFramePublicationEvidence& evidence)
{
	if (!frameRecordingActive.load(std::memory_order_acquire)
		|| !ValidateProfilerFramePublicationEvidence(evidence))
	{
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	if (state == ProfilerSessionState::Recording && currentFrame.has_value())
	{
		currentFrame->framePublication = evidence;
	}
}

void Profiler::RecordEditorStages(std::span<const ProfilerEditorStageTiming> stages)
{
	if(!frameRecordingActive.load(std::memory_order_acquire) || stages.empty())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if(state != ProfilerSessionState::Recording || !currentFrame.has_value())
	{
		return;
	}

	// EditorSystem and MainEditorSubsystem publish fixed named regions in two
	// bounded batches. Thirty-two records keep every native window diagnosable
	// without allowing an accidental producer loop to inflate retained frames.
	constexpr std::size_t kMaximumEditorStageRecordsPerBatch = 32u;
	std::size_t published = 0u;
	for(const ProfilerEditorStageTiming& timing : stages)
	{
		if(published >= kMaximumEditorStageRecordsPerBatch)
		{
			break;
		}
		if(timing.stageName == nullptr || timing.stageName[0] == '\0')
		{
			continue;
		}
		PerformanceStageRecord stage;
		stage.domain = PerformanceStageDomain::Editor;
		stage.stageName = timing.stageName;
		stage.duration = AvailableTelemetryDuration(timing.durationNanoseconds);
		stage.workloadName = "EditorSystem";
		stage.workItemCount = timing.workItemCount;
		if(ValidatePerformanceStageRecord(stage).valid)
		{
			currentFrame->performanceStages.push_back(std::move(stage));
			++published;
		}
	}
}

// Profiler::BeginSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::BeginSample(const char* name, ProfilerModuleId moduleId, const char* taskLabel)
{
	if (!markerRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}

	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (frameToken == 0u)
	{
		return false;
	}

	ProfilerThreadBuffer* buffer = AcquireThreadBuffer(*hotPath, frameToken);
	if (!buffer)
	{
		return false;
	}
	PrepareThreadBufferForFrame(*buffer, sessionGeneration, frameToken);
	if (buffer->editorWorkDepth != 0u)
	{
		// Editor work is outside the game profiler. Do not let nested ImGui,
		// managed editor, or editor-presentation scopes enter CPU Engine tables.
		return false;
	}
	if (buffer->openSampleCount >= kProfilerMaximumOpenSampleDepth)
	{
		buffer->droppedSamples.fetch_add(1u, std::memory_order_relaxed);
		return false;
	}
	const std::uint64_t writeSequence =
		buffer->writeSequence.load(std::memory_order_relaxed);
	const std::uint64_t readSequence =
		buffer->readSequence.load(std::memory_order_acquire);
	const std::uint64_t reservedAncestorSlots =
		static_cast<std::uint64_t>(buffer->openSampleCount + 1u);
	if (writeSequence - readSequence + reservedAncestorSlots
		> kProfilerThreadSampleCapacity)
	{
		// Every accepted child reserves enough ring space for itself and all
		// still-open ancestors. A saturated ring can therefore drop a new subtree
		// atomically, but never retain a child whose parent cannot be published.
		buffer->droppedSamples.fetch_add(1u, std::memory_order_relaxed);
		return false;
	}

	OpenProfilerSample& opened = buffer->openSamples[buffer->openSampleCount];
	opened = {};
	opened.sample.sessionGeneration = sessionGeneration;
	opened.sample.frameToken = frameToken;
	opened.sample.sampleId = hotPath->nextSampleId.fetch_add(1u, std::memory_order_relaxed);
	opened.sample.parentSampleId = buffer->openSampleCount == 0u
		? 0u
		: buffer->openSamples[buffer->openSampleCount - 1u].sample.sampleId;
	opened.sample.moduleId = moduleId;
	const bool nameTruncated =
		CopyInlineText(opened.sample.name, name, "Unnamed Sample");
	const bool taskTruncated =
		CopyInlineText(opened.sample.taskLabel, taskLabel, "");
	if (nameTruncated || taskTruncated)
	{
		buffer->truncatedLabels.fetch_add(1u, std::memory_order_relaxed);
	}
	const std::int64_t frameStartNanoseconds =
		hotPath->activeFrameStartNanoseconds.load(std::memory_order_acquire);
	const std::int64_t excludedEditorNanoseconds =
		hotPath->excludedEditorNanoseconds.load(std::memory_order_acquire);
	opened.sample.startNanoseconds = std::max<std::int64_t>(
		0,
		SteadyNowNanoseconds() - frameStartNanoseconds - excludedEditorNanoseconds);
	opened.sample.depth = static_cast<std::uint32_t>(buffer->openSampleCount);
	++buffer->openSampleCount;
	return true;
}

// Profiler::EndSample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::EndSample()
{
	if (t_profilerThreadBinding.owner != hotPath.get() || !t_profilerThreadBinding.buffer)
	{
		return;
	}

	ProfilerThreadBuffer& buffer = *t_profilerThreadBinding.buffer;
	if (buffer.openSampleCount == 0u)
	{
		return;
	}

	OpenProfilerSample opened = buffer.openSamples[--buffer.openSampleCount];
	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (opened.sample.sessionGeneration != sessionGeneration
		|| opened.sample.frameToken != frameToken
		|| frameToken == 0u)
	{
		buffer.lateSamples.fetch_add(1u, std::memory_order_relaxed);
		return;
	}
	const std::int64_t frameStartNanoseconds =
		hotPath->activeFrameStartNanoseconds.load(std::memory_order_acquire);
	const std::int64_t excludedEditorNanoseconds =
		hotPath->excludedEditorNanoseconds.load(std::memory_order_acquire);
	const std::int64_t gameEndNanoseconds = std::max<std::int64_t>(
		0,
		SteadyNowNanoseconds() - frameStartNanoseconds - excludedEditorNanoseconds);
	opened.sample.durationNanoseconds =
		std::max<std::int64_t>(0, gameEndNanoseconds - opened.sample.startNanoseconds);
	if (opened.fullyExcluded)
	{
		return;
	}
	PublishCompletedSample(buffer, std::move(opened.sample));
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

	ProfilerRenderOptimizationEvidence renderEvidence;
	renderEvidence.views[0] = MakeProfilerRenderOptimizationViewSummary(gameStats);
	renderEvidence.views[1] = MakeProfilerRenderOptimizationViewSummary(sceneStats);
	// Do not publish half-coherent renderer counters. A malformed producer leaves
	// this frame without optimization evidence instead of presenting plausible
	// zeroes or mixing generations from different views.
	if (ValidateProfilerRenderOptimizationEvidence(renderEvidence))
	{
		currentFrame->renderOptimization = renderEvidence;
	}

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

	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (frameToken == 0u)
	{
		return;
	}
	ProfilerThreadBuffer* buffer = AcquireThreadBuffer(*hotPath, frameToken);
	if (!buffer)
	{
		return;
	}
	PrepareThreadBufferForFrame(*buffer, sessionGeneration, frameToken);
	buffer->queuedTasks.fetch_add(1u, std::memory_order_relaxed);
}

// Profiler::RecordTaskStarted is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Profiler::RecordTaskStarted(const char* taskLabel)
{
	if (!taskRecordingActive.load(std::memory_order_acquire))
	{
		return false;
	}

	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (frameToken == 0u)
	{
		return false;
	}
	ProfilerThreadBuffer* buffer = AcquireThreadBuffer(*hotPath, frameToken);
	if (!buffer)
	{
		return false;
	}
	PrepareThreadBufferForFrame(*buffer, sessionGeneration, frameToken);
	buffer->startedTasks.fetch_add(1u, std::memory_order_relaxed);
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

	const std::uint64_t sessionGeneration = hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken = hotPath->activeFrameToken.load(std::memory_order_acquire);
	if (frameToken == 0u)
	{
		return;
	}
	ProfilerThreadBuffer* buffer = AcquireThreadBuffer(*hotPath, frameToken);
	if (!buffer)
	{
		return;
	}
	PrepareThreadBufferForFrame(*buffer, sessionGeneration, frameToken);
	buffer->finishedTasks.fetch_add(1u, std::memory_order_relaxed);
}

// Profiler::SelectFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::SelectFrame(std::uint64_t frameIndex)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (selectedFrameIndex != frameIndex)
	{
		selectedFrameIndex = frameIndex;
		snapshotRevision.fetch_add(1u, std::memory_order_release);
	}
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
		snapshot.frames.assign(retainedFrames.begin(), retainedFrames.end());
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
	frames.reserve(retainedFrames.size() + (currentFrame.has_value() ? 1u : 0u));
	auto appendFrame = [&frames](const ProfilerFrameRecord& frame) {
		ProfilerFrameListItem item;
		item.frameIndex = frame.frameIndex;
		item.durationMs = frame.durationMs;
		item.complete = frame.complete;
		frames.push_back(item);
	};
	for (const ProfilerFrameRecord& frame : retainedFrames)
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
		frames.assign(retainedFrames.begin(), retainedFrames.end());
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
			if (!retainedFrames.empty())
			{
				snapshot = retainedFrames.back();
			}
			else if (currentFrame.has_value())
			{
				snapshot = *currentFrame;
			}
		}
		else
		{
			for (const ProfilerFrameRecord& frame : retainedFrames)
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

bool Profiler::IsFrameRecordingActive() const noexcept
{
	return frameRecordingActive.load(std::memory_order_acquire);
}

std::uint64_t Profiler::GetSnapshotRevision() const noexcept
{
	return snapshotRevision.load(std::memory_order_acquire);
}

void Profiler::BeginLatestEngineFrameTiming() noexcept
{
	// Reset before sampling the start so a previous interrupted editor interval
	// can never leak into the next host frame.
	latestExcludedEditorNanoseconds = 0;
	latestEditorWorkStartNanoseconds = 0;
	latestEditorWorkDepth = 0u;
	latestEngineFrameStartNanoseconds = SteadyNowNanoseconds();
}

void Profiler::PublishLatestEngineFrameTiming() noexcept
{
	const std::int64_t nowNanoseconds = SteadyNowNanoseconds();
	if (latestEngineFrameStartNanoseconds == 0)
	{
		return;
	}
	if (latestEditorWorkDepth != 0u)
	{
		// Engine shutdown or an exception may interrupt the paired editor scope.
		// Close the outer interval here so the public clock remains conservative
		// and the next frame begins from a clean depth.
		latestExcludedEditorNanoseconds += std::max<std::int64_t>(
			0,
			nowNanoseconds - latestEditorWorkStartNanoseconds);
		latestEditorWorkDepth = 0u;
	}
	const std::int64_t wallNanoseconds = std::max<std::int64_t>(
		0,
		nowNanoseconds - latestEngineFrameStartNanoseconds);
	const std::int64_t excludedNanoseconds = std::clamp<std::int64_t>(
		latestExcludedEditorNanoseconds,
		0,
		wallNanoseconds);
	const double durationMs = NanosecondsToMilliseconds(
		wallNanoseconds - excludedNanoseconds);
	latestEngineFrameStartNanoseconds = 0;

	// The odd/even sequence makes the two atomic fields one coherent snapshot.
	latestEngineFrameSequence.fetch_add(1u, std::memory_order_acq_rel);
	latestEngineFrameDurationMs.store(durationMs, std::memory_order_relaxed);
	latestEngineFrameSequence.fetch_add(1u, std::memory_order_release);
}

ProfilerLatestEngineFrameTiming Profiler::GetLatestEngineFrameTiming() const noexcept
{
	ProfilerLatestEngineFrameTiming result;
	std::uint64_t sequence = 0u;
	for (;;)
	{
		sequence = latestEngineFrameSequence.load(std::memory_order_acquire);
		if ((sequence & 1u) != 0u)
		{
			continue;
		}
		result.durationMs = latestEngineFrameDurationMs.load(std::memory_order_relaxed);
		if (sequence == latestEngineFrameSequence.load(std::memory_order_acquire))
		{
			break;
		}
	}
	result.frameIndex = sequence / 2u;
	result.valid = result.frameIndex != 0u && std::isfinite(result.durationMs)
		&& result.durationMs >= 0.0;
	return result;
}

// Profiler::NowMs is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
double Profiler::NowMs() const
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sessionStart).count();
}

void Profiler::CommitCurrentFrameLocked()
{
	if (!currentFrame.has_value())
	{
		return;
	}

	// Move the string-heavy detailed record instead of duplicating every sample
	// allocation at the protected frame boundary.
	retainedFrames.push_back(std::move(*currentFrame));
	currentFrame.reset();

	// Zero means the user requested the complete capture. A non-zero value keeps
	// the existing rolling policy for explicitly bounded recording clients.
	const std::size_t maximumFrames = settings.maxFrames;
	while (maximumFrames > 0u && retainedFrames.size() > maximumFrames)
	{
		retainedFrames.pop_front();
	}

	if (settings.maxDurationSeconds <= 0.0 || retainedFrames.empty())
	{
		return;
	}

	// A deque makes duration-based rolling eviction proportional only to records
	// actually removed; surviving detailed frames are never shifted.
	const double cutoff = retainedFrames.back().frameStartMs - settings.maxDurationSeconds * 1000.0;
	while (!retainedFrames.empty() && retainedFrames.front().frameStartMs < cutoff)
	{
		retainedFrames.pop_front();
	}
}

void Profiler::FinalizePartialFrameLocked()
{
	if (!currentFrame.has_value())
	{
		hotPath->activeFrameToken.store(0u, std::memory_order_release);
		return;
	}

	const std::uint64_t sessionGeneration =
		hotPath->sessionGeneration.load(std::memory_order_acquire);
	const std::uint64_t frameToken =
		hotPath->activeFrameToken.load(std::memory_order_acquire);
	const std::int64_t frameStartNanoseconds =
		hotPath->activeFrameStartNanoseconds.load(std::memory_order_acquire);
	const std::int64_t nowNanoseconds = SteadyNowNanoseconds();
	std::int64_t interruptedEditorNanoseconds = 0;
	if (t_profilerThreadBinding.owner == hotPath.get()
		&& t_profilerThreadBinding.buffer)
	{
		ProfilerThreadBuffer& buffer = *t_profilerThreadBinding.buffer;
		if (buffer.boundSessionGeneration == sessionGeneration
			&& buffer.boundFrameToken == frameToken
			&& buffer.editorWorkDepth != 0u)
		{
			// Pause/Finish are invoked from EditorSystem's protected ImGui
			// boundary on this same thread. Close only its own TLS interval at the
			// action time; never inspect another producer's non-atomic stack state.
			interruptedEditorNanoseconds += std::max<std::int64_t>(
				0,
				nowNanoseconds - buffer.editorWorkStartNanoseconds);
			buffer.editorWorkDepth = 0u;
		}
	}
	hotPath->excludedEditorNanoseconds.fetch_add(
		interruptedEditorNanoseconds,
		std::memory_order_acq_rel);
	const std::int64_t wallDurationNanoseconds =
		std::max<std::int64_t>(0, nowNanoseconds - frameStartNanoseconds);
	const std::int64_t excludedEditorNanoseconds = std::clamp<std::int64_t>(
		hotPath->excludedEditorNanoseconds.load(std::memory_order_acquire),
		0,
		wallDurationNanoseconds);
	currentFrame->durationMs =
		NanosecondsToMilliseconds(wallDurationNanoseconds - excludedEditorNanoseconds);
	AddOrReplaceTimingStreamSample(*currentFrame, MakeTimingSample(
		"engine_cpu",
		"Engine CPU",
		ProfilerTimingStreamScope::EngineCpu,
		ProfilerTimingStreamSource::ProfilerClock,
		ProfilerTimingStreamStatus::Valid,
		currentFrame->durationMs));
	MergeThreadSamplesForFrameLocked(sessionGeneration, frameToken);
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

	// A toolbar action can interrupt the current protected editor frame. Preserve
	// the elapsed evidence but label it partial so percentiles and UI rows can
	// distinguish it from a naturally completed EndFrame record.
	currentFrame->complete = false;
	AggregateSamplesForFrame(*currentFrame);
	CommitCurrentFrameLocked();
	hotPath->activeFrameToken.store(0u, std::memory_order_release);
}

void Profiler::MergeThreadSamplesForFrameLocked(
	std::uint64_t sessionGeneration,
	std::uint64_t frameToken)
{
	if (!currentFrame.has_value() || frameToken == 0u)
	{
		return;
	}

	std::vector<std::shared_ptr<ProfilerThreadBuffer>> buffers;
	{
		// Thread enrollment is rare and separate from the scope hot path. Copy
		// strong references at the protected boundary so transient workers that
		// already exited still contribute their completed samples.
		std::lock_guard<std::mutex> registryLock(hotPath->registryMutex);
		buffers = hotPath->registeredBuffers;
	}

	if (hotPath->rejectedThreadBufferFrameToken.load(std::memory_order_acquire) == frameToken)
	{
		droppedSampleCount +=
			hotPath->rejectedThreadBuffers.exchange(0u, std::memory_order_acq_rel);
	}
	for (const std::shared_ptr<ProfilerThreadBuffer>& buffer : buffers)
	{
		if (buffer->counterFrameToken.load(std::memory_order_acquire) == frameToken)
		{
			queuedTaskCount += buffer->queuedTasks.exchange(0u, std::memory_order_acq_rel);
			startedTaskCount += buffer->startedTasks.exchange(0u, std::memory_order_acq_rel);
			finishedTaskCount += buffer->finishedTasks.exchange(0u, std::memory_order_acq_rel);
		}

		const std::uint64_t readSequence = buffer->readSequence.load(std::memory_order_relaxed);
		const std::uint64_t writeSequence = buffer->writeSequence.load(std::memory_order_acquire);
		currentFrame->samples.reserve(
			currentFrame->samples.size() + static_cast<std::size_t>(writeSequence - readSequence));
		for (std::uint64_t sequence = readSequence; sequence < writeSequence; ++sequence)
		{
			const BufferedProfilerSample& buffered =
				buffer->completedSamples[sequence % kProfilerThreadSampleCapacity];
			if (buffered.sessionGeneration != sessionGeneration || buffered.frameToken != frameToken)
			{
				++lateSampleCount;
				continue;
			}

			ProfilerSample sample;
			sample.sampleId = buffered.sampleId;
			sample.parentSampleId = buffered.parentSampleId;
			sample.moduleId = buffered.moduleId;
			sample.name = buffered.name.data();
			sample.threadLabel = buffer->threadLabel;
			sample.taskLabel = buffered.taskLabel.data();
			sample.startMs = std::min(
				currentFrame->durationMs,
				NanosecondsToMilliseconds(buffered.startNanoseconds));
			sample.durationMs = std::min(
				NanosecondsToMilliseconds(buffered.durationNanoseconds),
				std::max(0.0, currentFrame->durationMs - sample.startMs));
			sample.depth = buffered.depth;
			sample.complete = buffered.complete;
			currentFrame->samples.push_back(std::move(sample));
		}
		buffer->readSequence.store(writeSequence, std::memory_order_release);
		if (buffer->diagnosticFrameToken.load(std::memory_order_acquire) == frameToken)
		{
			droppedSampleCount += buffer->droppedSamples.exchange(0u, std::memory_order_acq_rel);
			truncatedSampleLabelCount += buffer->truncatedLabels.exchange(0u, std::memory_order_acq_rel);
			lateSampleCount += buffer->lateSamples.exchange(0u, std::memory_order_acq_rel);
		}
	}

	// Retire only buffers that were part of this merge, are now drained, and no
	// longer have a TLS owner. A thread that enrolled concurrently is absent from
	// the local snapshot and cannot be mistaken for an exited worker.
	std::lock_guard<std::mutex> registryLock(hotPath->registryMutex);
	hotPath->registeredBuffers.erase(
		std::remove_if(
			hotPath->registeredBuffers.begin(),
			hotPath->registeredBuffers.end(),
			[&buffers](const std::shared_ptr<ProfilerThreadBuffer>& registeredBuffer) {
				const bool merged = std::any_of(
					buffers.begin(),
					buffers.end(),
					[&registeredBuffer](const std::shared_ptr<ProfilerThreadBuffer>& candidate) {
						return candidate.get() == registeredBuffer.get();
					});
				return merged
					&& registeredBuffer.use_count() == 2u
					&& registeredBuffer->readSequence.load(std::memory_order_acquire)
						== registeredBuffer->writeSequence.load(std::memory_order_acquire);
			}),
		hotPath->registeredBuffers.end());
}

// Profiler::AggregateSamplesForFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::AggregateSamplesForFrame(ProfilerFrameRecord& frame) const
{
	for (ProfilerModuleSummary& module : frame.moduleSummaries)
	{
		// totalMs is CPU work rather than a sum of inclusive nodes. Nested samples
		// overlap by design, so summing every duration made a fully instrumented
		// hierarchy look progressively more expensive as detail was added.
		module.totalMs = CalculateSampleCoverageMs(frame.samples, module.moduleId, true);
		module.available = module.totalMs > 0.0;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ProfilerModuleSummary* engineCpu = FindModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine))
	{
		engineCpu->available = settings.cpuEngineEnabled;
		const double capturedWallMs = CalculateSampleCoverageMs(frame.samples, ProfilerModuleId::CpuEngine, false);
		AddOrReplaceMetric(*engineCpu, "Frame duration", frame.durationMs, "ms");
		AddOrReplaceMetric(*engineCpu, "Captured wall time", capturedWallMs, "ms");
		AddOrReplaceMetric(*engineCpu, "Captured CPU work", engineCpu->totalMs, "ms");
		AddOrReplaceMetric(*engineCpu, "Scope coverage", frame.durationMs > 0.0 ? capturedWallMs * 100.0 / frame.durationMs : 0.0, "%");
		AddOrReplaceMetric(*engineCpu, "Uncovered frame time", std::max(0.0, frame.durationMs - capturedWallMs), "ms");
		AddOrReplaceMetric(*engineCpu, "Engine sample count", static_cast<double>(std::count_if(frame.samples.begin(), frame.samples.end(), [](const ProfilerSample& sample) {
			return sample.moduleId == ProfilerModuleId::CpuEngine;
		})), "count");
		AddOrReplaceMetric(*engineCpu, "Profiler dropped scopes", static_cast<double>(droppedSampleCount), "count");
		AddOrReplaceMetric(*engineCpu, "Profiler truncated labels", static_cast<double>(truncatedSampleLabelCount), "count");
		AddOrReplaceMetric(*engineCpu, "Profiler late scopes", static_cast<double>(lateSampleCount), "count");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ProfilerModuleSummary* tasks = FindModule(frame.moduleSummaries, ProfilerModuleId::TaskExecution))
	{
		tasks->available = settings.taskExecutionEnabled;
		const double capturedWallMs = CalculateSampleCoverageMs(frame.samples, ProfilerModuleId::TaskExecution, false);
		AddOrReplaceMetric(*tasks, "Captured wall time", capturedWallMs, "ms");
		AddOrReplaceMetric(*tasks, "Parallel CPU work", tasks->totalMs, "ms");
		AddOrReplaceMetric(*tasks, "Queued tasks", static_cast<double>(queuedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Started tasks", static_cast<double>(startedTaskCount), "count");
		AddOrReplaceMetric(*tasks, "Finished tasks", static_cast<double>(finishedTaskCount), "count");
	}

	const auto sampleOrder = [](const ProfilerSample& left, const ProfilerSample& right) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (left.startMs == right.startMs)
		{
			return left.depth < right.depth;
		}
		return left.startMs < right.startMs;
	};
	// A single producer, and many already time-ordered multi-producer frames, need
	// no sort. The linear check avoids an O(n log n) reorder while retaining the
	// exact stable display contract when buffers were merged out of time order.
	if(!std::is_sorted(frame.samples.begin(), frame.samples.end(), sampleOrder))
	{
		std::sort(frame.samples.begin(), frame.samples.end(), sampleOrder);
	}
}

// Profiler::UpdateFastRecordingFlagsLocked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Profiler::UpdateFastRecordingFlagsLocked()
{
	// An armed recording outside Play Mode owns no current frame. Keeping all
	// fast flags false there prevents editor-preview instrumentation from paying
	// profiler synchronization costs or leaking counters into the next play frame.
	const bool frameActive = state == ProfilerSessionState::Recording
		&& currentFrame.has_value()
		&& hotPath->activeFrameToken.load(std::memory_order_acquire) != 0u;
	const bool retainOptionalDetails = frameActive
		&& (settings.detailLevel == ProfilerCaptureDetailLevel::Detailed
			|| (settings.detailLevel == ProfilerCaptureDetailLevel::Sampled
				&& currentFrame->frameIndex % std::max(1u, settings.sampleIntervalFrames) == 0u));
	frameRecordingActive.store(frameActive, std::memory_order_release);
	// Optional scopes span every worker and contend on the profiler's capture
	// buffers. Apply the selected detail level during collection instead of
	// filling those bounded rings on frames whose hierarchy will not be retained.
	markerRecordingActive.store(retainOptionalDetails && settings.recordMarkers, std::memory_order_release);
	// Queue/start/finish counters are compact per-thread atomics and remain useful
	// every frame. Task scope strings still follow marker sampling above.
	taskRecordingActive.store(frameActive && settings.taskExecutionEnabled, std::memory_order_release);
}
}
