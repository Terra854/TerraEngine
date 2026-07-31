// ProfilerCapture.h — Contains declarations for the Profiler Capture file-specific types, helper functions, and module integration code used by runtime profiling sessions, timing markers, capture serialization, and scope helpers.

#pragma once

#include "Profiling/PerformanceTelemetry.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Profiling
{
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

// struct ProfilerCaptureSettings groups related state and operations so callers have one documented contract.
struct ProfilerCaptureSettings
{
	bool cpuEngineEnabled = true;
	bool taskExecutionEnabled = true;
	bool recordMarkers = true;
	bool recordTaskContext = true;
	std::uint32_t maxFrames = 10000u;
	double maxDurationSeconds = 0.0;
	ProfilerCaptureMode captureMode = ProfilerCaptureMode::LiveRolling;
	ProfilerCaptureDetailLevel detailLevel = ProfilerCaptureDetailLevel::Detailed;
	// Sampled mode uses this deterministic cadence for optional scopes. Aggregate
	// mode skips optional scopes; critical typed evidence remains every frame.
	std::uint32_t sampleIntervalFrames = 4u;
	// Chunks are bounded before writing so a long capture is recoverable even if
	// the final chunk/index is interrupted by shutdown or a power failure.
	std::uint32_t maximumChunkFrames = 120u;
	std::uint64_t maximumChunkBytes = 1024u * 1024u;
};

// struct ProfilerMetric groups related state and operations so callers have one documented contract.
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

// struct ProfilerModuleSummary groups related state and operations so callers have one documented contract.
struct ProfilerModuleSummary
{
	ProfilerModuleId moduleId = ProfilerModuleId::CpuEngine;
	bool available = false;
	double totalMs = 0.0;
	std::vector<ProfilerMetric> metrics;
};

// struct ProfilerSample groups related state and operations so callers have one documented contract.
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

// struct ProfilerFrameRecord groups related state and operations so callers have one documented contract.
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
	// Only active opted-in captures allocate this immutable record. Dormant frames
	// leave it null so disabled/collapsed projects retain structurally minimal data.
	std::shared_ptr<const ProfilerAnimationFrameTelemetry> animationTelemetry;
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

// struct ProfilerFrameListItem keeps the editor frame list cheap to redraw.
// It intentionally mirrors only the columns shown in the list so opening the
// profiler window does not copy every sample string for every retained frame.
struct ProfilerFrameListItem
{
	std::uint64_t frameIndex = 0u;
	double durationMs = 0.0;
	bool complete = true;
};

// struct ProfilerCaptureMetadata groups related state and operations so callers have one documented contract.
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

// struct ProfilerCaptureArtifact groups related state and operations so callers have one documented contract.
struct ProfilerCaptureArtifact
{
	std::string fileName;
	std::filesystem::path absolutePath;
	ProfilerCaptureMetadata metadata;
	std::uint32_t payloadVersion = 1u;
	std::uint64_t frameCount = 0u;
	std::uint64_t committedFrameCount = 0u;
	bool complete = true;
	bool recoveredIncompleteTail = false;
	std::vector<ProfilerModuleId> moduleIds;
	bool supported = true;
	std::string status;
};

// struct ProfilerCaptureDocument groups related state and operations so callers have one documented contract.
struct ProfilerCaptureDocument
{
	ProfilerCaptureMetadata metadata;
	std::vector<ProfilerFrameRecord> frames;
	std::optional<std::filesystem::path> sourcePath;
	std::string lastError;
	std::uint32_t payloadVersion = 2u;
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
std::string MakeTimestampForFileName();
std::string MakeReadableTimestamp();
bool AnyModuleEnabled(const ProfilerCaptureSettings& settings);
std::vector<ProfilerModuleSummary> MakeDefaultModuleSummaries(const ProfilerCaptureSettings& settings);
void TrimFrameBuffer(std::vector<ProfilerFrameRecord>& frames, const ProfilerCaptureSettings& settings);
void AddOrReplaceTimingStreamSample(ProfilerFrameRecord& frame, ProfilerTimingStreamSample sample);
std::vector<ProfilerTimingStreamAggregate> BuildTimingStreamAggregates(const std::vector<ProfilerFrameRecord>& frames, const std::optional<ProfilerFrameRecord>& selectedFrame = std::nullopt);
bool SaveCaptureDocument(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError);
bool LoadCaptureDocument(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError);
bool TryReadCaptureArtifactMetadata(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
bool TryReadCaptureArtifact(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);
}
