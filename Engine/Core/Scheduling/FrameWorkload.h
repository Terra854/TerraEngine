// FrameWorkload.h - architecture-hardening workload declarations and summaries.
//
// These types describe frame work before it touches live engine state. The
// scheduler can use them to prove which jobs may run on workers, which jobs
// must stay protected, and which declarations are unsafe enough to reject.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace TerraEngine::Scheduling {
	struct JobGraphContext;

	enum class FrameWorkloadExecutionPolicy {
		Protected,
		WorkerSafe,
		Rejected
	};

	enum class FrameWorkloadMutationPolicy {
		None,
		DeferredOnly
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
		std::vector<std::string> reads;
		std::vector<std::string> writes;
		bool workerEligible = false;
		bool mainThreadRequired = true;
		FrameWorkloadMutationPolicy mutationPolicy = FrameWorkloadMutationPolicy::None;
		std::string protectedReason;
		std::string profilingLabel;
		std::function<void(JobGraphContext&)> work;
	};

	struct FrameWorkloadResult {
		FrameWorkloadDeclaration declaration;
		FrameWorkloadExecutionPolicy executionPolicy = FrameWorkloadExecutionPolicy::Protected;
		bool completed = false;
		bool fallbackUsed = false;
	};

	struct ArchitectureMigrationSummary {
		std::size_t workerSafeCount = 0;
		std::size_t protectedCount = 0;
		std::size_t rejectedCount = 0;
		std::size_t fallbackCount = 0;
		std::size_t mutationFlushCount = 0;
		std::size_t activeMembershipInvalidationCount = 0;
		std::size_t staleCompactReferenceCount = 0;
		std::size_t hierarchySnapshotRebuildCount = 0;
		std::size_t componentAccessGrantDeniedCount = 0;
		std::vector<std::string> fallbackReasons;
		std::string formatted;
	};

	struct FrameExecutionPlan {
		std::string frameId = "frame";
		std::vector<FrameWorkloadDeclaration> workloads;
		std::vector<FrameWorkloadResult> results;
		std::vector<ArchitectureDiagnostic> diagnostics;
		ArchitectureMigrationSummary migrationSummary;
		bool valid = true;
		bool completed = false;
		std::string formatted;
	};

	TERRAENGINE_API std::string ToString(FrameWorkloadExecutionPolicy policy);
	TERRAENGINE_API std::string ToString(FrameWorkloadMutationPolicy policy);
	TERRAENGINE_API std::string ToString(ArchitectureDiagnosticSeverity severity);
	TERRAENGINE_API std::string ToString(ArchitectureDiagnosticCode code);
	TERRAENGINE_API std::string FormatArchitectureDiagnostics(const std::vector<ArchitectureDiagnostic>& diagnostics);
	TERRAENGINE_API std::string FormatArchitectureMigrationSummary(const ArchitectureMigrationSummary& summary);
	TERRAENGINE_API std::string FormatFrameExecutionPlan(const FrameExecutionPlan& plan);
	TERRAENGINE_API FrameExecutionPlan ValidateFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId = "frame");
	TERRAENGINE_API FrameExecutionPlan ExecuteFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId = "frame");
	TERRAENGINE_API void AddArchitectureDiagnostic(std::vector<ArchitectureDiagnostic>& diagnostics, ArchitectureDiagnosticSeverity severity, ArchitectureDiagnosticCode code, std::string workloadName, std::string resourceName, std::string message, std::string action);

}
