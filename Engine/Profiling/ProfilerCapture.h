// ProfilerCapture.h — Contains declarations for the Profiler Capture file-specific types, helper functions, and module integration code used by runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#pragma once

#include "Profiling/PerformanceTelemetry.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Profiling
{
// Complete journal frames may carry an opaque, fixed-record sample hand-off
// between the protected frame boundary and the serialized journal worker. The
// concrete storage stays private to Profiler.cpp so capture readers and writers
// cannot accidentally depend on producer-ring representation details.
struct ProfilerDeferredSampleStorage;

// ProfilerSessionState names the supported states explicitly so serialization and switches stay readable.
enum class ProfilerSessionState : std::uint8_t
{
	Idle,
	Recording,
	Paused,
	Finalizing,
	Saving,
	OpeningSavedCapture,
	Loaded,
	Error
};

// ProfilerModuleId names the supported states explicitly so serialization and switches stay readable.
enum class ProfilerModuleId : std::uint8_t
{
	CpuEngine,
	TaskExecution
};

// ProfilerCaptureMode names the supported states explicitly so serialization and switches stay readable.
enum class ProfilerCaptureMode : std::uint8_t
{
	LiveRolling,
	FinalizingToArtifact,
	LoadedReadOnly
};

// ProfilerCaptureDetailLevel controls optional sample retention without ever
// dropping the streams needed to explain a frame's critical path or correctness.
enum class ProfilerCaptureDetailLevel : std::uint8_t
{
	Detailed,
	Sampled,
	Aggregate
};

// ProfilerTimingStreamStatus separates real samples from delayed or missing
// telemetry so the editor never presents absent GPU timing as a valid 0 ms.
enum class ProfilerTimingStreamStatus : std::uint8_t
{
	Valid,
	Unavailable,
	Stale,
	Delayed,
	Invalid
};

// ProfilerTimingStreamScope names the user-facing lane that produced a timing
// sample. The game-facing profiler exposes engine CPU plus independent viewport
// timings and intentionally has no editor timing category.
enum class ProfilerTimingStreamScope : std::uint8_t
{
	EngineCpu,
	ViewportCpu,
	ViewportGpu
};

// ProfilerTimingStreamSource records where a sample came from so captures stay
// debuggable even when live renderer diagnostics are not available later.
enum class ProfilerTimingStreamSource : std::uint8_t
{
	ProfilerClock,
	RenderStats,
	BackendGpuTimestamp,
	Unavailable
};


struct ProfilerCaptureSettings
{
	bool cpuEngineEnabled = true;
	bool taskExecutionEnabled = true;
	bool recordMarkers = true;
	bool recordTaskContext = true;
	// Zero keeps every recorded frame. Callers may still request an explicit
	// non-zero rolling limit for constrained automation or focused tests.
	std::uint32_t maxFrames = 0u;
	double maxDurationSeconds = 0.0;
	ProfilerCaptureMode captureMode = ProfilerCaptureMode::LiveRolling;
	ProfilerCaptureDetailLevel detailLevel = ProfilerCaptureDetailLevel::Detailed;
	// Sampled mode uses this deterministic cadence for optional scopes. Aggregate
	// mode skips optional scopes; critical typed evidence remains every frame.
	std::uint32_t sampleIntervalFrames = 4u;
	// Chunks are bounded before writing so a long capture is recoverable even if
	// the final chunk/index is interrupted by shutdown or a power failure.
	std::uint32_t maximumChunkFrames = 120u;
	// Eight MiB keeps recovery records bounded while allowing detailed live
	// frames to amortize journal headers, checksums, and compression work. The
	// queue budget remains separately bounded at 256 MiB.
	std::uint64_t maximumChunkBytes = 8u * 1024u * 1024u;
	// Live V1 journaling owns only a bounded amount of capture memory.  A zero
	// value is never treated as unlimited here: callers must opt into a concrete
	// budget so a long recording cannot silently grow with its duration.
	std::uint64_t residentByteBudget = 256u * 1024u * 1024u;
	// Pending sealed values are bounded independently from segment size. The
	// larger default gives the worker enough batching headroom for dense frames
	// while the one-quarter compact reserve still protects core evidence.
	std::uint64_t maximumPendingBytes = 256u * 1024u * 1024u;
	std::uint64_t maximumSealedFrameBytes = 2u * 1024u * 1024u;
};


struct ProfilerMetric
{
	std::string name;
	double value = 0.0;
	std::string unit;
};

// ProfilerTimingStreamSample is a single frame's value for one timing lane.
// milliseconds is meaningful only when status is Valid.
struct ProfilerTimingStreamSample
{
	std::string id;
	std::string label;
	ProfilerTimingStreamScope scope = ProfilerTimingStreamScope::EngineCpu;
	ProfilerTimingStreamSource source = ProfilerTimingStreamSource::ProfilerClock;
	ProfilerTimingStreamStatus status = ProfilerTimingStreamStatus::Unavailable;
	double milliseconds = 0.0;
	std::string statusReason;
};

// ProfilerTimingStreamAggregate is computed from a capture window. missingCount
// includes unavailable, stale, delayed, and invalid samples for the same stream.
struct ProfilerTimingStreamAggregate
{
	std::string id;
	std::string label;
	ProfilerTimingStreamScope scope = ProfilerTimingStreamScope::EngineCpu;
	ProfilerTimingStreamSource source = ProfilerTimingStreamSource::ProfilerClock;
	ProfilerTimingStreamStatus selectedStatus = ProfilerTimingStreamStatus::Unavailable;
	double selectedMilliseconds = 0.0;
	std::string selectedStatusReason;
	std::size_t sampleCount = 0u;
	std::size_t missingCount = 0u;
	double minMs = 0.0;
	double maxMs = 0.0;
	double meanMs = 0.0;
	double medianMs = 0.0;
	double p90Ms = 0.0;
	double p95Ms = 0.0;
	double p99Ms = 0.0;
	double stdDevMs = 0.0;
};


struct ProfilerModuleSummary
{
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	bool available = false;
	double totalMs = 0.0;
	std::vector<ProfilerMetric> metrics;
};


struct ProfilerSample
{
	std::uint64_t sampleId = 0u;
	std::uint64_t parentSampleId = 0u;
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	std::string name;
	std::string threadLabel;
	std::string taskLabel;
	double startMs = 0.0;
	double durationMs = 0.0;
	std::uint32_t depth = 0u;
	std::uint32_t occurrenceCount = 1u;
	bool complete = true;
	// Aggregate captures omit optional scopes to avoid mutex contention across
	// worker threads. This flag preserves only the fixed frame-root record that
	// lets timeline consumers explain a measured frame without enabling them.
	bool retainInAggregate = false;
};

// Physics fields are copied at the profiler frame boundary. Saved captures and
// display code therefore need neither a live physics world nor a diagnostics UI.
struct ProfilerPhysicsFrameCounters
{
	std::uint64_t tick = 0U;
	std::uint64_t bodyCount = 0U;
	std::uint64_t contactCount = 0U;
	std::uint64_t queryCount = 0U;
	std::uint64_t eventCount = 0U;
	std::uint64_t rejectedCount = 0U;
	std::uint64_t staleCount = 0U;
	std::uint64_t commandQueueDepth = 0U;
	std::uint64_t shapeCacheBytes = 0U;
	std::uint64_t shapeCacheHits = 0U;
	std::uint64_t shapeCacheMisses = 0U;
	std::uint64_t snapshotBytes = 0U;
	std::uint64_t snapshotHighWaterBytes = 0U;
	std::uint64_t providerAllocations = 0U;
	std::uint64_t providerAllocationHighWater = 0U;
	double prepareMs = 0.0;
	double commitMs = 0.0;
	double simulateMs = 0.0;
	double queryMs = 0.0;
	double publishMs = 0.0;
};

// ProfilerAnimationDiagnosticsFacts keeps the always-present animation
// evidence in fixed typed storage while a capture is retained. Dormant frames
// use these three scalar facts instead of allocating and searching three
// string-based ProfilerMetric entries on every frame. Public capture copies
// materialize the established metric names only when a caller requests them.
struct ProfilerAnimationDiagnosticsFacts
{
	std::uint64_t schedulerMembershipChecks = 0U;
	bool dormant = false;
	bool baselineIdentityMatched = false;
};

// ProfilerAnimationOptimizationEvidence is the bounded, frame-coherent proof
// for the compact and complete animation paths. The profiler owns this copy so
// reopening a capture never depends on live animator storage or diagnostic
// strings. Timing values use nanoseconds to avoid locale and precision drift.
struct ProfilerAnimationOptimizationEvidence
{
	static constexpr std::size_t kFallbackReasonCount = 13U;
	static constexpr std::size_t kTimingCount = 8U;

	std::uint64_t frameIndex = 0U;
	std::uint64_t candidateGeneration = 0U;
	std::uint64_t acceptedGeneration = 0U;
	std::uint64_t rejectedGeneration = 0U;
	std::uint64_t topologyGeneration = 0U;
	std::uint64_t presentationGeneration = 0U;
	std::uint64_t rendererGeneration = 0U;
	std::uint64_t sceneGeneration = 0U;
	std::uint64_t activeAnimators = 0U;
	std::uint64_t suspendedAnimators = 0U;
	std::uint64_t compactEligibleAnimators = 0U;
	std::uint64_t compactProcessedAnimators = 0U;
	std::uint64_t completeCapturedAnimators = 0U;
	std::uint64_t completeProcessedAnimators = 0U;
	std::uint64_t compactToCompleteTransitions = 0U;
	std::uint64_t completeToCompactTransitions = 0U;
	std::uint64_t retainedTopologyHits = 0U;
	std::uint64_t retainedTopologyMisses = 0U;
	std::uint64_t compactPlanGroups = 0U;
	std::uint64_t compactDispatches = 0U;
	std::uint64_t rejectedCandidateCount = 0U;
	std::array<std::uint64_t, kFallbackReasonCount> fallbackReasonCounts{};
	std::array<std::uint64_t, kTimingCount> timingNanoseconds{};
	bool valid = false;
};

// ProfilerRenderOptimizationViewSummary is a fixed-size copy of the renderer's
// accepted optimization evidence for one view. The profiler owns these values;
// saved captures never retain renderer objects, Vulkan handles, or hot-path
// diagnostic strings.
struct ProfilerRenderOptimizationViewSummary
{
	std::uint32_t viewType = 0U;
	std::uint64_t frameGeneration = 0U;
	std::uint64_t topologyGeneration = 0U;
	std::uint64_t viewGeneration = 0U;
	std::uint64_t rendererGeneration = 0U;
	std::uint64_t topologyBuildNanoseconds = 0U;
	std::uint64_t dynamicMaterializationNanoseconds = 0U;
	std::uint64_t instanceStreamUploadNanoseconds = 0U;
	std::uint64_t groupedCommandRecordingNanoseconds = 0U;
	std::uint32_t visibleInstanceCount = 0U;
	std::uint32_t groupedInstanceCount = 0U;
	std::uint32_t fallbackInstanceCount = 0U;
	std::uint32_t groupedSubmissionCount = 0U;
	std::uint32_t fallbackSubmissionCount = 0U;
	std::uint32_t avoidedSubmissionCount = 0U;
	std::uint32_t instanceStreamBytes = 0U;
	std::uint32_t instanceStreamCapacityBytes = 0U;
	std::uint32_t topologyReuseHits = 0U;
	std::uint32_t topologyInvalidations = 0U;
	std::uint32_t viewEvidenceReuseHits = 0U;
	std::uint32_t viewEvidenceInvalidations = 0U;
	std::uint32_t rejectedIncompleteKeyCount = 0U;
	std::uint32_t rejectedUnsupportedVariantCount = 0U;
	std::uint32_t rejectedOutOfRangeCount = 0U;
	std::uint32_t rejectedStaleGenerationCount = 0U;
	std::uint32_t fallbackAllocationFailureCount = 0U;
	std::uint32_t fallbackRecoveryCount = 0U;
	bool valid = false;
};

// Game and Scene are recorded together at the profiler frame boundary. Keeping
// the pair typed lets the UI compare evidence with the same frame's timing
// streams without rebuilding string-keyed metric tables.
struct ProfilerRenderOptimizationEvidence
{
	std::array<ProfilerRenderOptimizationViewSummary, 2U> views{};
};

// ProfilerFramePublicationEvidence is optional journal V1 evidence for one complete
// frame. Availability is explicit: an unavailable field is not represented as
// a numeric zero, which keeps comparisons fail-closed when instrumentation is
// absent on one run.
struct ProfilerFramePublicationEvidence
{
	static constexpr std::uint32_t kSchemaVersion = 1U;
	// Keep enough bounded slots for scene, game, capture, picking, shadow, and
	// visualizer consumers without allocating per-view diagnostics dynamically.
	static constexpr std::size_t kMaximumViews = 8U;
	std::uint32_t schemaVersion = kSchemaVersion;
	std::uint64_t availabilityMask = 0U;
	std::uint64_t frameGeneration = 0U;
	std::uint64_t sceneGeneration = 0U;
	std::uint64_t renderSessionGeneration = 0U;
	std::uint64_t topologyGeneration = 0U;
	std::uint64_t dynamicGeneration = 0U;
	std::uint64_t authorityGeneration = 0U;
	std::uint64_t presentationGeneration = 0U;
	std::uint32_t renderOutcome = 0U;
	std::uint32_t animationOutcome = 0U;
	std::uint32_t rejectionReason = 0U;
	std::uint32_t persistentReuseCount = 0U;
	std::uint32_t persistentRebuildCount = 0U;
	std::uint32_t dynamicReuseCount = 0U;
	std::uint32_t dynamicRebuildCount = 0U;
	std::uint32_t authorityReuseCount = 0U;
	std::uint32_t authorityRebuildCount = 0U;
	std::uint32_t viewSelectionReuseCount = 0U;
	std::uint32_t viewSelectionRebuildCount = 0U;
	std::array<std::uint64_t, 8U> disjointTimingNanoseconds{};
	std::array<ProfilerRenderOptimizationViewSummary, kMaximumViews> views{};
	std::uint32_t viewCount = 0U;
};

// ProfilerAnimationStage is append-only because saved capture files store these
// values numerically. The names split CPU planning/authority work from GPU work
// and protected publication so a slow frame cannot hide behind one aggregate.
enum class ProfilerAnimationStage : std::uint8_t
{
	SchedulerMembership = 0U,
	WorkPlanning = 1U,
	CpuGraphEvaluation = 2U,
	MotionMatching = 3U,
	IkSolve = 4U,
	RagdollHandoff = 5U,
	CpuAuthority = 6U,
	GpuPlanning = 7U,
	GpuEvaluation = 8U,
	Deformation = 9U,
	ProtectedPublication = 10U,
	Presentation = 11U,
	Count = 12U
};

inline constexpr std::size_t kProfilerAnimationStageCount =
	static_cast<std::size_t>(ProfilerAnimationStage::Count);
inline constexpr std::size_t kMaximumProfilerAnimationDetailRecordsPerFrame = 256U;
inline constexpr std::uint32_t kProfilerAnimationWorkerEvidence = 1U << 0U;
inline constexpr std::uint32_t kProfilerAnimationVolumeEvidence = 1U << 1U;
inline constexpr std::uint32_t kProfilerAnimationMemoryEvidence = 1U << 2U;
inline constexpr std::uint32_t kProfilerAnimationTransferEvidence = 1U << 3U;
inline constexpr std::uint32_t kProfilerAnimationStallEvidence = 1U << 4U;
inline constexpr std::uint32_t kProfilerAnimationCacheEvidence = 1U << 5U;
inline constexpr std::uint32_t kProfilerAnimationAllEvidence =
	kProfilerAnimationWorkerEvidence | kProfilerAnimationVolumeEvidence
	| kProfilerAnimationMemoryEvidence | kProfilerAnimationTransferEvidence
	| kProfilerAnimationStallEvidence | kProfilerAnimationCacheEvidence;

// ProfilerAnimationStageTiming distinguishes an unavailable timestamp from a
// measured zero. Sampled timings remain explicitly labelled so consumers do not
// accidentally compare them with an every-frame timing stream.
struct ProfilerAnimationStageTiming
{
	std::uint64_t nanoseconds = 0U;
	bool available = false;
	bool sampled = false;
};

// ProfilerAnimationFrameSummary is fixed-size copied evidence for one active
// frame. Counts are raw volumes and bytes rather than derived percentages, so
// final-phase tooling can reproduce utilization, cache reuse, and memory math.
struct ProfilerAnimationFrameSummary
{
	std::uint32_t schemaVersion = 1U;
	// Each bit states that the corresponding zero/non-zero counter family was
	// actually observed. Missing hardware instrumentation must not look like zero.
	std::uint32_t availableEvidenceMask = 0U;
	std::uint64_t schedulerMembershipChecks = 0U;
	std::uint64_t activeAnimators = 0U;
	std::uint64_t evaluatedAnimators = 0U;
	std::uint64_t cpuAnimators = 0U;
	std::uint64_t gpuVisualAnimators = 0U;
	std::uint64_t hybridAnimators = 0U;
	std::uint64_t workerCount = 0U;
	std::uint64_t workerJobs = 0U;
	std::uint64_t workerUsefulItems = 0U;
	std::uint64_t workerBusyNanoseconds = 0U;
	std::uint64_t workerCapacityNanoseconds = 0U;
	std::uint64_t sampledJoints = 0U;
	std::uint64_t blendedTracks = 0U;
	std::uint64_t evaluatedGraphNodes = 0U;
	std::uint64_t motionMatchingCandidates = 0U;
	std::uint64_t ikChains = 0U;
	std::uint64_t ragdollBodies = 0U;
	std::uint64_t gpuEvaluatedJoints = 0U;
	std::uint64_t gpuBatches = 0U;
	std::uint64_t frameArenaBytes = 0U;
	std::uint64_t frameArenaHighWaterBytes = 0U;
	std::uint64_t poseCacheBytes = 0U;
	std::uint64_t poseCacheHighWaterBytes = 0U;
	std::uint64_t gpuResidentBytes = 0U;
	std::uint64_t gpuResidentHighWaterBytes = 0U;
	std::uint64_t gpuTransferBytes = 0U;
	std::uint64_t gpuReadbackBytes = 0U;
	std::uint64_t completePoseReadbackBytes = 0U;
	std::uint64_t boundedMetadataReadbackBytes = 0U;
	std::uint64_t gpuWaitCount = 0U;
	std::uint64_t schedulerWaitCount = 0U;
	std::uint64_t protectedBoundaryStallCount = 0U;
	std::uint64_t poseCacheHits = 0U;
	std::uint64_t poseCacheMisses = 0U;
	std::uint64_t clipCacheHits = 0U;
	std::uint64_t clipCacheMisses = 0U;
	std::uint64_t residencyHits = 0U;
	std::uint64_t residencyMisses = 0U;
	std::array<ProfilerAnimationStageTiming, kProfilerAnimationStageCount> stageTimings{};
};

// Detailed records are numeric copied diagnostics only. They contain no live
// object pointers or unbounded strings and are retained only when the capture's
// existing Detailed/Sampled policy opts in.
struct ProfilerAnimationDetailRecord
{
	std::uint64_t stableInstanceId = 0U;
	std::uint32_t executionMode = 0U;
	std::uint32_t batchOrdinal = 0U;
	std::uint64_t sampledJoints = 0U;
	std::uint64_t evaluatedGraphNodes = 0U;
	std::uint64_t transferBytes = 0U;
	std::uint64_t readbackBytes = 0U;
	std::uint64_t evaluationNanoseconds = 0U;
	bool poseCacheReused = false;
};

// Active animation telemetry is referenced immutably from retained frames. A
// null pointer is the dormant/disabled representation, keeping empty projects
// on the existing three-scalar allocation-free path.
struct ProfilerAnimationFrameTelemetry
{
	ProfilerAnimationFrameSummary summary;
	std::vector<ProfilerAnimationDetailRecord> details;
};


struct ProfilerFrameRecord
{
	std::uint64_t frameIndex = 0u;
	double frameStartMs = 0.0;
	double durationMs = 0.0;
	bool complete = true;
	std::vector<ProfilerTimingStreamSample> timingStreams;
	std::vector<ProfilerModuleSummary> moduleSummaries;
	std::vector<ProfilerSample> samples;
	std::optional<ProfilerPhysicsFrameCounters> physics;
	// Retained frames keep common dormant-animation evidence as values. Snapshot
	// helpers add the legacy public metric strings to their copied frame only.
	std::optional<ProfilerAnimationDiagnosticsFacts> animationDiagnostics;
	std::optional<ProfilerAnimationOptimizationEvidence> animationOptimization;
	// Only active opted-in captures allocate this immutable record. Dormant frames
	// leave it null so disabled/collapsed projects retain structurally minimal data.
	std::shared_ptr<const ProfilerAnimationFrameTelemetry> animationTelemetry;
	std::optional<ProfilerRenderOptimizationEvidence> renderOptimization;
	std::optional<ProfilerFramePublicationEvidence> framePublication;
	// Typed streams are value snapshots. They make native/editor telemetry
	// available after loading a capture without retaining a live subsystem.
	std::vector<PerformanceStageRecord> performanceStages;
	std::vector<ProtectedBoundaryRecord> protectedBoundaries;
	std::vector<WorkerTelemetryRecord> workerTelemetry;
	std::vector<WorkloadExecutionRecord> workloadExecutions;
	std::vector<PresentationWaitRecord> presentationWaits;
	std::optional<FrameResponsivenessRecord> responsiveness;
	std::optional<PhysicsWorkVolumeRecord> physicsWorkVolume;
};

// ProfilerCaptureLossClass tells a reopened artifact exactly which optional
// evidence was deliberately omitted under pressure.  It is not an error value:
// a valid zero-duration sample remains distinguishable from unavailable or
// intentionally discarded detail.
enum class ProfilerCaptureLossClass : std::uint8_t
{
	None,
	AnimationDetails,
	SampleLeaves,
	TaskContext,
	WorkloadRanges,
	CompactOnly,
	CoreRejected
};

// This ordered view is the single pressure-policy contract used by live
// sealing. Earlier tiers preserve progressively more diagnostic value; callers
// must stop as soon as the pending-byte admission test succeeds.
[[nodiscard]] std::span<const ProfilerCaptureLossClass>
GetProfilerOptionalDetailSheddingOrder() noexcept;

// ProfilerCaptureLossReason records why a deterministic policy was selected.
// Persisting this value makes a saturated disk or queue debuggable after the
// editor has restarted.
enum class ProfilerCaptureLossReason : std::uint8_t
{
	None,
	QueuePressure,
	DestinationUnavailable,
	CoreBudgetExceeded
};

// ProfilerCaptureLossRange coalesces adjacent frames that lost the same class
// of optional evidence.  Counts are kept separately so tools never infer loss
// from a missing vector or a numeric zero.
struct ProfilerCaptureLossRange
{
	std::uint64_t firstFrameIndex = 0u;
	std::uint64_t lastFrameIndex = 0u;
	ProfilerCaptureLossClass lossClass = ProfilerCaptureLossClass::None;
	ProfilerCaptureLossReason reason = ProfilerCaptureLossReason::None;
	std::uint64_t discardedRecordCount = 0u;
	std::uint64_t discardedByteCount = 0u;
};

// ProfilerSealedFrame is the immutable hand-off between the protected frame
// boundary and journal persistence.  The owned-byte count is measured before
// queue admission, allowing producers to reject pressure without waiting for
// a persistence worker or relying on allocator-global statistics.
struct ProfilerSealedFrame
{
	ProfilerFrameRecord frame;
	std::shared_ptr<ProfilerDeferredSampleStorage> deferredSamples;
	std::uint64_t ownedByteCount = 0u;
	std::uint64_t deferredAggregationReservationBytes = 0u;
	// Journal frames can defer only derived presentation data. These scalar inputs
	// snapshot every mutable profiler counter needed by coverage aggregation, so
	// the persistence worker never reads a later frame's live state.
	bool needsDeferredAggregation = false;
	bool cpuEngineEnabled = false;
	bool taskExecutionEnabled = false;
	std::uint64_t droppedSampleCount = 0u;
	std::uint64_t truncatedSampleLabelCount = 0u;
	std::uint64_t lateSampleCount = 0u;
	std::uint64_t queuedTaskCount = 0u;
	std::uint64_t startedTaskCount = 0u;
	std::uint64_t finishedTaskCount = 0u;
	std::vector<ProfilerCaptureLossRange> losses;
	// Frames admitted through the reserved compact tier remain distinguishable
	// from full-detail work. The persistence worker merges both FIFO streams by
	// frame index, preserving chronology while preventing optional detail from
	// consuming the bytes reserved for a later compact frame core.
	bool compactCoreOnly = false;
};

// ProfilerCaptureHealth is a bounded live snapshot.  Every field is a scalar
// so an open Profiler window can report recording health without copying the
// growing frame history or any detailed sample hierarchy.
struct ProfilerCaptureHealth
{
	std::uint64_t producedFrameCount = 0u;
	std::uint64_t queuedFrameCount = 0u;
	std::uint64_t committedFrameCount = 0u;
	std::uint64_t residentBytes = 0u;
	std::uint64_t residentHighWaterBytes = 0u;
	std::uint64_t pendingBytes = 0u;
	// In-flight bytes remain resident while a worker serializes/flushed a batch.
	// They are not released until the write either commits or fails.
	std::uint64_t inFlightBytes = 0u;
	std::uint64_t droppedDetailRecordCount = 0u;
	std::uint64_t droppedDetailByteCount = 0u;
	std::uint64_t rejectedCoreFrameCount = 0u;
	std::uint64_t persistenceLagFrames = 0u;
	std::uint64_t persistenceLagHighWaterFrames = 0u;
	std::uint64_t collectionNanoseconds = 0u;
	std::uint64_t sealingNanoseconds = 0u;
	std::uint64_t queueingNanoseconds = 0u;
	std::uint64_t persistenceNanoseconds = 0u;
	std::uint64_t finalizationNanoseconds = 0u;
	std::uint64_t persistedBytes = 0u;
	std::uint64_t persistenceOperationCount = 0u;
	std::uint64_t persistenceThroughputBytesPerSecond = 0u;
	bool destinationWritable = true;
	bool terminalFailure = false;
};

// struct ProfilerFrameListItem keeps the editor frame list cheap to redraw.
// It intentionally mirrors only the columns shown in the list so opening the
// profiler window does not copy every sample string for every retained frame.
struct ProfilerFrameListItem
{
	std::uint64_t frameIndex = 0u;
	double durationMs = 0.0;
	bool complete = true;
};


struct ProfilerCaptureMetadata
{
	std::string terraEngineVersion = "unknown";
	std::string projectName;
	std::string projectPathHint;
	std::string scenePath;
	std::string startedAt;
	std::string savedAt;
	ProfilerCaptureSettings settings;
};


struct ProfilerCaptureArtifact
{
	std::string fileName;
	std::filesystem::path absolutePath;
	ProfilerCaptureMetadata metadata;
	std::string formatIdentifier = "TerraEngineProfilerJournalV1";
	std::uint32_t formatVersion = 1u;
	std::uint64_t frameCount = 0u;
	std::uint64_t committedFrameCount = 0u;
	bool complete = true;
	bool recoveredIncompleteTail = false;
	std::vector<ProfilerModuleId> moduleIds;
	bool supported = true;
	std::string status;
};


struct ProfilerCaptureDocument
{
	ProfilerCaptureMetadata metadata;
	std::vector<ProfilerFrameRecord> frames;
	std::optional<std::filesystem::path> sourcePath;
	std::string lastError;
	std::string formatIdentifier = "TerraEngineProfilerJournalV1";
	std::uint32_t formatVersion = 1u;
	std::uint64_t committedFrameCount = 0u;
	bool complete = true;
	bool recoveredIncompleteTail = false;
};

const char* ToString(ProfilerSessionState state);
const char* ToString(ProfilerModuleId moduleId);
const char* ToString(ProfilerTimingStreamStatus status);
const char* ToString(ProfilerTimingStreamScope scope);
const char* ToString(ProfilerTimingStreamSource source);
const char* ToString(ProfilerCaptureDetailLevel level);
const char* ToString(ProfilerAnimationStage stage);
// Validation is fail-closed before a record enters a live frame or a loaded
// capture. Rejected records never replace prior complete telemetry.
bool ValidateProfilerAnimationFrameTelemetry(const ProfilerAnimationFrameTelemetry& telemetry) noexcept;
bool ValidateProfilerAnimationOptimizationEvidence(
	const ProfilerAnimationOptimizationEvidence& evidence) noexcept;
bool ValidateProfilerRenderOptimizationEvidence(const ProfilerRenderOptimizationEvidence& evidence) noexcept;
bool ValidateProfilerFramePublicationEvidence(const ProfilerFramePublicationEvidence& evidence) noexcept;
bool ValidateProfilerDisjointTimingBuckets(const std::array<std::uint64_t, 8U>& buckets, std::uint64_t frameNanoseconds, std::uint64_t toleranceNanoseconds = 0U) noexcept;
std::string MakeTimestampForFileName();
std::string MakeReadableTimestamp();
bool AnyModuleEnabled(const ProfilerCaptureSettings& settings);
std::vector<ProfilerModuleSummary> MakeDefaultModuleSummaries(const ProfilerCaptureSettings& settings);
void TrimFrameBuffer(std::vector<ProfilerFrameRecord>& frames, const ProfilerCaptureSettings& settings);
// Active animation telemetry is stored as fixed typed values while recording.
// Snapshot/read callers invoke this presentation adapter after copying a frame,
// so stable UI metric aliases never allocate or scan a live frame boundary.
void MaterializeProfilerAnimationPresentationMetrics(ProfilerFrameRecord& frame);
void MaterializeProfilerAnimationPresentationMetrics(ProfilerCaptureDocument& document);
// The V1 journal uses these helpers before a sealed frame enters its bounded
// queue.  They intentionally operate only on value-owned records, never live
// profiler state or allocator-global counters.
std::uint64_t EstimateProfilerFrameOwnedBytes(const ProfilerFrameRecord& frame) noexcept;
bool ValidateProfilerSealedFrame(const ProfilerSealedFrame& frame, std::string& outError) noexcept;
void CoalesceProfilerCaptureLossRange(
	std::vector<ProfilerCaptureLossRange>& ranges,
	const ProfilerCaptureLossRange& next);
// The journal stores one self-contained compact frame payload per local string
// table.  Keeping the binary envelope separate from this value codec lets the
// journal recover a committed segment without depending on neighboring data.
bool EncodeProfilerJournalFrame(
	const ProfilerFrameRecord& frame,
	const ProfilerCaptureSettings& settings,
	std::string& outPayload,
	std::string& outError);
bool DecodeProfilerJournalFrame(
	std::string_view payload,
	ProfilerFrameRecord& outFrame,
	std::string& outError);
void AddOrReplaceTimingStreamSample(ProfilerFrameRecord& frame, ProfilerTimingStreamSample sample);
std::vector<ProfilerTimingStreamAggregate> BuildTimingStreamAggregates(const std::vector<ProfilerFrameRecord>& frames, const std::optional<ProfilerFrameRecord>& selectedFrame = std::nullopt);
bool SaveCaptureDocument(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError);
bool LoadCaptureDocument(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError);
bool TryReadCaptureArtifactMetadata(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
}
