// JobGraph.h - validated dependency graph execution over the engine ThreadPool.
//
// Part 2 of the scheduler work lives here: callers declare graph-local handles,
// dependencies, barriers, task groups, cancellation, and profiling labels. The
// graph validates those declarations before dispatching worker-eligible jobs to
// ThreadPool while keeping main-thread/protected work on the caller thread.

#pragma once

#include "Core/Export.h"
#include "SystemScheduler.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Scheduling {

	enum class JobGraphNodeKind {
		Invalid,
		Job,
		TaskGroup,
		Barrier
	};

	enum class JobGraphExecutionPolicy {
		WorkerEligible,
		MainThread,
		Protected,
		MixedProtected
	};

	enum class JobGraphNodeState {
		Pending,
		Ready,
		Running,
		Completed,
		Failed,
		Cancelled,
		Skipped,
		Blocked
	};

	enum class JobGraphDiagnosticSeverity {
		Info,
		Warning,
		Error
	};

	enum class JobGraphDiagnosticCategory {
		Dependency,
		Barrier,
		TaskGroup,
		Execution,
		Cancellation,
		Scheduler,
		Mutation,
		Profiler,
		Summary
	};

	enum class JobGraphBarrierPurpose {
		Phase,
		Resource,
		Mutation,
		FrameEnd,
		Custom
	};

	enum class JobGraphCancellationScope {
		Graph,
		TaskGroup,
		Job
	};

	struct DependencyHandle {
		std::uint32_t id = 0;
		std::uint32_t generation = 0;
		JobGraphNodeKind kind = JobGraphNodeKind::Invalid;
		std::string label;

		bool IsValid() const;
	};

	struct JobGraphContext {
		bool IsCancellationRequested() const;
		const std::string& CancellationReason() const;

	private:
		friend class JobGraph;
		const std::atomic<bool>* cancellationRequested = nullptr;
		const std::string* cancellationReason = nullptr;
	};

	using JobGraphWork = std::function<void(JobGraphContext&)>;
	using JobGraphMutationBarrierCallback = std::function<MutationFlushRuntimeState(const std::string& barrierName)>;

	struct JobGraphDiagnostic {
		JobGraphDiagnosticSeverity severity = JobGraphDiagnosticSeverity::Info;
		JobGraphDiagnosticCategory category = JobGraphDiagnosticCategory::Summary;
		std::string graphId;
		std::string nodeLabel;
		std::string message;
		std::string recommendedAction;
	};

	struct JobGraphResultEntry {
		DependencyHandle handle;
		std::string name;
		JobGraphNodeKind kind = JobGraphNodeKind::Invalid;
		JobGraphNodeState state = JobGraphNodeState::Pending;
		JobGraphExecutionPolicy executionPolicy = JobGraphExecutionPolicy::Protected;
		std::string profilingLabel;
		std::size_t partitionIndex = 0;
		std::size_t partitionCount = 0;
		std::size_t itemBegin = 0;
		std::size_t itemEnd = 0;
		std::size_t usefulWorkItemCount = 0;
		std::uint64_t dispatchCount = 0;
		std::uint64_t queueDurationNs = 0;
		std::uint64_t runDurationNs = 0;
		std::uint64_t coordinatorWaitDurationNs = 0;
	};

	struct JobGraphDependencyEdge {
		DependencyHandle before;
		DependencyHandle after;
	};

	struct JobGraphSummary {
		bool valid = true;
		bool completed = false;
		bool cancelled = false;
		std::string graphId;
		std::string label;
		std::vector<JobGraphResultEntry> jobs;
		std::vector<JobGraphResultEntry> taskGroups;
		std::vector<JobGraphResultEntry> barriers;
		std::vector<JobGraphDependencyEdge> dependencies;
		std::vector<JobGraphDiagnostic> diagnostics;
		std::size_t workerJobCount = 0;
		std::size_t protectedJobCount = 0;
		std::size_t inlineJobCount = 0;
		std::size_t rejectedJobCount = 0;
		std::size_t fallbackJobCount = 0;
		std::size_t blockedDependencyCount = 0;
		std::uint64_t coordinatorWaitTimeNs = 0;
		std::uint64_t workerTimeNs = 0;
		std::uint64_t protectedTimeNs = 0;
		double workerUtilization = 0.0;
		std::string formatted;
	};

	struct JobGraphJobDeclaration {
		std::string name;
		DependencyHandle group;
		std::vector<DependencyHandle> dependencies;
		JobGraphExecutionPolicy executionPolicy = JobGraphExecutionPolicy::Protected;
		bool cancellable = true;
		bool requiresSuccessfulDependencies = true;
		bool mayMutateSceneStructure = false;
		std::string profilingLabel;
		std::size_t partitionIndex = 0;
		std::size_t partitionCount = 0;
		std::size_t itemBegin = 0;
		std::size_t itemEnd = 0;
		std::size_t usefulWorkItemCount = 0;
		JobGraphWork work;
	};

	struct JobGraphTaskGroupDeclaration {
		std::string name;
		std::vector<DependencyHandle> jobs;
		JobGraphExecutionPolicy executionPolicy = JobGraphExecutionPolicy::Protected;
		std::string profilingLabel;
	};

	struct JobGraphBarrierDeclaration {
		std::string name;
		JobGraphBarrierPurpose purpose = JobGraphBarrierPurpose::Custom;
		std::vector<DependencyHandle> before;
		std::vector<DependencyHandle> after;
		std::string profilingLabel;
		bool diagnosePendingMutations = false;
	};

	struct JobGraphCancellationRequest {
		bool requested = false;
		std::string reason;
		JobGraphCancellationScope scope = JobGraphCancellationScope::Graph;
		DependencyHandle target;
		bool requestedBeforeExecution = true;
	};

	struct JobGraphExecuteOptions {
		JobGraphMutationBarrierCallback mutationBarrierCallback;
		// Frame scheduling consumes structured telemetry and does not display the
		// verbose graph string. Keeping formatting opt-in for that hot path avoids
		// building the same diagnostic text twice per frame.
		bool formatSummary = true;
	};

	struct JobGraphSchedulerLoweringInput {
		std::string graphId = "system-scheduler";
		std::string label = "System Scheduler Job Graph";
		std::vector<SchedulerPhaseDefinition> phases;
		std::vector<SystemDeclaration> orderedSystems;
		std::vector<MutationFlushPoint> mutationFlushPoints;
	};

	std::string ToString(JobGraphNodeKind kind);
	std::string ToString(JobGraphExecutionPolicy policy);
	std::string ToString(JobGraphNodeState state);
	std::string ToString(JobGraphDiagnosticSeverity severity);
	std::string ToString(JobGraphDiagnosticCategory category);
	std::string ToString(JobGraphBarrierPurpose purpose);
	std::string FormatJobGraphSummary(const JobGraphSummary& summary);

	class TERRAENGINE_API JobGraph {
	public:
		JobGraph(std::string graphId = "job-graph", std::string label = "Job Graph");
		~JobGraph();
		JobGraph(JobGraph&&) noexcept;
		JobGraph& operator=(JobGraph&&) noexcept;
		JobGraph(const JobGraph&) = delete;
		JobGraph& operator=(const JobGraph&) = delete;

		void Reset(std::string graphId, std::string label);
		// Rearm keeps the validated node/edge topology but clears all frame-local
		// execution state. It is valid only after the prior Execute call returned;
		// callers update each job callback before executing the next frame.
		void Rearm(std::string graphId);
		DependencyHandle AddJob(JobGraphJobDeclaration declaration);
		// Stable scheduler descriptors change their captured frame inputs without
		// changing graph topology. Updating only the callback lets Execute reuse the
		// previously validated dependency graph and avoids rebuilding it every frame.
		bool UpdateJobWork(
			const DependencyHandle& handle,
			JobGraphWork work,
			std::string profilingLabel = {});
		// AddPartitionedJobs creates one independently schedulable node per stable
		// range. Callers supply ranges in deterministic publication order; worker
		// completion order never changes the returned handle order.
		std::vector<DependencyHandle> AddPartitionedJobs(std::vector<JobGraphJobDeclaration> declarations);
		DependencyHandle AddTaskGroup(JobGraphTaskGroupDeclaration declaration);
		DependencyHandle AddBarrier(JobGraphBarrierDeclaration declaration);
		void AddDependency(DependencyHandle before, DependencyHandle after);
		void RequestCancellation(std::string reason, JobGraphCancellationScope scope = JobGraphCancellationScope::Graph, DependencyHandle target = {});
		bool IsCancellationRequested() const;

		const JobGraphSummary& Validate(bool formatSummary = true);
		const JobGraphSummary& Execute(const JobGraphExecuteOptions& options = {});
		const JobGraphSummary& LatestSummary() const;

		static JobGraph FromSchedulerLowering(const JobGraphSchedulerLoweringInput& input);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

}
