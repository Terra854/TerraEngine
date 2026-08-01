// FrameWorkload.h - architecture-hardening workload declarations and summaries.
//
// These types describe frame work before it touches live engine state. The
// scheduler can use them to prove which jobs may run on workers, which jobs
// must stay protected, and which declarations are unsafe enough to reject.

#pragma once

#include "Core/Export.h"
#include "Profiling/PerformanceTelemetry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace TerraEngine::Scheduling {
	struct JobGraphContext;

	enum class FrameWorkloadExecutionPolicy {
		Protected,
		WorkerSafe,
		Inline,
		Omitted,
		Rejected
	};

	enum class FrameWorkloadMutationPolicy {
		None,
		DeferredOnly
	};

	enum class FrameWorkloadExecutionMode {
		Parallel,
		Compatibility
	};

	// DeterministicRanges divides an immutable item domain into stable,
	// contiguous ranges. Range boundaries depend only on descriptor values, not
	// on the number of available workers, so publication order stays reproducible.
	enum class FrameWorkloadPartitionPolicy {
		Single,
		DeterministicRanges
	};

	struct FrameWorkloadPartition {
		std::size_t partitionIndex = 0;
		std::size_t partitionCount = 0;
		std::size_t itemBegin = 0;
		std::size_t itemEnd = 0;
	};

	enum class WorkerConversionState {
		Unreviewed,
		Protected,
		PartiallyConverted,
		WorkerConverted,
		Rejected
	};

	enum class WorkerSystemPhaseKind {
		Snapshot,
		WorkerPrepare,
		ProtectedCommit,
		Barrier,
		Fallback
	};

	enum class HardBoundaryClassification {
		Protected,
		PartiallyWorkerSafe,
		WorkerSafe,
		Rejected
	};

	enum class ConversionFindingStatus {
		WorkerSafe,
		PartiallyWorkerSafe,
		Protected,
		Rejected,
		NeedsFollowUp
	};

	enum class ArchitectureDiagnosticSeverity {
		Info,
		Warning,
		Error
	};

	enum class ArchitectureDiagnosticCode {
		None,
		EmptyWorkloadName,
		EmptyOwnerSystem,
		MissingWorkerAccess,
		MainThreadProtected,
		ProtectedFallback,
		AccessConflict,
		MutationRequiresBarrier,
		ExecutionFailed,
		CompatibilityFallback,
		InactiveWorkloadOmitted,
		WorkloadNotReady,
		DuplicateDescriptorIdentity,
		MissingExplicitDependency,
		ExplicitDependencyCycle,
		ActiveMembershipInvalidated,
		CompactReferenceStale,
		HierarchySnapshotBlocked,
		ComponentAccessGrantDenied
	};

	struct ArchitectureDiagnostic {
		ArchitectureDiagnosticSeverity severity = ArchitectureDiagnosticSeverity::Info;
		ArchitectureDiagnosticCode code = ArchitectureDiagnosticCode::None;
		std::string workloadName;
		std::string resourceName;
		std::string message;
		std::string recommendedAction;
	};

	struct FrameWorkloadDeclaration {
		std::string workloadName;
		std::string ownerSystem;
		std::string phase;
		// Stable descriptor identity is cached by the owning system. Compatibility
		// callers may leave it empty; validation derives a deterministic identity.
		std::string stableDescriptorId;
		std::uint64_t stableDescriptorGeneration = 1u;
		std::string activationPredicateKey;
		bool active = true;
		bool ready = true;
		std::string readinessReason;
		std::vector<std::string> reads;
		std::vector<std::string> writes;
		std::vector<std::string> explicitDependencies;
		std::size_t executionOrder = 0;
		std::size_t estimatedWorkItemCount = 0;
		std::size_t inlineWorkItemThreshold = 0;
		FrameWorkloadPartitionPolicy partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		std::size_t partitionWorkItemCount = 0;
		std::size_t maximumPartitionCount = 0;
		bool sameFrameVisibilityRequired = false;
		bool workerEligible = false;
		bool mainThreadRequired = true;
		FrameWorkloadMutationPolicy mutationPolicy = FrameWorkloadMutationPolicy::None;
		WorkerSystemPhaseKind phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		HardBoundaryClassification boundaryClassification = HardBoundaryClassification::Protected;
		bool hardBoundary = false;
		std::string safeSubwork;
		std::string protectedCommit;
		std::string protectedReason;
		std::string fallbackReason;
		std::string profilingLabel;
		std::function<void(JobGraphContext&)> work;
		std::function<void(const FrameWorkloadPartition&, JobGraphContext&)> rangeWork;
	};

	struct FrameWorkloadResult {
		FrameWorkloadDeclaration declaration;
		FrameWorkloadExecutionPolicy executionPolicy = FrameWorkloadExecutionPolicy::Protected;
		bool completed = false;
		bool fallbackUsed = false;
		bool inlineSelected = false;
		bool omitted = false;
		std::size_t partitionCount = 0;
	};

	// Validation lowers descriptor and resource ordering into compact result
	// indices once. Execution consumes these indices directly instead of rebuilding
	// string maps and repeating the all-pairs resource-conflict search.
	struct FrameWorkloadDependencyEdge {
		std::size_t prerequisiteIndex = 0;
		std::size_t dependentIndex = 0;
	};

	struct ArchitectureMigrationSummary {
		std::size_t workerSafeCount = 0;
		std::size_t protectedCount = 0;
		std::size_t inlineCount = 0;
		std::size_t rejectedCount = 0;
		std::size_t omittedCount = 0;
		std::size_t fallbackCount = 0;
		std::size_t mutationFlushCount = 0;
		std::size_t workerPhaseCount = 0;
		std::size_t protectedCommitCount = 0;
		std::size_t barrierCount = 0;
		std::size_t hardBoundaryCount = 0;
		std::size_t partiallyWorkerSafeBoundaryCount = 0;
		std::uint64_t protectedBaselineTimeNs = 0;
		std::uint64_t workerPreparationTimeNs = 0;
		std::uint64_t protectedCommitTimeNs = 0;
		std::uint64_t coordinatorWaitTimeNs = 0;
		double workerUtilization = 0.0;
		std::size_t activeMembershipInvalidationCount = 0;
		std::size_t staleObjectHandleCount = 0;
		std::size_t hierarchySnapshotRebuildCount = 0;
		std::size_t componentAccessGrantDeniedCount = 0;
		std::vector<std::string> fallbackReasons;
		std::vector<std::string> workerCategories;
		std::vector<std::string> protectedCategories;
		std::vector<std::string> rejectedCategories;
		std::vector<std::string> hardBoundaryFindings;
		std::string formatted;
	};

	struct WorkerSystemPhase {
		std::string phaseName;
		WorkerSystemPhaseKind phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		std::vector<std::string> reads;
		std::vector<std::string> writes;
		bool requiresMainThread = true;
		bool allowsDeferredMutation = false;
		std::string profilingLabel;
	};

	struct ConvertedSystem {
		std::string name;
		std::string ownerArea;
		WorkerConversionState conversionState = WorkerConversionState::Unreviewed;
		std::vector<WorkerSystemPhase> phases;
		std::string fallbackReason;
		ArchitectureMigrationSummary coverageMetrics;
	};

	struct HardBoundary {
		std::string boundaryName;
		HardBoundaryClassification currentClassification = HardBoundaryClassification::Protected;
		std::vector<std::string> riskFactors;
		std::string safeSubwork;
		std::string protectedCommit;
		std::string requiredEvidence;
	};

	struct ConversionFinding {
		std::string targetName;
		ConversionFindingStatus findingStatus = ConversionFindingStatus::NeedsFollowUp;
		std::string evidence;
		std::string blockingReason;
		std::string recommendedAction;
		std::string lastValidatedScenario;
	};

	struct WorkerCoverageReport {
		std::string frameId;
		std::vector<std::string> workerCategories;
		std::vector<std::string> protectedCategories;
		std::vector<std::string> rejectedCategories;
		std::vector<std::string> fallbackCategories;
		std::vector<std::string> hardBoundaryFindings;
		std::size_t barrierCount = 0;
		std::uint64_t protectedTimeNs = 0;
		std::uint64_t workerTimeNs = 0;
		std::string formattedSummary;
	};

	struct ProtectedCommit {
		std::string commitName;
		std::string sourceSystem;
		std::vector<std::string> requiredBefore;
		std::vector<std::string> mutationInputs;
		std::string externalAffinity;
		std::string diagnostics;
	};

	struct ValidationScenario {
		std::string scenarioName;
		std::string sceneScale;
		std::vector<std::string> targetSystems;
		std::string expectedOutcome;
		std::string baselineMode;
		std::string measurement;
	};

	struct FrameExecutionPlan {
		std::string frameId = "frame";
		std::vector<FrameWorkloadDeclaration> workloads;
		std::vector<FrameWorkloadResult> results;
		std::vector<FrameWorkloadDependencyEdge> dependencyEdges;
		std::vector<ArchitectureDiagnostic> diagnostics;
		// Execution records are bounded value snapshots; they never retain the
		// std::function callbacks held by workload declarations.
		std::vector<TerraEngine::Profiling::WorkloadExecutionRecord> executionRecords;
		std::size_t droppedExecutionRecordCount = 0;
		ArchitectureMigrationSummary migrationSummary;
		FrameWorkloadExecutionMode executionMode = FrameWorkloadExecutionMode::Parallel;
		bool parallelExecutionUsed = false;
		bool valid = true;
		bool completed = false;
		std::string formatted;
	};

	TERRAENGINE_API std::string ToString(FrameWorkloadExecutionPolicy policy);
	TERRAENGINE_API std::string ToString(FrameWorkloadMutationPolicy policy);
	TERRAENGINE_API std::string ToString(FrameWorkloadExecutionMode mode);
	TERRAENGINE_API std::string ToString(FrameWorkloadPartitionPolicy policy);
	TERRAENGINE_API std::string ToString(WorkerConversionState state);
	TERRAENGINE_API std::string ToString(WorkerSystemPhaseKind kind);
	TERRAENGINE_API std::string ToString(HardBoundaryClassification classification);
	TERRAENGINE_API std::string ToString(ConversionFindingStatus status);
	TERRAENGINE_API std::string ToString(ArchitectureDiagnosticSeverity severity);
	TERRAENGINE_API std::string ToString(ArchitectureDiagnosticCode code);
	TERRAENGINE_API std::string FormatArchitectureDiagnostics(const std::vector<ArchitectureDiagnostic>& diagnostics);
	TERRAENGINE_API std::string FormatArchitectureMigrationSummary(const ArchitectureMigrationSummary& summary);
	TERRAENGINE_API std::string FormatWorkerCoverageReport(const WorkerCoverageReport& report);
	TERRAENGINE_API std::string FormatFrameExecutionPlan(const FrameExecutionPlan& plan);
	TERRAENGINE_API FrameWorkloadDeclaration MakeSnapshotPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::string profilingLabel = {});
	TERRAENGINE_API FrameWorkloadDeclaration MakeWorkerPreparePhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::vector<std::string> writes, std::string profilingLabel = {});
	TERRAENGINE_API FrameWorkloadDeclaration MakeProtectedCommitPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::vector<std::string> writes, std::string protectedReason, std::string profilingLabel = {});
	TERRAENGINE_API FrameWorkloadDeclaration MakeBarrierPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> resources, std::string protectedReason, std::string profilingLabel = {});
	TERRAENGINE_API FrameWorkloadDeclaration MakeFallbackPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::vector<std::string> writes, std::string fallbackReason, std::string profilingLabel = {});
	TERRAENGINE_API std::vector<HardBoundary> InitialWorkerHardBoundaries();
	TERRAENGINE_API std::vector<ConversionFinding> InitialWorkerConversionFindings();
	TERRAENGINE_API WorkerCoverageReport BuildWorkerCoverageReport(const FrameExecutionPlan& plan);
	TERRAENGINE_API FrameExecutionPlan ValidateFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId = "frame");
	TERRAENGINE_API FrameExecutionPlan ExecuteFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId = "frame");
	TERRAENGINE_API FrameExecutionPlan ExecuteFrameWorkloadsCompatibility(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId = "frame");
	// Returns a bounded value copy from the most recently completed scheduler
	// execution. The profiler consumes it after the protected frame boundary;
	// callbacks and mutable workload inputs are never retained here.
	TERRAENGINE_API std::vector<TerraEngine::Profiling::WorkloadExecutionRecord> LatestFrameWorkloadExecutionRecordsForProfiling();
	TERRAENGINE_API std::vector<FrameWorkloadPartition> BuildDeterministicWorkloadPartitions(std::size_t itemCount, std::size_t targetItemsPerPartition, std::size_t maximumPartitionCount = 0);
	TERRAENGINE_API void AddArchitectureDiagnostic(std::vector<ArchitectureDiagnostic>& diagnostics, ArchitectureDiagnosticSeverity severity, ArchitectureDiagnosticCode code, std::string workloadName, std::string resourceName, std::string message, std::string action);

}
