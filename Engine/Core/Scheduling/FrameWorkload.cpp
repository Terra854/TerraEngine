// FrameWorkload.cpp - validation and execution for architecture-hardening work.

#include "FrameWorkload.h"

#include "JobGraph.h"
#include "Profiling/ProfilerScope.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		std::mutex g_ProfilingExecutionRecordsMutex;
		std::vector<TerraEngine::Profiling::WorkloadExecutionRecord> g_ProfilingExecutionRecords;

		void PublishExecutionRecordsForProfiling(const FrameExecutionPlan& plan)
		{
			std::lock_guard<std::mutex> lock(g_ProfilingExecutionRecordsMutex);
			g_ProfilingExecutionRecords = plan.executionRecords;
		}

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

		bool SortedListsIntersect(
			const std::vector<std::string>& left,
			const std::vector<std::string>& right,
			std::string& matchedValue)
		{
			// Validation normalizes resource names before conflict checks. A linear
			// merge therefore finds intersections without allocating four tree sets
			// for every workload pair.
			std::size_t leftIndex = 0u;
			std::size_t rightIndex = 0u;
			while (leftIndex < left.size() && rightIndex < right.size())
			{
				if (left[leftIndex] < right[rightIndex])
				{
					++leftIndex;
				}
				else if (right[rightIndex] < left[leftIndex])
				{
					++rightIndex;
				}
				else
				{
					matchedValue = left[leftIndex];
					return true;
				}
			}
			return false;
		}

		bool HasWriteConflict(const FrameWorkloadDeclaration& left, const FrameWorkloadDeclaration& right, std::string& resourceName)
		{
			return SortedListsIntersect(left.writes, right.writes, resourceName)
				|| SortedListsIntersect(left.writes, right.reads, resourceName)
				|| SortedListsIntersect(right.writes, left.reads, resourceName);
		}

		FrameWorkloadExecutionPolicy SelectPolicy(const FrameWorkloadDeclaration& declaration, std::vector<ArchitectureDiagnostic>& diagnostics)
		{
			if (!declaration.active)
			{
				AddArchitectureDiagnostic(diagnostics, ArchitectureDiagnosticSeverity::Info, ArchitectureDiagnosticCode::InactiveWorkloadOmitted, declaration.workloadName, declaration.activationPredicateKey, "Inactive workload descriptor was omitted before graph construction.", "No action is required unless the activation predicate should have selected this workload.");
				return FrameWorkloadExecutionPolicy::Omitted;
			}
			if (!declaration.ready)
			{
				const std::string reason = declaration.readinessReason.empty() ? "required immutable input is not ready" : declaration.readinessReason;
				AddArchitectureDiagnostic(diagnostics, ArchitectureDiagnosticSeverity::Info, ArchitectureDiagnosticCode::WorkloadNotReady, declaration.workloadName, "", "Workload descriptor was omitted because " + reason + ".", "Publish the required immutable input before activating the workload.");
				return FrameWorkloadExecutionPolicy::Omitted;
			}
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

		void RefreshMigrationSummary(FrameExecutionPlan& plan, bool formatSummary = true)
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
				case FrameWorkloadExecutionPolicy::Omitted:
					++summary.omittedCount;
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
			if (formatSummary)
			{
				summary.formatted = FormatArchitectureMigrationSummary(summary);
			}
			plan.migrationSummary = std::move(summary);
		}

		struct ScheduledPartition
		{
			std::size_t resultIndex = 0u;
			FrameWorkloadPartition partition;
			std::string jobName;
		};

		TerraEngine::Profiling::WorkloadExecutionPath ToTelemetryPath(FrameWorkloadExecutionPolicy policy)
		{
			switch (policy)
			{
			case FrameWorkloadExecutionPolicy::Inline:
				return TerraEngine::Profiling::WorkloadExecutionPath::Inline;
			case FrameWorkloadExecutionPolicy::WorkerSafe:
				return TerraEngine::Profiling::WorkloadExecutionPath::Worker;
			case FrameWorkloadExecutionPolicy::Protected:
				return TerraEngine::Profiling::WorkloadExecutionPath::Protected;
			case FrameWorkloadExecutionPolicy::Rejected:
				return TerraEngine::Profiling::WorkloadExecutionPath::Rejected;
			case FrameWorkloadExecutionPolicy::Omitted:
			default:
				return TerraEngine::Profiling::WorkloadExecutionPath::None;
			}
		}

		TerraEngine::Profiling::WorkloadExecutionStatus ToTelemetryStatus(const FrameWorkloadResult& result, bool completed)
		{
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
			{
				return TerraEngine::Profiling::WorkloadExecutionStatus::Rejected;
			}
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted)
			{
				return result.declaration.active
					? TerraEngine::Profiling::WorkloadExecutionStatus::NotReady
					: TerraEngine::Profiling::WorkloadExecutionStatus::Inactive;
			}
			if (completed)
			{
				return TerraEngine::Profiling::WorkloadExecutionStatus::Completed;
			}
			return TerraEngine::Profiling::WorkloadExecutionStatus::Cancelled;
		}

		void AppendExecutionRecord(
			FrameExecutionPlan& plan,
			const FrameWorkloadResult& result,
			const FrameWorkloadPartition& partition,
			const JobGraphResultEntry* entry,
			bool completed)
		{
			if (plan.executionRecords.size() >= TerraEngine::Profiling::kMaximumWorkloadExecutionRecordsPerFrame)
			{
				++plan.droppedExecutionRecordCount;
				return;
			}

			// This record intentionally copies only decision evidence. In particular it
			// never retains the descriptor's callbacks or per-frame snapshot inputs.
			TerraEngine::Profiling::WorkloadExecutionRecord record;
			const TerraEngine::Profiling::TelemetryDuration unavailableDuration{
				TerraEngine::Profiling::TelemetryAvailability::Unavailable,
				0u,
				"The workload path did not measure this duration."
			};
			record.frameId = plan.frameId;
			record.descriptorId = result.declaration.stableDescriptorId;
			record.descriptorGeneration = result.declaration.stableDescriptorGeneration;
			record.ownerSystem = result.declaration.ownerSystem;
			record.phase = result.declaration.phase;
			record.workloadName = result.declaration.workloadName;
			record.activationPredicateKey = result.declaration.activationPredicateKey;
			record.active = result.declaration.active;
			record.ready = result.declaration.ready;
			record.selectedPath = ToTelemetryPath(result.executionPolicy);
			record.status = ToTelemetryStatus(result, completed);
			record.partitionIndex = partition.partitionIndex;
			record.partitionCount = partition.partitionCount;
			record.itemBegin = partition.itemBegin;
			record.itemEnd = partition.itemEnd;
			record.sameFrameVisibilityRequired = result.declaration.sameFrameVisibilityRequired;
			record.queueDuration = unavailableDuration;
			record.runDuration = unavailableDuration;
			record.coordinatorWaitDuration = unavailableDuration;
			record.ownershipWaitDuration = unavailableDuration;
			record.flushDuration = unavailableDuration;
			record.publicationDuration = unavailableDuration;
			if (entry != nullptr)
			{
				record.dispatchCount = entry->dispatchCount;
				record.usefulWorkItemCount = entry->usefulWorkItemCount;
				record.queueDuration.availability = TerraEngine::Profiling::TelemetryAvailability::Available;
				record.queueDuration.nanoseconds = entry->queueDurationNs;
				record.queueDuration.unavailableReason.clear();
				record.runDuration.availability = TerraEngine::Profiling::TelemetryAvailability::Available;
				record.runDuration.nanoseconds = entry->runDurationNs;
				record.runDuration.unavailableReason.clear();
				record.coordinatorWaitDuration.availability = TerraEngine::Profiling::TelemetryAvailability::Available;
				record.coordinatorWaitDuration.nanoseconds = entry->coordinatorWaitDurationNs;
				record.coordinatorWaitDuration.unavailableReason.clear();
			}
			plan.executionRecords.push_back(std::move(record));
		}

		void ApplyJobGraphSummary(
			FrameExecutionPlan& plan,
			const JobGraphSummary& summary,
			const std::vector<ScheduledPartition>& scheduledPartitions,
			bool collectExecutionRecords)
		{
			std::unordered_map<std::string, JobGraphNodeState> statesByName;
			std::unordered_map<std::string, const JobGraphResultEntry*> entriesByName;
			for (const JobGraphResultEntry& entry : summary.jobs)
			{
				statesByName[entry.name] = entry.state;
				entriesByName[entry.name] = &entry;
			}
			for (FrameWorkloadResult& result : plan.results)
			{
				result.completed = result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted;
			}
			std::vector<bool> hasScheduledPartition(plan.results.size(), false);
			std::vector<bool> allScheduledPartitionsCompleted(plan.results.size(), true);
			for (const ScheduledPartition& scheduled : scheduledPartitions)
			{
				FrameWorkloadResult& result = plan.results[scheduled.resultIndex];
				const auto found = statesByName.find(scheduled.jobName);
				const bool completed = found != statesByName.end() && found->second == JobGraphNodeState::Completed;
				hasScheduledPartition[scheduled.resultIndex] = true;
				allScheduledPartitionsCompleted[scheduled.resultIndex] = allScheduledPartitionsCompleted[scheduled.resultIndex] && completed;
				const auto entry = entriesByName.find(scheduled.jobName);
				if (collectExecutionRecords)
				{
					AppendExecutionRecord(plan, result, scheduled.partition,
						entry == entriesByName.end() ? nullptr : entry->second,
						completed);
				}
			}
			for (std::size_t resultIndex = 0; resultIndex < plan.results.size(); ++resultIndex)
			{
				if (hasScheduledPartition[resultIndex])
				{
					// A partitioned descriptor completes only after every deterministic
					// range succeeds; one completed worker range is not a full publication.
					plan.results[resultIndex].completed = allScheduledPartitionsCompleted[resultIndex];
				}
			}
			for (const FrameWorkloadResult& result : plan.results)
			{
				if (result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted || result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
				{
					if (collectExecutionRecords)
					{
						AppendExecutionRecord(plan, result, {}, nullptr,
							result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted);
					}
				}
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
		case FrameWorkloadExecutionPolicy::Omitted:
			return "omitted";
		case FrameWorkloadExecutionPolicy::Rejected:
			return "rejected";
		case FrameWorkloadExecutionPolicy::Protected:
		default:
			return "protected";
		}
	}

	std::string ToString(FrameWorkloadPartitionPolicy policy)
	{
		return policy == FrameWorkloadPartitionPolicy::DeterministicRanges ? "deterministic-ranges" : "single";
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
		case ArchitectureDiagnosticCode::InactiveWorkloadOmitted: return "inactive-workload-omitted";
		case ArchitectureDiagnosticCode::WorkloadNotReady: return "workload-not-ready";
		case ArchitectureDiagnosticCode::DuplicateDescriptorIdentity: return "duplicate-descriptor-identity";
		case ArchitectureDiagnosticCode::MissingExplicitDependency: return "missing-explicit-dependency";
		case ArchitectureDiagnosticCode::ExplicitDependencyCycle: return "explicit-dependency-cycle";
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
			<< " omitted=" << summary.omittedCount
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
				<< " mutation=" << ToString(result.declaration.mutationPolicy)
				<< " descriptor=" << result.declaration.stableDescriptorId << ":" << result.declaration.stableDescriptorGeneration
				<< " active=" << (result.declaration.active ? "true" : "false")
				<< " ready=" << (result.declaration.ready ? "true" : "false")
				<< " partitions=" << result.partitionCount
				<< " completed=" << (result.completed ? "true" : "false")
				<< "]";
		}
		stream << '\n' << plan.migrationSummary.formatted << '\n';
		stream << FormatArchitectureDiagnostics(plan.diagnostics);
		return stream.str();
	}

	std::vector<FrameWorkloadPartition> BuildDeterministicWorkloadPartitions(std::size_t itemCount, std::size_t targetItemsPerPartition, std::size_t maximumPartitionCount)
	{
		std::vector<FrameWorkloadPartition> partitions;
		if (itemCount == 0)
		{
			return partitions;
		}

		// A zero target explicitly means one range. This makes an incompletely
		// tuned descriptor safe without multiplying dispatch overhead.
		const std::size_t safeTarget = targetItemsPerPartition == 0 ? itemCount : targetItemsPerPartition;
		std::size_t partitionCount = (itemCount + safeTarget - 1u) / safeTarget;
		if (maximumPartitionCount > 0)
		{
			partitionCount = std::min(partitionCount, maximumPartitionCount);
		}
		partitionCount = std::max<std::size_t>(1u, partitionCount);
		partitions.reserve(partitionCount);
		for (std::size_t index = 0; index < partitionCount; ++index)
		{
			// Integer-ratio boundaries distribute any remainder deterministically
			// while covering every item exactly once without overlaps or gaps.
			FrameWorkloadPartition partition;
			partition.partitionIndex = index;
			partition.partitionCount = partitionCount;
			partition.itemBegin = (itemCount * index) / partitionCount;
			partition.itemEnd = (itemCount * (index + 1u)) / partitionCount;
			partitions.push_back(partition);
		}
		return partitions;
	}

	FrameExecutionPlan ValidateFrameWorkloadsInternal(
		std::vector<FrameWorkloadDeclaration> workloads,
		std::string frameId,
		bool formatPlan)
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
			declaration.explicitDependencies = NormalizeList(std::move(declaration.explicitDependencies));
			if (declaration.stableDescriptorId.empty())
			{
				// Legacy system declarations receive a stable derived identity. Systems
				// that cache descriptors can supply their own identity and generation.
				declaration.stableDescriptorId = declaration.ownerSystem + "/" + declaration.phase + "/" + declaration.workloadName;
			}
			if (declaration.stableDescriptorGeneration == 0u)
			{
				declaration.stableDescriptorGeneration = 1u;
			}
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
			result.omitted = result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted;
			plan.results.push_back(std::move(result));
		}

		std::unordered_map<std::string, std::size_t> descriptorIndex;
		for (std::size_t index = 0; index < plan.results.size(); ++index)
		{
			const std::string& identity = plan.results[index].declaration.stableDescriptorId;
			const auto [found, inserted] = descriptorIndex.emplace(identity, index);
			if (!inserted)
			{
				AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::DuplicateDescriptorIdentity, identity, "", "Stable workload descriptor identity is declared more than once in this frame.", "Give every cached descriptor a unique stable identity.");
				plan.results[index].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
				plan.results[found->second].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
			}
		}

		// Propagate dependency readiness before graph construction. A descriptor
		// whose prerequisite is inactive is itself omitted, so neither descriptor
		// creates a no-op job or synchronization edge.
		bool readinessChanged = true;
		while (readinessChanged)
		{
			readinessChanged = false;
			for (FrameWorkloadResult& result : plan.results)
			{
				if (result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected || result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted)
				{
					continue;
				}
				for (const std::string& dependencyIdentity : result.declaration.explicitDependencies)
				{
					const auto dependency = descriptorIndex.find(dependencyIdentity);
					if (dependency == descriptorIndex.end())
					{
						AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::MissingExplicitDependency, result.declaration.workloadName, dependencyIdentity, "Explicit workload dependency does not exist in this frame plan.", "Declare the stable dependency descriptor or omit the dependent workload.");
						result.executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
						break;
					}
					if (plan.results[dependency->second].executionPolicy == FrameWorkloadExecutionPolicy::Omitted)
					{
						result.declaration.ready = false;
						result.declaration.readinessReason = "explicit dependency '" + dependencyIdentity + "' is not active or ready";
						result.executionPolicy = FrameWorkloadExecutionPolicy::Omitted;
						result.omitted = true;
						AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Info, ArchitectureDiagnosticCode::WorkloadNotReady, result.declaration.workloadName, dependencyIdentity, "Workload was omitted because an explicit dependency is not active or ready.", "Align dependent activation with the prerequisite or publish the prerequisite input first.");
						readinessChanged = true;
						break;
					}
				}
			}
		}

		// Validate explicit dependency cycles independently from resource edges so
		// a bad cached descriptor is rejected before any callback can run.
		std::vector<std::size_t> dependencyIndegree(plan.results.size(), 0u);
		std::vector<std::vector<std::size_t>> dependencyOutgoing(plan.results.size());
		std::size_t dependencyNodeCount = 0u;
		for (std::size_t index = 0; index < plan.results.size(); ++index)
		{
			const FrameWorkloadResult& result = plan.results[index];
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted || result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
			{
				continue;
			}
			++dependencyNodeCount;
			for (const std::string& dependencyIdentity : result.declaration.explicitDependencies)
			{
				const auto dependency = descriptorIndex.find(dependencyIdentity);
				if (dependency == descriptorIndex.end()
					|| plan.results[dependency->second].executionPolicy == FrameWorkloadExecutionPolicy::Omitted
					|| plan.results[dependency->second].executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
				{
					continue;
				}
				dependencyOutgoing[dependency->second].push_back(index);
				++dependencyIndegree[index];
			}
		}
		std::vector<std::size_t> dependencyReady;
		for (std::size_t index = 0; index < dependencyIndegree.size(); ++index)
		{
			if (plan.results[index].executionPolicy != FrameWorkloadExecutionPolicy::Omitted
				&& plan.results[index].executionPolicy != FrameWorkloadExecutionPolicy::Rejected
				&& dependencyIndegree[index] == 0u)
			{
				dependencyReady.push_back(index);
			}
		}
		std::size_t dependencyVisited = 0u;
		while (!dependencyReady.empty())
		{
			const std::size_t index = dependencyReady.back();
			dependencyReady.pop_back();
			++dependencyVisited;
			for (std::size_t dependent : dependencyOutgoing[index])
			{
				if (--dependencyIndegree[dependent] == 0u)
				{
					dependencyReady.push_back(dependent);
				}
			}
		}
		if (dependencyVisited != dependencyNodeCount)
		{
			AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::ExplicitDependencyCycle, plan.frameId, "", "Explicit workload dependencies contain a cycle.", "Remove or reverse one stable descriptor dependency before executing the frame.");
			for (std::size_t index = 0; index < dependencyIndegree.size(); ++index)
			{
				if (dependencyIndegree[index] > 0u)
				{
					plan.results[index].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
				}
			}
		}
		// Compute reachability once per source. The previous pair-wise DFS allocated
		// scratch vectors for every worker pair, which made validation quadratic in
		// allocations before any actual frame work could begin.
		std::vector<std::vector<bool>> dependencyReachability(
			plan.results.size(), std::vector<bool>(plan.results.size(), false));
		std::vector<std::size_t> pending;
		pending.reserve(plan.results.size());
		for (std::size_t source = 0; source < plan.results.size(); ++source)
		{
			pending.clear();
			pending.insert(pending.end(), dependencyOutgoing[source].begin(), dependencyOutgoing[source].end());
			while (!pending.empty())
			{
				const std::size_t current = pending.back();
				pending.pop_back();
				if (dependencyReachability[source][current])
				{
					continue;
				}
				dependencyReachability[source][current] = true;
				pending.insert(pending.end(), dependencyOutgoing[current].begin(), dependencyOutgoing[current].end());
			}
		}

		// Preserve the already-resolved descriptor indices for graph lowering. This
		// removes a second descriptor map and conflict scan from execution.
		for (std::size_t dependentIndex = 0; dependentIndex < plan.results.size(); ++dependentIndex)
		{
			for (const std::string& dependencyIdentity : plan.results[dependentIndex].declaration.explicitDependencies)
			{
				const auto dependency = descriptorIndex.find(dependencyIdentity);
				if (dependency != descriptorIndex.end())
				{
					plan.dependencyEdges.push_back({ dependency->second, dependentIndex });
				}
			}
		}

		for (std::size_t leftIndex = 0; leftIndex < plan.results.size(); ++leftIndex)
		{
			for (std::size_t rightIndex = leftIndex + 1; rightIndex < plan.results.size(); ++rightIndex)
			{
				const FrameWorkloadResult& left = plan.results[leftIndex];
				const FrameWorkloadResult& right = plan.results[rightIndex];
				// An explicit dependency path already supplies the required ordering.
				// Skipping both direct and transitive paths prevents a redundant
				// alphabetical conflict edge from reversing that dataflow and forming a
				// cycle when the declared path runs from right to left.
				const bool explicitlyOrdered = dependencyReachability[leftIndex][rightIndex]
					|| dependencyReachability[rightIndex][leftIndex];
				if (explicitlyOrdered)
				{
					continue;
				}
				std::string resourceName;
				if (HasWriteConflict(left.declaration, right.declaration, resourceName))
				{
					plan.dependencyEdges.push_back({ leftIndex, rightIndex });
					const bool workerConflict = left.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe
						&& right.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe;
					if (workerConflict)
					{
						AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::AccessConflict, left.declaration.workloadName + " <-> " + right.declaration.workloadName, resourceName, "Worker-safe workloads have unordered write conflict.", "Add graph ordering or keep one workload protected.");
						plan.results[leftIndex].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
						plan.results[rightIndex].executionPolicy = FrameWorkloadExecutionPolicy::Rejected;
					}
				}
			}
		}

		SortDiagnostics(plan.diagnostics);
		plan.valid = std::none_of(plan.diagnostics.begin(), plan.diagnostics.end(), IsError);
		RefreshMigrationSummary(plan, formatPlan);
		if (formatPlan)
		{
			plan.formatted = FormatFrameExecutionPlan(plan);
		}
		return plan;
	}

	FrameExecutionPlan ValidateFrameWorkloads(std::vector<FrameWorkloadDeclaration> workloads, std::string frameId)
	{
		return ValidateFrameWorkloadsInternal(std::move(workloads), std::move(frameId), true);
	}

	FrameExecutionPlan ExecuteFrameWorkloads(
		std::vector<FrameWorkloadDeclaration> workloads,
		std::string frameId,
		FrameWorkloadExecutionOptions options)
	{
		FrameExecutionPlan plan;
		{
			TERRAENGINE_PROFILE_SCOPE("FrameWorkload: Validate and Lower",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			plan = ValidateFrameWorkloadsInternal(
				std::move(workloads), std::move(frameId), false);
		}
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
			plan = ExecuteFrameWorkloadsCompatibility(
				std::move(plan.workloads), std::move(plan.frameId), options);
			return plan;
		}

		// The graph topology changes only when descriptor identity, activation,
		// partition ranges, policy, generation, or dependency edges change. Callback
		// captures are refreshed every frame without invalidating that topology.
		thread_local JobGraph graph("frame-workload", "Architecture Hardened Frame");
		thread_local std::vector<std::vector<DependencyHandle>> handlesByResult;
		thread_local std::vector<ScheduledPartition> scheduledPartitions;
		struct PreparedFrameJob
		{
			std::size_t resultIndex = 0u;
			FrameWorkloadPartition partition{};
			std::uint64_t descriptorGeneration = 0u;
			JobGraphJobDeclaration declaration{};
		};
		struct CachedJobTopology
		{
			std::string name;
			std::size_t resultIndex = 0u;
			std::uint64_t descriptorGeneration = 0u;
			JobGraphExecutionPolicy executionPolicy = JobGraphExecutionPolicy::Protected;
			bool mayMutateSceneStructure = false;
			std::size_t partitionIndex = 0u;
			std::size_t partitionCount = 0u;
			std::size_t itemBegin = 0u;
			std::size_t itemEnd = 0u;

			bool operator==(const CachedJobTopology&) const = default;
		};
		thread_local std::vector<PreparedFrameJob> preparedJobs;
		thread_local std::vector<CachedJobTopology> cachedTopology;
		thread_local std::vector<std::pair<std::size_t, std::size_t>> cachedDependencyEdges;
		thread_local std::vector<DependencyHandle> cachedHandles;
		preparedJobs.clear();
		scheduledPartitions.clear();
		for (std::vector<DependencyHandle>& handles : handlesByResult)
		{
			handles.clear();
		}
		handlesByResult.resize(plan.results.size());

		{
			TERRAENGINE_PROFILE_SCOPE("FrameWorkload: Build Job Graph",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for (std::size_t resultIndex = 0; resultIndex < plan.results.size(); ++resultIndex)
			{
				FrameWorkloadResult& result = plan.results[resultIndex];
				if (result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted
					|| result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
				{
					continue;
				}

				std::vector<FrameWorkloadPartition> partitions;
				if (result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe
					&& result.declaration.partitionPolicy == FrameWorkloadPartitionPolicy::DeterministicRanges
					&& result.declaration.estimatedWorkItemCount > 0u
					&& result.declaration.rangeWork)
				{
					partitions = BuildDeterministicWorkloadPartitions(
						result.declaration.estimatedWorkItemCount,
						result.declaration.partitionWorkItemCount,
						result.declaration.maximumPartitionCount);
				}
				if (partitions.empty())
				{
					partitions.push_back({ 0u, 1u, 0u, result.declaration.estimatedWorkItemCount });
				}
				result.partitionCount = partitions.size();
				for (const FrameWorkloadPartition& partition : partitions)
				{
					PreparedFrameJob prepared{};
					prepared.resultIndex = resultIndex;
					prepared.partition = partition;
					prepared.descriptorGeneration = result.declaration.stableDescriptorGeneration;
					JobGraphJobDeclaration& job = prepared.declaration;
					job.name = result.declaration.stableDescriptorId + "#"
						+ std::to_string(partition.partitionIndex);
					job.executionPolicy = result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe
						? JobGraphExecutionPolicy::WorkerEligible
						: result.executionPolicy == FrameWorkloadExecutionPolicy::Inline
						? JobGraphExecutionPolicy::MainThread
						: JobGraphExecutionPolicy::Protected;
					job.cancellable = true;
					job.requiresSuccessfulDependencies = true;
					job.mayMutateSceneStructure = result.declaration.mutationPolicy
						== FrameWorkloadMutationPolicy::DeferredOnly;
					job.profilingLabel = result.declaration.profilingLabel.empty()
						? result.declaration.workloadName : result.declaration.profilingLabel;
					job.partitionIndex = partition.partitionIndex;
					job.partitionCount = partition.partitionCount;
					job.itemBegin = partition.itemBegin;
					job.itemEnd = partition.itemEnd;
					job.usefulWorkItemCount = partition.itemEnd - partition.itemBegin;
					if (result.declaration.rangeWork)
					{
						job.work = [rangeWork = result.declaration.rangeWork, partition](JobGraphContext& context) {
							rangeWork(partition, context);
						};
					}
					else
					{
						job.work = result.declaration.work;
					}
					preparedJobs.push_back(std::move(prepared));
					scheduledPartitions.push_back({ resultIndex, partition,
						preparedJobs.back().declaration.name });
				}
			}

			std::vector<CachedJobTopology> topology;
			topology.reserve(preparedJobs.size());
			for (const PreparedFrameJob& prepared : preparedJobs)
			{
				const JobGraphJobDeclaration& job = prepared.declaration;
				topology.push_back({ job.name, prepared.resultIndex,
					prepared.descriptorGeneration, job.executionPolicy,
					job.mayMutateSceneStructure, job.partitionIndex,
					job.partitionCount, job.itemBegin, job.itemEnd });
			}
			std::vector<std::pair<std::size_t, std::size_t>> dependencyEdges;
			dependencyEdges.reserve(plan.dependencyEdges.size());
			for (const FrameWorkloadDependencyEdge& edge : plan.dependencyEdges)
			{
				dependencyEdges.emplace_back(edge.prerequisiteIndex, edge.dependentIndex);
			}

			const bool reuseTopology = topology == cachedTopology
				&& dependencyEdges == cachedDependencyEdges
				&& cachedHandles.size() == preparedJobs.size();
			if (reuseTopology)
			{
				graph.Rearm(plan.frameId);
				for (std::size_t index = 0u; index < preparedJobs.size(); ++index)
				{
					const PreparedFrameJob& prepared = preparedJobs[index];
					graph.UpdateJobWork(cachedHandles[index],
						prepared.declaration.work,
						prepared.declaration.profilingLabel);
					handlesByResult[prepared.resultIndex].push_back(cachedHandles[index]);
				}
			}
			else
			{
				graph.Reset(plan.frameId, "Architecture Hardened Frame");
				cachedHandles.clear();
				cachedHandles.reserve(preparedJobs.size());
				for (PreparedFrameJob& prepared : preparedJobs)
				{
					const DependencyHandle handle = graph.AddJob(
						std::move(prepared.declaration));
					cachedHandles.push_back(handle);
					handlesByResult[prepared.resultIndex].push_back(handle);
				}
				for (const FrameWorkloadDependencyEdge& edge : plan.dependencyEdges)
				{
					if (edge.prerequisiteIndex >= handlesByResult.size()
						|| edge.dependentIndex >= handlesByResult.size())
					{
						continue;
					}
					for (const DependencyHandle& before : handlesByResult[edge.prerequisiteIndex])
					{
						for (const DependencyHandle& after : handlesByResult[edge.dependentIndex])
						{
							graph.AddDependency(before, after);
						}
					}
				}
				cachedTopology = std::move(topology);
				cachedDependencyEdges = std::move(dependencyEdges);
			}
		}

		JobGraphExecuteOptions executeOptions;
		executeOptions.formatSummary = false;
		const JobGraphSummary* summaryPointer = nullptr;
		{
			TERRAENGINE_PROFILE_SCOPE("FrameWorkload: Execute Job Graph",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			summaryPointer = &graph.Execute(executeOptions);
		}
		const JobGraphSummary& summary = *summaryPointer;
		plan.valid = summary.valid;
		for (FrameWorkloadResult& result : plan.results)
		{
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::WorkerSafe)
			{
				plan.parallelExecutionUsed = true;
			}
		}
		ApplyJobGraphSummary(plan, summary, scheduledPartitions,
			options.collectExecutionRecords);
		if (!summary.valid || !plan.completed)
		{
			AddArchitectureDiagnostic(plan.diagnostics, ArchitectureDiagnosticSeverity::Error, ArchitectureDiagnosticCode::ExecutionFailed, plan.frameId, "", "Frame workload graph did not complete successfully.", "Inspect JobGraph diagnostics.");
		}
		{
			TERRAENGINE_PROFILE_SCOPE("FrameWorkload: Publish Execution Diagnostics",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			SortDiagnostics(plan.diagnostics);
			RefreshMigrationSummary(plan, options.formatDiagnostics);
			plan.formatted = options.formatDiagnostics
				? FormatFrameExecutionPlan(plan) : std::string{};
			if (options.collectExecutionRecords)
			{
				PublishExecutionRecordsForProfiling(plan);
			}
		}
		return plan;
	}

	FrameExecutionPlan ExecuteFrameWorkloadsCompatibility(
		std::vector<FrameWorkloadDeclaration> workloads,
		std::string frameId,
		FrameWorkloadExecutionOptions options)
	{
		FrameExecutionPlan plan = ValidateFrameWorkloadsInternal(std::move(workloads), std::move(frameId), false);
		plan.executionMode = FrameWorkloadExecutionMode::Compatibility;
		plan.parallelExecutionUsed = false;
		if (!plan.valid)
		{
			return plan;
		}
		ApplyInlinePolicy(plan);

		JobGraphContext context;
		std::unordered_map<std::string, std::size_t> resultIndexByDescriptor;
		for (std::size_t index = 0; index < plan.results.size(); ++index)
		{
			resultIndexByDescriptor.emplace(plan.results[index].declaration.stableDescriptorId, index);
		}
		std::vector<std::size_t> indegree(plan.results.size(), 0U);
		std::vector<std::vector<std::size_t>> outgoing(plan.results.size());
		for (std::size_t index = 0; index < plan.results.size(); ++index)
		{
			for (const std::string& dependencyId : plan.results[index].declaration.explicitDependencies)
			{
				const auto dependency = resultIndexByDescriptor.find(dependencyId);
				if (dependency != resultIndexByDescriptor.end())
				{
					outgoing[dependency->second].push_back(index);
					++indegree[index];
				}
			}
		}
		std::vector<std::size_t> executionOrder;
		executionOrder.reserve(plan.results.size());
		while (executionOrder.size() < plan.results.size())
		{
			const auto ready = std::find(indegree.begin(), indegree.end(), 0U);
			if (ready == indegree.end()) break;
			const std::size_t index = static_cast<std::size_t>(std::distance(indegree.begin(), ready));
			indegree[index] = std::numeric_limits<std::size_t>::max();
			executionOrder.push_back(index);
			for (const std::size_t dependent : outgoing[index])
			{
				if (indegree[dependent] != std::numeric_limits<std::size_t>::max() && indegree[dependent] > 0U)
				{
					--indegree[dependent];
				}
			}
		}

		for (const std::size_t resultIndex : executionOrder)
		{
			FrameWorkloadResult& result = plan.results[resultIndex];
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::Omitted)
			{
				result.completed = true;
				if (options.collectExecutionRecords)
				{
					AppendExecutionRecord(plan, result, {}, nullptr, true);
				}
				continue;
			}
			if (result.executionPolicy == FrameWorkloadExecutionPolicy::Rejected)
			{
				result.completed = false;
				if (options.collectExecutionRecords)
				{
					AppendExecutionRecord(plan, result, {}, nullptr, false);
				}
				continue;
			}
			std::vector<FrameWorkloadPartition> partitions;
			if (result.declaration.partitionPolicy == FrameWorkloadPartitionPolicy::DeterministicRanges
				&& result.declaration.estimatedWorkItemCount > 0u
				&& result.declaration.rangeWork)
			{
				partitions = BuildDeterministicWorkloadPartitions(result.declaration.estimatedWorkItemCount, result.declaration.partitionWorkItemCount, result.declaration.maximumPartitionCount);
			}
			if (partitions.empty())
			{
				partitions.push_back({ 0u, 1u, 0u, result.declaration.estimatedWorkItemCount });
			}
			result.partitionCount = partitions.size();
			try
			{
				for (const FrameWorkloadPartition& partition : partitions)
				{
					TERRAENGINE_PROFILE_TASK_SCOPE("FrameWorkload::CompatibilityJob", TerraEngine::Profiling::ProfilerModuleId::CpuEngine, result.declaration.workloadName.c_str());
					if (result.declaration.rangeWork)
					{
						result.declaration.rangeWork(partition, context);
					}
					else if (result.declaration.work)
					{
						result.declaration.work(context);
					}
					JobGraphResultEntry entry;
					entry.dispatchCount = 1u;
					entry.usefulWorkItemCount = partition.itemEnd - partition.itemBegin;
					if (options.collectExecutionRecords)
					{
						AppendExecutionRecord(plan, result, partition, &entry, true);
					}
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
		RefreshMigrationSummary(plan, options.formatDiagnostics);
		plan.formatted = options.formatDiagnostics
			? FormatFrameExecutionPlan(plan) : std::string{};
		if (options.collectExecutionRecords)
		{
			PublishExecutionRecordsForProfiling(plan);
		}
		return plan;
	}

	std::vector<TerraEngine::Profiling::WorkloadExecutionRecord> LatestFrameWorkloadExecutionRecordsForProfiling()
	{
		std::lock_guard<std::mutex> lock(g_ProfilingExecutionRecordsMutex);
		return g_ProfilingExecutionRecords;
	}
}
