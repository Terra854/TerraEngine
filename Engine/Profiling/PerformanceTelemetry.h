// PerformanceTelemetry.h — Defines typed profiler evidence for performance-critical frame work.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace TerraEngine::Profiling
{
// TelemetryAvailability prevents an absent clock or counter from looking like a
// valid zero-duration sample in capture summaries and automation output.
enum class TelemetryAvailability
{
	Available,
	Unavailable
};

// TelemetryDuration is used for all time fields so unavailable timing has a
// reason and never accidentally participates in percentile calculations.
struct TelemetryDuration
{
	TelemetryAvailability availability = TelemetryAvailability::Unavailable;
	std::uint64_t nanoseconds = 0u;
	std::string unavailableReason;
};

// PerformanceStageDomain separates native physics stages from editor Scene View
// stages without putting arbitrary editor window scopes into game CPU samples.
enum class PerformanceStageDomain
{
	Physics,
	SceneView,
	// Editor stages are captured as value timings outside Engine CPU. This keeps
	// the profiler useful for diagnosing editor-window cost without allowing
	// nested ImGui markers to contaminate the game-facing CPU timeline.
	Editor
};

struct PerformanceStageRecord
{
	PerformanceStageDomain domain = PerformanceStageDomain::Physics;
	std::string stageName;
	TelemetryDuration duration;
	std::string workloadName;
	std::uint64_t workItemCount = 0u;
	bool sampled = false;
};

// ProtectedBoundaryRecord decomposes the protected section of a frame. Keeping
// each duration separate identifies whether a stall was queueing, ownership,
// execution, flush, or final publication work.
struct ProtectedBoundaryRecord
{
	std::string boundaryName;
	TelemetryDuration queueDuration;
	TelemetryDuration ownershipWaitDuration;
	TelemetryDuration executionDuration;
	TelemetryDuration flushDuration;
	TelemetryDuration publicationDuration;
};

// WorkerTelemetryRecord records only worker-owned scheduling facts. It can be
// produced by worker-safe accounting and later copied into a profiler frame.
struct WorkerTelemetryRecord
{
	std::uint32_t workerId = 0u;
	std::uint64_t queueDepth = 0u;
	std::uint64_t dispatchCount = 0u;
	std::uint64_t usefulWorkItemCount = 0u;
	TelemetryDuration runDuration;
	TelemetryDuration waitDuration;
	TelemetryDuration idleDuration;
};

// WorkloadExecutionPath identifies the scheduler path selected for one stable
// workload descriptor. It is intentionally independent from task closures so
// captures never extend the lifetime of mutable per-frame inputs.
enum class WorkloadExecutionPath
{
	None,
	Inline,
	Worker,
	Protected,
	Rejected
};

// WorkloadExecutionStatus records the terminal scheduling decision. Inactive
// and not-ready descriptors are retained as decisions with zero dispatches;
// they are never represented as ThreadPool jobs.
enum class WorkloadExecutionStatus
{
	Inactive,
	NotReady,
	Inline,
	Queued,
	Running,
	Completed,
	Rejected,
	Cancelled
};

// WorkloadExecutionRecord is a value-only snapshot of one scheduler decision.
// Strings, counters, and durations are copied; callbacks and mutable frame data
// are deliberately absent so profiler retention cannot keep gameplay state alive.
struct WorkloadExecutionRecord
{
	std::string frameId;
	std::string descriptorId;
	std::uint64_t descriptorGeneration = 0u;
	std::string ownerSystem;
	std::string phase;
	std::string workloadName;
	std::string activationPredicateKey;
	bool active = false;
	bool ready = false;
	WorkloadExecutionPath selectedPath = WorkloadExecutionPath::None;
	WorkloadExecutionStatus status = WorkloadExecutionStatus::Inactive;
	bool workerIdentityAvailable = false;
	std::uint32_t workerId = 0u;
	std::uint64_t queueDepthAtEnqueue = 0u;
	std::uint64_t queueDepthAtStart = 0u;
	std::uint64_t dispatchCount = 0u;
	std::uint64_t usefulWorkItemCount = 0u;
	std::uint64_t partitionIndex = 0u;
	std::uint64_t partitionCount = 0u;
	std::uint64_t itemBegin = 0u;
	std::uint64_t itemEnd = 0u;
	bool sameFrameVisibilityRequired = false;
	TelemetryDuration queueDuration;
	TelemetryDuration runDuration;
	TelemetryDuration coordinatorWaitDuration;
	TelemetryDuration ownershipWaitDuration;
	TelemetryDuration flushDuration;
	TelemetryDuration publicationDuration;
};

// A frame may declare many fine-grained ranges. Bounding retained decision
// records keeps diagnostics safe even if a malformed descriptor asks for an
// excessive number of partitions.
inline constexpr std::size_t kMaximumWorkloadExecutionRecordsPerFrame = 4096u;

// PresentationWaitKind keeps unavoidable presentation waits distinguishable
// from CPU work, avoiding misleading attribution to render preparation.
enum class PresentationWaitKind
{
	SwapchainAcquire,
	RenderQueueBackpressure,
	GpuCompletion,
	UiComposition
};

struct PresentationWaitRecord
{
	PresentationWaitKind kind = PresentationWaitKind::SwapchainAcquire;
	TelemetryDuration duration;
	std::string detail;
};

// FrameResponsivenessRecord retains user-visible latency separately from frame
// cost. A responsive frame can be slow, and a fast frame can still wait to present.
struct FrameResponsivenessRecord
{
	std::uint64_t frameIndex = 0u;
	TelemetryDuration inputToPresentationDuration;
	TelemetryDuration frameDuration;
	std::uint64_t presentationSequence = 0u;
};

// ProfilerArtifactRecord describes the compact capture result after a save or
// recovery attempt. Recovery is valid only for an otherwise incomplete artifact.
struct ProfilerArtifactRecord
{
	std::string artifactId;
	std::uint64_t storedByteCount = 0u;
	std::uint64_t committedFrameCount = 0u;
	bool complete = false;
	bool recoveredIncompleteTail = false;
};

// PhysicsWorkVolumeRecord exposes processed-versus-published body counts for a
// tick. The ordered counts make excessive extraction or publication visible.
struct PhysicsWorkVolumeRecord
{
	std::uint64_t tick = 0u;
	std::uint64_t totalBodyCount = 0u;
	std::uint64_t activeBodyCount = 0u;
	std::uint64_t examinedBodyCount = 0u;
	std::uint64_t extractedBodyCount = 0u;
	std::uint64_t convertedBodyCount = 0u;
	std::uint64_t changedBodyCount = 0u;
	std::uint64_t publishedBodyCount = 0u;
};

// TelemetryValidationResult uses diagnostics instead of exceptions because
// capture readers must safely reject malformed external artifacts.
struct TelemetryValidationResult
{
	bool valid = false;
	std::vector<std::string> diagnostics;
};

[[nodiscard]] TelemetryValidationResult ValidateTelemetryDuration(const TelemetryDuration& duration);
[[nodiscard]] TelemetryValidationResult ValidatePerformanceStageRecord(const PerformanceStageRecord& record);
[[nodiscard]] TelemetryValidationResult ValidateProtectedBoundaryRecord(const ProtectedBoundaryRecord& record);
[[nodiscard]] TelemetryValidationResult ValidateWorkerTelemetryRecord(const WorkerTelemetryRecord& record);
[[nodiscard]] TelemetryValidationResult ValidateWorkloadExecutionRecord(const WorkloadExecutionRecord& record);
[[nodiscard]] TelemetryValidationResult ValidatePresentationWaitRecord(const PresentationWaitRecord& record);
[[nodiscard]] TelemetryValidationResult ValidateFrameResponsivenessRecord(const FrameResponsivenessRecord& record);
[[nodiscard]] TelemetryValidationResult ValidateProfilerArtifactRecord(const ProfilerArtifactRecord& record);
[[nodiscard]] TelemetryValidationResult ValidatePhysicsWorkVolumeRecord(const PhysicsWorkVolumeRecord& record);
}
