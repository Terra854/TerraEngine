// FrameWorkload.cpp - validation and execution for architecture-hardening work.

#include "FrameWorkload.h"

#include "JobGraph.h"
#include "Profiling/ProfilerScope.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		int SeverityRank(ArchitectureDiagnosticSeverity severity)
		{
			switch (severity)
			{
			case ArchitectureDiagnosticSeverity::Error:
				return 0;
			case ArchitectureDiagnosticSeverity::Warning:
				return 1;
			case ArchitectureDiagnosticSeverity::Info:
			default:
				return 2;
			}
		}

		bool IsError(const ArchitectureDiagnostic& diagnostic)
		{
			return diagnostic.severity == ArchitectureDiagnosticSeverity::Error;
		}

		void SortDiagnostics(std::vector<ArchitectureDiagnostic>& diagnostics)
		{
			std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const ArchitectureDiagnostic& left, const ArchitectureDiagnostic& right) {
				return std::make_tuple(SeverityRank(left.severity), static_cast<int>(left.code), left.workloadName, left.resourceName, left.message)
					< std::make_tuple(SeverityRank(right.severity), static_cast<int>(right.code), right.workloadName, right.resourceName, right.message);
			});
		}

		std::vector<std::string> NormalizeList(std::vector<std::string> values)
		{
			values.erase(std::remove_if(values.begin(), values.end(), [](const std::string& value) {
				return value.empty();
			}), values.end());
			std::stable_sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end()), values.end());
			return values;
		}

		std::set<std::string> ReadSet(const FrameWorkloadDeclaration& declaration)
		{
			return std::set<std::string>(declaration.reads.begin(), declaration.reads.end());
		}

		std::set<std::string> WriteSet(const FrameWorkloadDeclaration& declaration)
		{
			return std::set<std::string>(declaration.writes.begin(), declaration.writes.end());
		}

		bool HasWriteConflict(const FrameWorkloadDeclaration& left, const FrameWorkloadDeclaration& right, std::string& resourceName)
		{
			const std::set<std::string> leftReads = ReadSet(left);
			const std::set<std::string> leftWrites = WriteSet(left);
			const std::set<std::string> rightReads = ReadSet(right);
			const std::set<std::string> rightWrites = WriteSet(right);
			for (const std::string& write : leftWrites)
			{
				if (rightWrites.contains(write) || rightReads.contains(write))
				{
					resourceName = write;
					return true;
				}
			}
			for (const std::string& write : rightWrites)
			{
				if (leftReads.contains(write))
				{
					resourceName = write;
					return true;
				}
			}
			return false;
		}

		FrameWorkloadExecutionPolicy SelectPolicy(const FrameWorkloadDeclaration& declaration, std::vector<ArchitectureDiagnostic>& diagnostics)
		{
			if (declaration.workerEligible && declaration.reads.empty() && declaration.writes.empty())
			{
				AddArchitectureDiagnostic(diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::MissingWorkerAccess, declaration.workloadName, "", "Worker-safe workload has no declared reads or writes.", "Declare all accessed resources or keep the workload protected.");
				return FrameWorkloadExecutionPolicy::Rejected;
			}
			if (declaration.workerEligible && !declaration.mainThreadRequired)
			{
				return FrameWorkloadExecutionPolicy::WorkerSafe;
			}
			if (declaration.mainThreadRequired)
			{
				AddArchitectureDiagnostic(diagnostics, ArchitectureDiagnosticSeverity::Info, ArchitectureDiagnosticCode::MainThreadProtected, declaration.workloadName, "", "Workload remains protected because it requires the main thread.", "No action required unless the workload can be converted to immutable inputs.");
			}
			else if (!declaration.workerEligible)
			{
				AddArchitectureDiagnostic(diagnostics, ArchitectureDiagnosticSeverity::Info, ArchitectureDiagnosticCode::ProtectedFallback, declaration.workloadName, "", "Workload remains protected because worker-safe execution was not declared.", "Add a complete worker-safe access declaration when ready.");
			}
			return FrameWorkloadExecutionPolicy::Protected;
		}

		bool ShouldRunInline(const FrameWorkloadResult& result)
		{
			return result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe
				&& result.declaration.inlineWorkItemThreshold > 0
				&& result.declaration.estimatedWorkItemCount <= result.declaration.inlineWorkItemThreshold;
		}

		void ApplyInlinePolicy(FrameExecutionPlan& plan)
		{
			std::size_t inlineCount = 0;
			for (FrameWorkloadResult& result : plan.results)
			{
				if (ShouldRunInline(result))
				{
					result.executionPolicy = FrameWorkloadExecutionPolicy::Inline;
					result.inlineSelected = true;
					++inlineCount;
				}
			}
			plan.migrationSummary.inlineCount = inlineCount;
		}

		void RefreshMigrationSummary(FrameExecutionPlan& plan)
		{
			ArchitectureMigrationSummary summary;
			for (const FrameWorkloadResult& result : plan.results)
			{
				if (result.declaration.mutationPolicy == FrameWorkloadMutationPolicy::DeferredOnly)
				{
					++summary.mutationFlushCount;
				}
				if (result.declaration.phaseKind == WorkerSystemPhaseKind::WorkerPrepare)
				{
					++summary.workerPhaseCount;
				}
				else if (result.declaration.phaseKind == WorkerSystemPhaseKind::ProtectedCommit)
				{
					++summary.protectedCommitCount;
				}
				else if (result.declaration.phaseKind == WorkerSystemPhaseKind::Barrier)
				{
					++summary.barrierCount;
				}
				if (result.declaration.hardBoundary)
				{
					++summary.hardBoundaryCount;
					if (result.declaration.boundaryClassification == HardBoundaryClassification::PartiallyWorkerSafe)
					{
						++summary.partiallyWorkerSafeBoundaryCount;
					}
					std::ostringstream finding;
					finding << result.declaration.workloadName
						<< "=" << ToString(result.declaration.boundaryClassification)
						<< " safe='" << result.declaration.safeSubwork
						<< "' protected='" << result.declaration.protectedCommit << "'";
					summary.hardBoundaryFindings.push_back(finding.str());
				}
				switch (result.executionPolicy)
				{
				case FrameWorkloadExecutionPolicy::WorkerSafe:
					++summary.workerSafeCount;
					summary.workerCategories.push_back(result.declaration.ownerSystem + ":" + result.declaration.workloadName);
					break;
				case FrameWorkloadExecutionPolicy::Inline:
					summary.protectedCategories.push_back(result.declaration.ownerSystem + ":" + result.declaration.workloadName + ":inline");
					break;
				case FrameWorkloadExecutionPolicy::Rejected:
					++summary.rejectedCount;
					summary.rejectedCategories.push_back(result.declaration.ownerSystem + ":" + result.declaration.workloadName);
					break;
				case FrameWorkloadExecutionPolicy::Protected:
				default:
					++summary.protectedCount;
					summary.protectedCategories.push_back(result.declaration.ownerSystem + ":" + result.declaration.workloadName);
					if (result.fallbackUsed)
					{
						++summary.fallbackCount;
						const std::string reason = !result.declaration.fallbackReason.empty()
							? result.declaration.fallbackReason
							: result.declaration.protectedReason.empty()
							? "protected fallback"
							: result.declaration.protectedReason;
						summary.fallbackReasons.push_back(result.declaration.workloadName + ": " + reason);
					}
					break;
				}
			}
			for (const ArchitectureDiagnostic& diagnostic : plan.diagnostics)
			{
				if (diagnostic.code == ArchitectureDiagnosticCode::ActiveMembershipInvalidated)
				{
					++summary.activeMembershipInvalidationCount;
				}
				else if (diagnostic.code == ArchitectureDiagnosticCode::CompactReferenceStale)
				{
					++summary.staleObjectHandleCount;
				}
				else if (diagnostic.code == ArchitectureDiagnosticCode::HierarchySnapshotBlocked)
				{
					++summary.hierarchySnapshotRebuildCount;
				}
				else if (diagnostic.code == ArchitectureDiagnosticCode::ComponentAccessGrantDenied)
				{
					++summary.componentAccessGrantDeniedCount;
				}
			}
			auto normalize = [](std::vector<std::string>& values) {
				std::stable_sort(values.begin(), values.end());
				values.erase(std::unique(values.begin(), values.end()), values.end());
			};
			normalize(summary.workerCategories);
			normalize(summary.protectedCategories);
			normalize(summary.rejectedCategories);
			normalize(summary.fallbackReasons);
			normalize(summary.hardBoundaryFindings);
			summary.inlineCount += plan.migrationSummary.inlineCount;
			summary.protectedBaselineTimeNs = plan.migrationSummary.protectedBaselineTimeNs;
			summary.workerPreparationTimeNs = plan.migrationSummary.workerPreparationTimeNs;
			summary.protectedCommitTimeNs = plan.migrationSummary.protectedCommitTimeNs;
			summary.coordinatorWaitTimeNs = plan.migrationSummary.coordinatorWaitTimeNs;
			summary.workerUtilization = plan.migrationSummary.workerUtilization;
			summary.formatted = FormatArchitectureMigrationSummary(summary);
			plan.migrationSummary = std::move(summary);
		}

		void ApplyJobGraphSummary(FrameExecutionPlan& plan, const JobGraphSummary& summary)
		{
			std::unordered_map<std::string, JobGraphNodeState> statesByName;
			for (const JobGraphResultEntry& entry : summary.jobs)
			{
				statesByName[entry.name] = entry.state;
			}
			for (FrameWorkloadResult& result : plan.results)
			{
				const auto found = statesByName.find(result.declaration.workloadName);
				result.completed = found != statesByName.end() && found->second == JobGraphNodeState::Completed;
			}
			plan.completed = summary.completed && std::all_of(plan.results.begin(), plan.results.end(), [](const FrameWorkloadResult& result) {
				return result.completed || result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected;
			});
			plan.migrationSummary.inlineCount = summary.inlineJobCount;
			plan.migrationSummary.workerPreparationTimeNs = summary.workerTimeNs;
			plan.migrationSummary.protectedCommitTimeNs = summary.protectedTimeNs;
			plan.migrationSummary.coordinatorWaitTimeNs = summary.coordinatorWaitTimeNs;
			plan.migrationSummary.workerUtilization = summary.workerUtilization;
		}
	}

	void AddArchitectureDiagnostic(std::vector<ArchitectureDiagnostic>& diagnostics, ArchitectureDiagnosticSeverity severity, ArchitectureDiagnosticCode code, std::string workloadName, std::string resourceName, std::string message, std::string action)
	{
		diagnostics.push_back({
			severity,
			code,
			std::move(workloadName),
			std::move(resourceName),
			std::move(message),
			std::move(action)
		});
	}

	std::string ToString(FrameWorkloadExecutionPolicy policy)
	{
		switch (policy)
		{
		case FrameWorkloadExecutionPolicy::WorkerSafe:
			return "worker-safe";
		case FrameWorkloadExecutionPolicy::Inline:
			return "inline";
		case FrameWorkloadExecutionPolicy::Rejected:
			return "rejected";
		case FrameWorkloadExecutionPolicy::Protected:
		default:
			return "protected";
		}
	}

	std::string ToString(FrameWorkloadMutationPolicy policy)
	{
		return policy == FrameWorkloadMutationPolicy::DeferredOnly ? "deferred-only" : "none";
	}

	std::string ToString(FrameWorkloadExecutionMode mode)
	{
		return mode == FrameWorkloadExecutionMode::Compatibility ? "compatibility" : "parallel";
	}

	std::string ToString(WorkerConversionState state)
	{
		switch (state)
		{
		case WorkerConversionState::Protected:
			return "protected";
		case WorkerConversionState::PartiallyConverted:
			return "partially-converted";
		case WorkerConversionState::WorkerConverted:
			return "worker-converted";
		case WorkerConversionState::Rejected:
			return "rejected";
		case WorkerConversionState::Unreviewed:
		default:
			return "unreviewed";
		}
	}

	std::string ToString(WorkerSystemPhaseKind kind)
	{
		switch (kind)
		{
		case WorkerSystemPhaseKind::Snapshot:
			return "snapshot";
		case WorkerSystemPhaseKind::WorkerPrepare:
			return "worker-prepare";
		case WorkerSystemPhaseKind::ProtectedCommit:
			return "protected-commit";
		case WorkerSystemPhaseKind::Barrier:
			return "barrier";
		case WorkerSystemPhaseKind::Fallback:
		default:
			return "fallback";
		}
	}

	std::string ToString(HardBoundaryClassification classification)
	{
		switch (classification)
		{
		case HardBoundaryClassification::PartiallyWorkerSafe:
			return "partially-worker-safe";
		case HardBoundaryClassification::WorkerSafe:
			return "worker-safe";
		case HardBoundaryClassification::Rejected:
			return "rejected";
		case HardBoundaryClassification::Protected:
		default:
			return "protected";
		}
	}

	std::string ToString(ConversionFindingStatus status)
	{
		switch (status)
		{
		case ConversionFindingStatus::WorkerSafe:
			return "worker-safe";
		case ConversionFindingStatus::PartiallyWorkerSafe:
			return "partially-worker-safe";
		case ConversionFindingStatus::Protected:
			return "protected";
		case ConversionFindingStatus::Rejected:
			return "rejected";
		case ConversionFindingStatus::NeedsFollowUp:
		default:
			return "needs-follow-up";
		}
	}

	std::string ToString(ArchitectureDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case ArchitectureDiagnosticSeverity::Error:
			return "error";
		case ArchitectureDiagnosticSeverity::Warning:
			return "warning";
		case ArchitectureDiagnosticSeverity::Info:
		default:
			return "info";
		}
	}

	std::string ToString(ArchitectureDiagnosticCode code)
	{
		switch (code)
		{
		case ArchitectureDiagnosticCode::EmptyWorkloadName: return "empty-workload-name";
		case ArchitectureDiagnosticCode::EmptyOwnerSystem: return "empty-owner-system";
		case ArchitectureDiagnosticCode::MissingWorkerAccess: return "missing-worker-access";
		case ArchitectureDiagnosticCode::MainThreadProtected: return "main-thread-protected";
		case ArchitectureDiagnosticCode::ProtectedFallback: return "protected-fallback";
		case ArchitectureDiagnosticCode::AccessConflict: return "access-conflict";
		case ArchitectureDiagnosticCode::MutationRequiresBarrier: return "mutation-requires-barrier";
		case ArchitectureDiagnosticCode::ExecutionFailed: return "execution-failed";
		case ArchitectureDiagnosticCode::CompatibilityFallback: return "compatibility-fallback";
		case ArchitectureDiagnosticCode::ActiveMembershipInvalidated: return "active-membership-invalidated";
		case ArchitectureDiagnosticCode::CompactReferenceStale: return "compact-reference-stale";
		case ArchitectureDiagnosticCode::HierarchySnapshotBlocked: return "hierarchy-snapshot-blocked";
		case ArchitectureDiagnosticCode::ComponentAccessGrantDenied: return "component-access-grant-denied";
		case ArchitectureDiagnosticCode::None:
		default:
			return "none";
		}
	}

	std::string FormatArchitectureDiagnostics(const std::vector<ArchitectureDiagnostic>& diagnostics)
	{
		if (diagnostics.empty())
		{
			return "Architecture diagnostics: none";
		}
		std::ostringstream stream;
		stream << "Architecture diagnostics:";
		for (const ArchitectureDiagnostic& diagnostic : diagnostics)
		{
			stream << "\n- " << ToString(diagnostic.severity)
				<< "/" << ToString(diagnostic.code)
				<< " workload='" << diagnostic.workloadName
				<< "' resource='" << diagnostic.resourceName
				<< "': " << diagnostic.message
				<< " Action: " << diagnostic.recommendedAction;
		}
		return stream.str();
	}

	std::string FormatArchitectureMigrationSummary(const ArchitectureMigrationSummary& summary)
	{
		std::ostringstream stream;
		stream << "Architecture migration summary:"
			<< " workerSafe=" << summary.workerSafeCount
			<< " protected=" << summary.protectedCount
			<< " inline=" << summary.inlineCount
			<< " rejected=" << summary.rejectedCount
			<< " fallbacks=" << summary.fallbackCount
			<< " mutationFlushes=" << summary.mutationFlushCount
			<< " workerPhases=" << summary.workerPhaseCount
			<< " protectedCommits=" << summary.protectedCommitCount
			<< " barriers=" << summary.barrierCount
			<< " hardBoundaries=" << summary.hardBoundaryCount
			<< " partialBoundaries=" << summary.partiallyWorkerSafeBoundaryCount
			<< " protectedBaselineNs=" << summary.protectedBaselineTimeNs
			<< " workerPrepNs=" << summary.workerPreparationTimeNs
			<< " protectedCommitNs=" << summary.protectedCommitTimeNs
			<< " coordinatorWaitNs=" << summary.coordinatorWaitTimeNs
			<< " workerUtilization=" << summary.workerUtilization
			<< " activeInvalidations=" << summary.activeMembershipInvalidationCount
			<< " staleReferences=" << summary.staleObjectHandleCount
			<< " hierarchyRebuilds=" << summary.hierarchySnapshotRebuildCount
			<< " grantDenials=" << summary.componentAccessGrantDeniedCount;
		for (const std::string& reason : summary.fallbackReasons)
		{
			stream << " [" << reason << "]";
		}
		for (const std::string& category : summary.workerCategories)
		{
			stream << " [worker:" << category << "]";
		}
		for (const std::string& category : summary.protectedCategories)
		{
			stream << " [protected:" << category << "]";
		}
		for (const std::string& category : summary.rejectedCategories)
		{
			stream << " [rejected:" << category << "]";
		}
		for (const std::string& finding : summary.hardBoundaryFindings)
		{
			stream << " [boundary:" << finding << "]";
		}
		return stream.str();
	}

	std::string FormatWorkerCoverageReport(const WorkerCoverageReport& report)
	{
		std::ostringstream stream;
		stream << "Worker coverage report '" << report.frameId << "':"
			<< " worker=" << report.workerCategories.size()
			<< " protected=" << report.protectedCategories.size()
			<< " rejected=" << report.rejectedCategories.size()
			<< " fallback=" << report.fallbackCategories.size()
			<< " barriers=" << report.barrierCount
			<< " protectedTimeNs=" << report.protectedTimeNs
			<< " workerTimeNs=" << report.workerTimeNs;
		for (const std::string& category : report.workerCategories)
		{
			stream << " [worker:" << category << "]";
		}
		for (const std::string& category : report.protectedCategories)
		{
			stream << " [protected:" << category << "]";
		}
		for (const std::string& category : report.rejectedCategories)
		{
			stream << " [rejected:" << category << "]";
		}
		for (const std::string& category : report.fallbackCategories)
		{
			stream << " [fallback:" << category << "]";
		}
		for (const std::string& finding : report.hardBoundaryFindings)
		{
			stream << " [boundary:" << finding << "]";
		}
		return stream.str();
	}

	FrameWorkloadDeclaration MakeSnapshotPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::string profilingLabel)
	{
		FrameWorkloadDeclaration declaration;
		declaration.workloadName = std::move(workloadName);
		declaration.ownerSystem = std::move(ownerSystem);
		declaration.phase = "Snapshot";
		declaration.reads = std::move(reads);
		declaration.workerEligible = true;
		declaration.mainThreadRequired = false;
		declaration.phaseKind = WorkerSystemPhaseKind::Snapshot;
		declaration.profilingLabel = profilingLabel.empty() ? declaration.workloadName : std::move(profilingLabel);
		return declaration;
	}

	FrameWorkloadDeclaration MakeWorkerPreparePhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::vector<std::string> writes, std::string profilingLabel)
	{
		FrameWorkloadDeclaration declaration;
		declaration.workloadName = std::move(workloadName);
		declaration.ownerSystem = std::move(ownerSystem);
		declaration.phase = "WorkerPrepare";
		declaration.reads = std::move(reads);
		declaration.writes = std::move(writes);
		declaration.workerEligible = true;
		declaration.mainThreadRequired = false;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.profilingLabel = profilingLabel.empty() ? declaration.workloadName : std::move(profilingLabel);
		return declaration;
	}

	FrameWorkloadDeclaration MakeProtectedCommitPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::vector<std::string> writes, std::string protectedReason, std::string profilingLabel)
	{
		FrameWorkloadDeclaration declaration;
		declaration.workloadName = std::move(workloadName);
		declaration.ownerSystem = std::move(ownerSystem);
		declaration.phase = "ProtectedCommit";
		declaration.reads = std::move(reads);
		declaration.writes = std::move(writes);
		declaration.workerEligible = false;
		declaration.mainThreadRequired = true;
		declaration.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		declaration.protectedReason = std::move(protectedReason);
		declaration.profilingLabel = profilingLabel.empty() ? declaration.workloadName : std::move(profilingLabel);
		return declaration;
	}

	FrameWorkloadDeclaration MakeBarrierPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> resources, std::string protectedReason, std::string profilingLabel)
	{
		FrameWorkloadDeclaration declaration = MakeProtectedCommitPhase(std::move(workloadName), std::move(ownerSystem), resources, resources, std::move(protectedReason), std::move(profilingLabel));
		declaration.phase = "Barrier";
		declaration.phaseKind = WorkerSystemPhaseKind::Barrier;
		declaration.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
		return declaration;
	}

	FrameWorkloadDeclaration MakeFallbackPhase(std::string workloadName, std::string ownerSystem, std::vector<std::string> reads, std::vector<std::string> writes, std::string fallbackReason, std::string profilingLabel)
	{
		FrameWorkloadDeclaration declaration = MakeProtectedCommitPhase(std::move(workloadName), std::move(ownerSystem), std::move(reads), std::move(writes), fallbackReason, std::move(profilingLabel));
		declaration.phaseKind = WorkerSystemPhaseKind::Fallback;
		declaration.fallbackReason = declaration.protectedReason;
		return declaration;
	}

	std::vector<HardBoundary> InitialWorkerHardBoundaries()
	{
		return {
			{
				"ManagedGameplayCallbacks",
				HardBoundaryClassification::PartiallyWorkerSafe,
				{ "managed-runtime-affinity", "reflection-mutation", "immediate-structural-mutation" },
				"Native batch preparation, immutable dispatch inputs, generation validation, and deferred mutation capture.",
				"Arbitrary managed callback invocation, assembly reload/lifecycle callbacks, and immediate native/reflection API calls.",
				"Repeatable managed runtime validation before moving invocation itself to workers."
			},
			{
				"LiveGpuSubmission",
				HardBoundaryClassification::PartiallyWorkerSafe,
				{ "external-device-queue", "swapchain-affinity", "frame-fence-ownership" },
				"Render snapshots, draw/resource packets, secondary command buffers, and submit description assembly.",
				"Swapchain acquire, queue submit, present, frame fence/semaphore lifecycle, editor UI present, and recovery.",
				"Backend-specific proof that queue and presentation ownership can safely move off the coordinator."
			},
			{
				"WindowInputPolling",
				HardBoundaryClassification::Protected,
				{ "os-event-source", "backend-callback-ingestion", "cursor-mutation" },
				"Immutable post-poll input/window snapshots for worker readers.",
				"Platform event polling, close/resize handling, callback ingestion, cursor mutation, and swap-buffer calls.",
				"Platform backend contract that permits event polling away from the coordinator."
			},
			{
				"EditorReflectionMutation",
				HardBoundaryClassification::Protected,
				{ "editor-lifetime", "reflection-mutation", "undo-stack", "selection-side-effects" },
				"Target and payload validation against immutable snapshots.",
				"Reflected setters, component add/remove, object metadata/parent changes, undo stack updates, dirty markers, and selection state.",
				"Editor transaction model that can publish reflected mutations from a deterministic protected commit."
			}
		};
	}

	std::vector<ConversionFinding> InitialWorkerConversionFindings()
	{
		std::vector<ConversionFinding> findings;
		for (const HardBoundary& boundary : InitialWorkerHardBoundaries())
		{
			ConversionFinding finding;
			finding.targetName = boundary.boundaryName;
			finding.findingStatus = boundary.currentClassification == HardBoundaryClassification::PartiallyWorkerSafe
				? ConversionFindingStatus::PartiallyWorkerSafe
				: ConversionFindingStatus::Protected;
			finding.evidence = boundary.safeSubwork;
			finding.blockingReason = boundary.protectedCommit;
			finding.recommendedAction = boundary.requiredEvidence;
			finding.lastValidatedScenario = "019-worker-system-conversion initial boundary audit";
			findings.push_back(std::move(finding));
		}
		return findings;
	}

	WorkerCoverageReport BuildWorkerCoverageReport(const FrameExecutionPlan& plan)
	{
		WorkerCoverageReport report;
		report.frameId = plan.frameId;
		report.workerCategories = plan.migrationSummary.workerCategories;
		report.protectedCategories = plan.migrationSummary.protectedCategories;
		report.rejectedCategories = plan.migrationSummary.rejectedCategories;
		report.fallbackCategories = plan.migrationSummary.fallbackReasons;
		report.hardBoundaryFindings = plan.migrationSummary.hardBoundaryFindings;
		report.barrierCount = plan.migrationSummary.barrierCount;
		report.protectedTimeNs = plan.migrationSummary.protectedBaselineTimeNs + plan.migrationSummary.protectedCommitTimeNs;
		report.workerTimeNs = plan.migrationSummary.workerPreparationTimeNs;
		report.formattedSummary = FormatWorkerCoverageReport(report);
		return report;
	}

	std::string FormatFrameExecutionPlan(const FrameExecutionPlan& plan)
	{
		std::ostringstream stream;
		stream << "Frame execution plan '" << plan.frameId << "': "
			<< (plan.valid ? "valid" : "invalid") << ", "
			<< (plan.completed ? "completed" : "incomplete")
			<< ", mode=" << ToString(plan.executionMode)
			<< ", parallel=" << (plan.parallelExecutionUsed ? "true" : "false") << '\n';
		stream << "Workloads:";
		for (const FrameWorkloadResult& result : plan.results)
		{
			stream << " [" << result.declaration.workloadName
				<< " owner=" << result.declaration.ownerSystem
				<< " phase=" << result.declaration.phase
				<< " kind=" << ToString(result.declaration.phaseKind)
				<< " policy=" << ToString(result.executionPolicy)
				<< " completed=" << (result.completed ? "true" : "false")
				<< "]";
		}
		stream << '\n' << plan.migrationSummary.formatted << '\n';
		stream << FormatArchitectureDiagnostics(plan.diagnostics);
		return stream.str();
	}

	FrameExecutionPlan ValidateFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId)
	{
		FrameExecutionPlan plan;
		plan.frameId = std::move(frameId);
		plan.workloads = std::move(workloads);
		std::stable_sort(plan.workloads.begin(), plan.workloads.end(), [](const FrameWorkloadDeclaration& left, const FrameWorkloadDeclaration& right) {
			return std::tie(left.executionOrder, left.phase, left.workloadName, left.ownerSystem) < std::tie(right.executionOrder, right.phase, right.workloadName, right.ownerSystem);
		});

		for (FrameWorkloadDeclaration& declaration : plan.workloads)
		{
			declaration.reads = NormalizeList(std::move(declaration.reads));
			declaration.writes = NormalizeList(std::move(declaration.writes));
			if (declaration.workloadName.empty())
			{
				AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::EmptyWorkloadName, "", "", "Frame workload has no stable name.", "Set workloadName before validation.");
			}
			if (declaration.ownerSystem.empty())
			{
				AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::EmptyOwnerSystem, declaration.workloadName, "", "Frame workload has no owning system.", "Set ownerSystem to the stable SystemsBase name.");
			}
			FrameWorkloadResult result;
			result.declaration = declaration;
			result.executionPolicy = SelectPolicy(declaration, plan.diagnostics);
			result.fallbackUsed = result.executionPolicy == FrameWorkloadExecutionPolicy::Protected && !declaration.workerEligible;
			plan.results.push_back(std::move(result));
		}

		for (std::size_t leftIndex = 0; leftIndex < plan.results.size(); ++leftIndex)
		{
			for (std::size_t rightIndex = leftIndex + 1; rightIndex < plan.results.size(); ++rightIndex)
			{
				const FrameWorkloadResult& left = plan.results[leftIndex];
				const FrameWorkloadResult& right = plan.results[rightIndex];
				if (left.executionPolicy != FrameWorkloadExecutionPolicy::WorkerSafe || right.executionPolicy != FrameWorkloadExecutionPolicy::WorkerSafe)
				{
					continue;
				}
				if (left.declaration.executionOrder != 0
					&& right.declaration.executionOrder != 0
					&& left.declaration.executionOrder != right.declaration.executionOrder)
				{
					continue;
				}
				std::string resourceName;
				if (HasWriteConflict(left.declaration, right.declaration, resourceName))
				{
					AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::AccessConflict, left.declaration.workloadName + " <-> " + right.declaration.workloadName, resourceName, "Worker-safe workloads have unordered write conflict.", "Add graph ordering or keep one workload protected.");
					plan.results[leftIndex].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
					plan.results[rightIndex].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
				}
			}
		}

		SortDiagnostics(plan.diagnostics);
		plan.valid = std::none_of(plan.diagnostics.begin(), plan.diagnostics.end(), IsError);
		RefreshMigrationSummary(plan);
		plan.formatted = FormatFrameExecutionPlan(plan);
		return plan;
	}

	FrameExecutionPlan ExecuteFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId)
	{
		FrameExecutionPlan plan = ValidateFrameWorkloads(std::move(workloads), std::move(frameId));
		plan.executionMode = FrameWorkloadExecutionMode::Parallel;
		if (!plan.valid)
		{
			return plan;
		}
		ApplyInlinePolicy(plan);
		const bool hasWorkerWork = std::any_of(plan.results.begin(), plan.results.end(), [](const FrameWorkloadResult& result) {
			return result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe;
		});
		if (hasWorkerWork && ThreadPool::instance().getThreadCount() == 0)
		{
			AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Warning, ArchitectureDiagnosticCode::CompatibilityFallback, plan.frameId, "ThreadPool", "Parallel frame execution requested worker work before the ThreadPool was initialized.", "Initialize ThreadPool before executing worker-safe frame work, or call ExecuteFrameWorkloadsCompatibility for an explicit serial baseline.");
			plan = ExecuteFrameWorkloadsCompatibility(std::move(plan.workloads), std::move(plan.frameId));
			return plan;
		}

		JobGraph graph(plan.frameId, "Architecture Hardened Frame");
		std::vector<DependencyHandle> handles;
		handles.reserve(plan.results.size());
		for (FrameWorkloadResult& result : plan.results)
		{
			JobGraphJobDeclaration job;
			job.name = result.declaration.workloadName;
			job.executionPolicy = result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe
				? JobGraphExecutionPolicy::WorkerEligible
				: result.executionPolicy == FrameWorkloadExecutionPolicy::Inline
				? JobGraphExecutionPolicy::MainThread
				: JobGraphExecutionPolicy::Protected;
			job.cancellable = true;
			job.requiresSuccessfulDependencies = true;
			job.mayMutateSceneStructure = result.declaration.mutationPolicy == FrameWorkloadMutationPolicy::DeferredOnly;
			job.profilingLabel = result.declaration.profilingLabel.empty() ? result.declaration.workloadName : result.declaration.profilingLabel;
			job.work = result.declaration.work;
			handles.push_back(graph.AddJob(std::move(job)));
		}

		for (std::size_t leftIndex = 0; leftIndex < plan.results.size(); ++leftIndex)
		{
			for (std::size_t rightIndex = leftIndex + 1; rightIndex < plan.results.size(); ++rightIndex)
			{
				if (plan.results[leftIndex].declaration.executionOrder != 0
					&& plan.results[rightIndex].declaration.executionOrder != 0
					&& plan.results[leftIndex].declaration.executionOrder < plan.results[rightIndex].declaration.executionOrder)
				{
					graph.AddDependency(handles[leftIndex], handles[rightIndex]);
					continue;
				}
				std::string resourceName;
				if (HasWriteConflict(plan.results[leftIndex].declaration, plan.results[rightIndex].declaration, resourceName))
				{
					graph.AddDependency(handles[leftIndex], handles[rightIndex]);
				}
			}
		}

		const JobGraphSummary& summary = graph.Execute();
		plan.valid = summary.valid;
		for (FrameWorkloadResult& result : plan.results)
		{
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe)
			{
				plan.parallelExecutionUsed = true;
			}
		}
		ApplyJobGraphSummary(plan, summary);
		if (!summary.valid || !plan.completed)
		{
			AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::ExecutionFailed, plan.frameId, "", "Frame workload graph did not complete successfully.", "Inspect JobGraph diagnostics.");
		}
		SortDiagnostics(plan.diagnostics);
		RefreshMigrationSummary(plan);
		plan.formatted = FormatFrameExecutionPlan(plan);
		return plan;
	}

	FrameExecutionPlan ExecuteFrameWorkloadsCompatibility(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId)
	{
		FrameExecutionPlan plan = ValidateFrameWorkloads(std::move(workloads), std::move(frameId));
		plan.executionMode = FrameWorkloadExecutionMode::Compatibility;
		plan.parallelExecutionUsed = false;
		if (!plan.valid)
		{
			return plan;
		}
		ApplyInlinePolicy(plan);

		JobGraphContext context;
		for (FrameWorkloadResult& result : plan.results)
		{
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
			{
				result.completed = false;
				continue;
			}
			try
			{
				if (result.declaration.work)
				{
					TERRAENGINE_PROFILE_TASK_SCOPE("FrameWorkload::CompatibilityJob", TerraEngine::Profiling::ProfilerModuleId::CpuEngine, result.declaration.workloadName.c_str());
					result.declaration.work(context);
				}
				result.completed = true;
			}
			catch (...)
			{
				result.completed = false;
				AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::ExecutionFailed, result.declaration.workloadName, "", "Frame workload callback failed during compatibility execution.", "Fix the workload body or keep the owning system protected.");
			}
		}
		plan.completed = std::all_of(plan.results.begin(), plan.results.end(), [](const FrameWorkloadResult& result) {
			return result.completed || result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected;
		});
		if (!plan.completed)
		{
			AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::ExecutionFailed, plan.frameId, "", "Frame workload compatibility pass did not complete successfully.", "Inspect workload diagnostics.");
		}
		SortDiagnostics(plan.diagnostics);
		RefreshMigrationSummary(plan);
		plan.formatted = FormatFrameExecutionPlan(plan);
		return plan;
	}
}
