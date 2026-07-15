// JobGraph.cpp - graph validation, worker dispatch, and deterministic summaries.
//
// The implementation intentionally stays at the general job-graph layer. It
// does not know about ECS chunks or component queries; future Part 3 work can
// attach those jobs through the same handles, barriers, and task groups.

#include "JobGraph.h"

#include "Profiling/ProfilerScope.h"
#include "ThreadPool/ThreadPool.h"
#include "WorkerSafety.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		constexpr std::uint32_t kFirstHandleId = 1;

		bool IsTerminal(JobGraphNodeState state)
		{
			return state == JobGraphNodeState::Completed
				|| state == JobGraphNodeState::Failed
				|| state == JobGraphNodeState::Cancelled
				|| state == JobGraphNodeState::Skipped
				|| state == JobGraphNodeState::Blocked;
		}

		bool IsSuccessful(JobGraphNodeState state)
		{
			return state == JobGraphNodeState::Completed;
		}

		bool IsError(const JobGraphDiagnostic& diagnostic)
		{
			return diagnostic.severity == JobGraphDiagnosticSeverity::Error;
		}

		int SeverityRank(JobGraphDiagnosticSeverity severity)
		{
			switch (severity)
			{
			case JobGraphDiagnosticSeverity::Error:
				return 0;
			case JobGraphDiagnosticSeverity::Warning:
				return 1;
			case JobGraphDiagnosticSeverity::Info:
			default:
				return 2;
			}
		}

		void SortDiagnostics(std::vector<JobGraphDiagnostic>& diagnostics)
		{
			std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const JobGraphDiagnostic& left, const JobGraphDiagnostic& right) {
				return std::make_tuple(SeverityRank(left.severity), static_cast<int>(left.category), left.graphId, left.nodeLabel, left.message, left.recommendedAction)
					< std::make_tuple(SeverityRank(right.severity), static_cast<int>(right.category), right.graphId, right.nodeLabel, right.message, right.recommendedAction);
			});
		}

		std::string ExceptionMessage()
		{
			try
			{
				throw;
			}
			catch (const std::exception& exception)
			{
				return exception.what();
			}
			catch (...)
			{
				return "unknown exception";
			}
		}

		std::string HandleKey(const DependencyHandle& handle)
		{
			std::ostringstream stream;
			stream << ToString(handle.kind) << "#" << handle.id << ":" << handle.generation << " '" << handle.label << "'";
			return stream.str();
		}

		std::string NormalizeLabel(const std::string& value, const std::string& fallback)
		{
			return value.empty() ? fallback : value;
		}

		JobGraphExecutionPolicy PolicyFromSystem(const SystemDeclaration& declaration)
		{
			// Current engine systems declare mainThread=true. A future explicit
			// worker-safe declaration can opt out of that and become worker eligible
			// without changing the graph contract.
			return declaration.mainThread ? JobGraphExecutionPolicy::Protected : JobGraphExecutionPolicy::WorkerEligible;
		}
	}

	struct JobGraph::Impl {
		struct Node {
			DependencyHandle handle;
			std::string name;
			JobGraphNodeKind kind = JobGraphNodeKind::Invalid;
			JobGraphExecutionPolicy executionPolicy = JobGraphExecutionPolicy::Protected;
			JobGraphNodeState state = JobGraphNodeState::Pending;
			std::string profilingLabel;
			std::vector<DependencyHandle> dependencies;
			std::vector<DependencyHandle> groupJobs;
			JobGraphBarrierPurpose barrierPurpose = JobGraphBarrierPurpose::Custom;
			std::vector<DependencyHandle> barrierBefore;
			std::vector<DependencyHandle> barrierAfter;
			bool diagnosePendingMutations = false;
			bool cancellable = true;
			bool requiresSuccessfulDependencies = true;
			bool mayMutateSceneStructure = false;
			std::size_t partitionIndex = 0;
			std::size_t partitionCount = 0;
			std::size_t itemBegin = 0;
			std::size_t itemEnd = 0;
			std::size_t usefulWorkItemCount = 0;
			std::uint64_t dispatchCount = 0;
			std::uint64_t queueDurationNs = 0;
			std::uint64_t runDurationNs = 0;
			std::uint64_t coordinatorWaitDurationNs = 0;
			std::chrono::steady_clock::time_point enqueuedAt;
			JobGraphWork work;
		};

		std::string graphId;
		std::string label;
		std::uint32_t generation = 1;
		std::uint32_t nextHandleId = kFirstHandleId;
		std::vector<Node> nodes;
		std::unordered_map<std::uint32_t, std::size_t> nodeById;
		std::vector<JobGraphDependencyEdge> explicitEdges;
		JobGraphCancellationRequest cancellation;
		std::atomic<bool> cancellationRequested = false;
		JobGraphSummary latestSummary;

		DependencyHandle MakeHandle(JobGraphNodeKind kind, const std::string& nodeLabel)
		{
			DependencyHandle handle;
			handle.id = nextHandleId++;
			handle.generation = generation;
			handle.kind = kind;
			handle.label = nodeLabel;
			return handle;
		}

		void AddDiagnostic(std::vector<JobGraphDiagnostic>& diagnostics, JobGraphDiagnosticSeverity severity, JobGraphDiagnosticCategory category, const std::string& nodeLabel, std::string message, std::string action) const
		{
			diagnostics.push_back({
				severity,
				category,
				graphId,
				nodeLabel,
				std::move(message),
				std::move(action)
			});
		}

		Node* FindNode(const DependencyHandle& handle)
		{
			const auto found = nodeById.find(handle.id);
			if (found == nodeById.end())
			{
				return nullptr;
			}
			Node& node = nodes[found->second];
			if (handle.generation != generation || node.handle.kind != handle.kind)
			{
				return nullptr;
			}
			return &node;
		}

		const Node* FindNode(const DependencyHandle& handle) const
		{
			const auto found = nodeById.find(handle.id);
			if (found == nodeById.end())
			{
				return nullptr;
			}
			const Node& node = nodes[found->second];
			if (handle.generation != generation || node.handle.kind != handle.kind)
			{
				return nullptr;
			}
			return &node;
		}

		bool ValidateHandle(const DependencyHandle& handle, JobGraphNodeKind expectedKind, std::vector<JobGraphDiagnostic>& diagnostics, const std::string& ownerLabel) const
		{
			if (!handle.IsValid())
			{
				AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Dependency, ownerLabel, "Dependency handle is empty.", "Use a handle returned by AddJob, AddTaskGroup, or AddBarrier.");
				return false;
			}
			const Node* node = FindNode(handle);
			if (node == nullptr)
			{
				AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Dependency, ownerLabel, "Dependency handle is stale or unknown: " + HandleKey(handle) + ".", "Rebuild the graph and use handles from the same graph generation.");
				return false;
			}
			if (expectedKind != JobGraphNodeKind::Invalid && node->kind != expectedKind)
			{
				AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Dependency, ownerLabel, "Dependency handle has the wrong kind: " + HandleKey(handle) + ".", "Pass a handle of the expected graph node type.");
				return false;
			}
			return true;
		}

		std::vector<JobGraphDependencyEdge> BuildEdges(std::vector<JobGraphDiagnostic>& diagnostics) const
		{
			std::vector<JobGraphDependencyEdge> edges = explicitEdges;
			auto addEdge = [&](DependencyHandle before, DependencyHandle after) {
				if (before.IsValid() && after.IsValid())
				{
					edges.push_back({ std::move(before), std::move(after) });
				}
			};

			for (const Node& node : nodes)
			{
				if (node.kind == JobGraphNodeKind::Job)
				{
					for (const DependencyHandle& dependency : node.dependencies)
					{
						addEdge(dependency, node.handle);
					}
				}
				for (const DependencyHandle& job : node.groupJobs)
				{
					addEdge(job, node.handle);
				}
				for (const DependencyHandle& before : node.barrierBefore)
				{
					addEdge(before, node.handle);
				}
				for (const DependencyHandle& after : node.barrierAfter)
				{
					addEdge(node.handle, after);
				}
			}

			std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> seen;
			std::vector<JobGraphDependencyEdge> uniqueEdges;
			for (const JobGraphDependencyEdge& edge : edges)
			{
				if (!ValidateHandle(edge.before, JobGraphNodeKind::Invalid, diagnostics, "edge")
					|| !ValidateHandle(edge.after, JobGraphNodeKind::Invalid, diagnostics, "edge"))
				{
					continue;
				}
				const auto key = std::make_tuple(edge.before.id, edge.before.generation, edge.after.id, edge.after.generation);
				if (edge.before.id == edge.after.id)
				{
					AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Dependency, edge.after.label, "A graph node cannot depend on itself.", "Remove the self-dependency edge.");
					continue;
				}
				if (seen.insert(key).second)
				{
					uniqueEdges.push_back(edge);
				}
			}
			std::stable_sort(uniqueEdges.begin(), uniqueEdges.end(), [](const JobGraphDependencyEdge& left, const JobGraphDependencyEdge& right) {
				return std::tie(left.before.id, left.after.id, left.before.label, left.after.label)
					< std::tie(right.before.id, right.after.id, right.before.label, right.after.label);
			});
			return uniqueEdges;
		}

		bool ValidateAcyclic(const std::vector<JobGraphDependencyEdge>& edges, std::vector<JobGraphDiagnostic>& diagnostics) const
		{
			std::unordered_map<std::uint32_t, std::size_t> indexById;
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				indexById[nodes[index].handle.id] = index;
			}

			std::vector<int> indegree(nodes.size(), 0);
			std::vector<std::vector<std::size_t>> outgoing(nodes.size());
			for (const JobGraphDependencyEdge& edge : edges)
			{
				const auto before = indexById.find(edge.before.id);
				const auto after = indexById.find(edge.after.id);
				if (before == indexById.end() || after == indexById.end())
				{
					continue;
				}
				outgoing[before->second].push_back(after->second);
				++indegree[after->second];
			}

			auto stableLess = [&](std::size_t left, std::size_t right) {
				return nodes[left].handle.id > nodes[right].handle.id;
			};
			std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(stableLess)> ready(stableLess);
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (indegree[index] == 0)
				{
					ready.push(index);
				}
			}

			std::size_t visited = 0;
			while (!ready.empty())
			{
				const std::size_t current = ready.top();
				ready.pop();
				++visited;
				for (std::size_t next : outgoing[current])
				{
					--indegree[next];
					if (indegree[next] == 0)
					{
						ready.push(next);
					}
				}
			}

			if (visited != nodes.size())
			{
				AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Dependency, "", "Dependency graph contains a cycle.", "Remove or reverse one dependency or barrier edge in the cycle.");
				return false;
			}
			return true;
		}

		JobGraphSummary BuildSummary(const std::vector<JobGraphDependencyEdge>& edges, std::vector<JobGraphDiagnostic> diagnostics) const
		{
			JobGraphSummary summary;
			summary.graphId = graphId;
			summary.label = label;
			summary.cancelled = cancellationRequested.load(std::memory_order_acquire);
			summary.dependencies = edges;
			for (const Node& node : nodes)
			{
				JobGraphResultEntry entry;
				entry.handle = node.handle;
				entry.name = node.name;
				entry.kind = node.kind;
				entry.state = node.state;
				entry.executionPolicy = node.executionPolicy;
				entry.profilingLabel = node.profilingLabel;
				entry.partitionIndex = node.partitionIndex;
				entry.partitionCount = node.partitionCount;
				entry.itemBegin = node.itemBegin;
				entry.itemEnd = node.itemEnd;
				entry.usefulWorkItemCount = node.usefulWorkItemCount;
				entry.dispatchCount = node.dispatchCount;
				entry.queueDurationNs = node.queueDurationNs;
				entry.runDurationNs = node.runDurationNs;
				entry.coordinatorWaitDurationNs = node.coordinatorWaitDurationNs;
				if (node.kind == JobGraphNodeKind::Job)
				{
					if (node.executionPolicy == JobGraphExecutionPolicy::WorkerEligible)
					{
						++summary.workerJobCount;
					}
					else if (node.executionPolicy == JobGraphExecutionPolicy::Protected || node.executionPolicy == JobGraphExecutionPolicy::MainThread || node.executionPolicy == JobGraphExecutionPolicy::MixedProtected)
					{
						++summary.protectedJobCount;
					}
					if (node.executionPolicy == JobGraphExecutionPolicy::MainThread)
					{
						++summary.inlineJobCount;
					}
					if (node.state == JobGraphNodeState::Failed || node.state == JobGraphNodeState::Blocked || node.state == JobGraphNodeState::Cancelled)
					{
						++summary.rejectedJobCount;
					}
					if (node.state == JobGraphNodeState::Blocked)
					{
						++summary.blockedDependencyCount;
					}
					summary.jobs.push_back(std::move(entry));
				}
				else if (node.kind == JobGraphNodeKind::TaskGroup)
				{
					summary.taskGroups.push_back(std::move(entry));
				}
				else if (node.kind == JobGraphNodeKind::Barrier)
				{
					summary.barriers.push_back(std::move(entry));
				}
			}

			auto byHandle = [](const JobGraphResultEntry& left, const JobGraphResultEntry& right) {
				return left.handle.id < right.handle.id;
			};
			std::stable_sort(summary.jobs.begin(), summary.jobs.end(), byHandle);
			std::stable_sort(summary.taskGroups.begin(), summary.taskGroups.end(), byHandle);
			std::stable_sort(summary.barriers.begin(), summary.barriers.end(), byHandle);

			SortDiagnostics(diagnostics);
			summary.diagnostics = std::move(diagnostics);
			summary.valid = std::none_of(summary.diagnostics.begin(), summary.diagnostics.end(), IsError);
			summary.completed = summary.valid && std::all_of(nodes.begin(), nodes.end(), [](const Node& node) {
				return IsTerminal(node.state);
			});
			summary.formatted = FormatJobGraphSummary(summary);
			return summary;
		}
	};

	bool DependencyHandle::IsValid() const
	{
		return id != 0 && generation != 0 && kind != JobGraphNodeKind::Invalid;
	}

	bool JobGraphContext::IsCancellationRequested() const
	{
		return cancellationRequested != nullptr && cancellationRequested->load(std::memory_order_acquire);
	}

	const std::string& JobGraphContext::CancellationReason() const
	{
		static const std::string empty;
		return cancellationReason != nullptr ? *cancellationReason : empty;
	}

	std::string ToString(JobGraphNodeKind kind)
	{
		switch (kind)
		{
		case JobGraphNodeKind::Job:
			return "job";
		case JobGraphNodeKind::TaskGroup:
			return "task-group";
		case JobGraphNodeKind::Barrier:
			return "barrier";
		case JobGraphNodeKind::Invalid:
		default:
			return "invalid";
		}
	}

	std::string ToString(JobGraphExecutionPolicy policy)
	{
		switch (policy)
		{
		case JobGraphExecutionPolicy::WorkerEligible:
			return "worker-eligible";
		case JobGraphExecutionPolicy::MainThread:
			return "main-thread";
		case JobGraphExecutionPolicy::MixedProtected:
			return "mixed-protected";
		case JobGraphExecutionPolicy::Protected:
		default:
			return "protected";
		}
	}

	std::string ToString(JobGraphNodeState state)
	{
		switch (state)
		{
		case JobGraphNodeState::Ready:
			return "ready";
		case JobGraphNodeState::Running:
			return "running";
		case JobGraphNodeState::Completed:
			return "completed";
		case JobGraphNodeState::Failed:
			return "failed";
		case JobGraphNodeState::Cancelled:
			return "cancelled";
		case JobGraphNodeState::Skipped:
			return "skipped";
		case JobGraphNodeState::Blocked:
			return "blocked";
		case JobGraphNodeState::Pending:
		default:
			return "pending";
		}
	}

	std::string ToString(JobGraphDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case JobGraphDiagnosticSeverity::Error:
			return "error";
		case JobGraphDiagnosticSeverity::Warning:
			return "warning";
		case JobGraphDiagnosticSeverity::Info:
		default:
			return "info";
		}
	}

	std::string ToString(JobGraphDiagnosticCategory category)
	{
		switch (category)
		{
		case JobGraphDiagnosticCategory::Dependency:
			return "dependency";
		case JobGraphDiagnosticCategory::Barrier:
			return "barrier";
		case JobGraphDiagnosticCategory::TaskGroup:
			return "task-group";
		case JobGraphDiagnosticCategory::Execution:
			return "execution";
		case JobGraphDiagnosticCategory::Cancellation:
			return "cancellation";
		case JobGraphDiagnosticCategory::Scheduler:
			return "scheduler";
		case JobGraphDiagnosticCategory::Mutation:
			return "mutation";
		case JobGraphDiagnosticCategory::Profiler:
			return "profiler";
		case JobGraphDiagnosticCategory::Summary:
		default:
			return "summary";
		}
	}

	std::string ToString(JobGraphBarrierPurpose purpose)
	{
		switch (purpose)
		{
		case JobGraphBarrierPurpose::Phase:
			return "phase";
		case JobGraphBarrierPurpose::Resource:
			return "resource";
		case JobGraphBarrierPurpose::Mutation:
			return "mutation";
		case JobGraphBarrierPurpose::FrameEnd:
			return "frame-end";
		case JobGraphBarrierPurpose::Custom:
		default:
			return "custom";
		}
	}

	std::string FormatJobGraphSummary(const JobGraphSummary& summary)
	{
		std::ostringstream stream;
		stream << "Job graph summary '" << summary.label << "' (" << summary.graphId << "): "
			<< (summary.valid ? "valid" : "invalid")
			<< ", " << (summary.completed ? "completed" : "incomplete")
			<< ", " << (summary.cancelled ? "cancelled" : "not-cancelled") << '\n';

		stream << "Jobs:";
		for (const JobGraphResultEntry& entry : summary.jobs)
		{
			stream << " [" << entry.name << "=" << ToString(entry.state) << ", " << ToString(entry.executionPolicy) << ", profile=" << entry.profilingLabel << "]";
		}
		stream << '\n';
		stream << "Coverage:"
			<< " worker=" << summary.workerJobCount
			<< " protected=" << summary.protectedJobCount
			<< " inline=" << summary.inlineJobCount
			<< " rejected=" << summary.rejectedJobCount
			<< " fallback=" << summary.fallbackJobCount
			<< " blocked=" << summary.blockedDependencyCount << '\n';

		stream << "Task groups:";
		for (const JobGraphResultEntry& entry : summary.taskGroups)
		{
			stream << " [" << entry.name << "=" << ToString(entry.state) << ", " << ToString(entry.executionPolicy) << ", profile=" << entry.profilingLabel << "]";
		}
		stream << '\n';

		stream << "Barriers:";
		for (const JobGraphResultEntry& entry : summary.barriers)
		{
			stream << " [" << entry.name << "=" << ToString(entry.state) << ", profile=" << entry.profilingLabel << "]";
		}
		stream << '\n';

		stream << "Dependencies:";
		for (const JobGraphDependencyEdge& edge : summary.dependencies)
		{
			stream << " [" << edge.before.label << " -> " << edge.after.label << "]";
		}
		stream << '\n';

		if (summary.diagnostics.empty())
		{
			stream << "Diagnostics: none";
		}
		else
		{
			stream << "Diagnostics:";
			for (const JobGraphDiagnostic& diagnostic : summary.diagnostics)
			{
				stream << "\n- " << ToString(diagnostic.severity) << "/" << ToString(diagnostic.category)
					<< " node='" << diagnostic.nodeLabel << "': " << diagnostic.message
					<< " Action: " << diagnostic.recommendedAction;
			}
		}
		return stream.str();
	}

	JobGraph::JobGraph(std::string graphId, std::string label)
		: impl(std::make_unique<Impl>())
	{
		Reset(std::move(graphId), std::move(label));
	}

	JobGraph::~JobGraph() = default;
	JobGraph::JobGraph(JobGraph&&) noexcept = default;
	JobGraph& JobGraph::operator=(JobGraph&&) noexcept = default;

	void JobGraph::Reset(std::string graphId, std::string label)
	{
		impl->graphId = std::move(graphId);
		impl->label = std::move(label);
		impl->generation = impl->generation == UINT32_MAX ? 1 : impl->generation + 1;
		impl->nextHandleId = kFirstHandleId;
		impl->nodes.clear();
		impl->nodeById.clear();
		impl->explicitEdges.clear();
		impl->cancellation = {};
		impl->cancellationRequested.store(false, std::memory_order_release);
		impl->latestSummary = {};
	}

	DependencyHandle JobGraph::AddJob(JobGraphJobDeclaration declaration)
	{
		Impl::Node node;
		node.kind = JobGraphNodeKind::Job;
		node.name = NormalizeLabel(declaration.name, "Job " + std::to_string(impl->nextHandleId));
		node.handle = impl->MakeHandle(node.kind, node.name);
		node.dependencies = std::move(declaration.dependencies);
		node.executionPolicy = declaration.executionPolicy;
		node.cancellable = declaration.cancellable;
		node.requiresSuccessfulDependencies = declaration.requiresSuccessfulDependencies;
		node.mayMutateSceneStructure = declaration.mayMutateSceneStructure;
		node.partitionIndex = declaration.partitionIndex;
		node.partitionCount = declaration.partitionCount;
		node.itemBegin = declaration.itemBegin;
		node.itemEnd = declaration.itemEnd;
		node.usefulWorkItemCount = declaration.usefulWorkItemCount;
		node.profilingLabel = NormalizeLabel(declaration.profilingLabel, node.name);
		node.work = std::move(declaration.work);
		if (declaration.group.IsValid())
		{
			node.dependencies.push_back({});
			node.dependencies.pop_back();
			impl->explicitEdges.push_back({ node.handle, declaration.group });
		}
		impl->nodeById[node.handle.id] = impl->nodes.size();
		impl->nodes.push_back(node);
		return node.handle;
	}

	std::vector<DependencyHandle> JobGraph::AddPartitionedJobs(std::vector<JobGraphJobDeclaration> declarations)
	{
		std::vector<DependencyHandle> handles;
		handles.reserve(declarations.size());
		for (JobGraphJobDeclaration& declaration : declarations)
		{
			handles.push_back(AddJob(std::move(declaration)));
		}
		return handles;
	}

	DependencyHandle JobGraph::AddTaskGroup(JobGraphTaskGroupDeclaration declaration)
	{
		Impl::Node node;
		node.kind = JobGraphNodeKind::TaskGroup;
		node.name = NormalizeLabel(declaration.name, "Task Group " + std::to_string(impl->nextHandleId));
		node.handle = impl->MakeHandle(node.kind, node.name);
		node.executionPolicy = declaration.executionPolicy;
		node.groupJobs = std::move(declaration.jobs);
		node.profilingLabel = NormalizeLabel(declaration.profilingLabel, node.name);
		impl->nodeById[node.handle.id] = impl->nodes.size();
		impl->nodes.push_back(node);
		return node.handle;
	}

	DependencyHandle JobGraph::AddBarrier(JobGraphBarrierDeclaration declaration)
	{
		Impl::Node node;
		node.kind = JobGraphNodeKind::Barrier;
		node.name = NormalizeLabel(declaration.name, "Barrier " + std::to_string(impl->nextHandleId));
		node.handle = impl->MakeHandle(node.kind, node.name);
		node.executionPolicy = JobGraphExecutionPolicy::Protected;
		node.barrierPurpose = declaration.purpose;
		node.barrierBefore = std::move(declaration.before);
		node.barrierAfter = std::move(declaration.after);
		node.diagnosePendingMutations = declaration.diagnosePendingMutations;
		node.profilingLabel = NormalizeLabel(declaration.profilingLabel, node.name);
		impl->nodeById[node.handle.id] = impl->nodes.size();
		impl->nodes.push_back(node);
		return node.handle;
	}

	void JobGraph::AddDependency(DependencyHandle before, DependencyHandle after)
	{
		impl->explicitEdges.push_back({ std::move(before), std::move(after) });
	}

	void JobGraph::RequestCancellation(std::string reason, JobGraphCancellationScope scope, DependencyHandle target)
	{
		impl->cancellation.requested = true;
		impl->cancellation.reason = reason.empty() ? "Cancellation requested." : std::move(reason);
		impl->cancellation.scope = scope;
		impl->cancellation.target = std::move(target);
		impl->cancellationRequested.store(true, std::memory_order_release);
	}

	bool JobGraph::IsCancellationRequested() const
	{
		return impl->cancellationRequested.load(std::memory_order_acquire);
	}

	const JobGraphSummary& JobGraph::Validate()
	{
		std::vector<JobGraphDiagnostic> diagnostics;
		if (impl->graphId.empty())
		{
			impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Summary, "", "Graph id is empty.", "Assign a stable graph id before validation.");
		}
		if (impl->label.empty())
		{
			impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Summary, "", "Graph label is empty.", "Assign a human-readable graph label before validation.");
		}

		std::unordered_set<std::string> names;
		std::unordered_set<std::uint32_t> handles;
		for (const Impl::Node& node : impl->nodes)
		{
			if (node.name.empty())
			{
				impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Summary, "", "Graph node name is empty.", "Give every job, group, and barrier a stable name.");
			}
			else if (!names.insert(ToString(node.kind) + ":" + node.name).second)
			{
				impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Summary, node.name, "Duplicate graph node name.", "Use unique names within each graph node kind.");
			}
			if (!handles.insert(node.handle.id).second)
			{
				impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Dependency, node.name, "Duplicate graph handle id.", "Rebuild the graph so each node receives a unique handle.");
			}
			if (node.kind == JobGraphNodeKind::Job)
			{
				for (const DependencyHandle& dependency : node.dependencies)
				{
					impl->ValidateHandle(dependency, JobGraphNodeKind::Invalid, diagnostics, node.name);
				}
				if (node.mayMutateSceneStructure && node.executionPolicy == JobGraphExecutionPolicy::WorkerEligible)
				{
					impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Mutation, node.name, "Worker-eligible job declares scene-structure mutation.", "Move scene mutation work behind a protected mutation barrier or defer it through SceneMutationQueue.");
				}
			}
			if (node.kind == JobGraphNodeKind::TaskGroup)
			{
				bool hasWorker = false;
				bool hasMainThread = false;
				for (const DependencyHandle& job : node.groupJobs)
				{
					if (!impl->ValidateHandle(job, JobGraphNodeKind::Job, diagnostics, node.name))
					{
						continue;
					}
					const Impl::Node* member = impl->FindNode(job);
					hasWorker = hasWorker || member->executionPolicy == JobGraphExecutionPolicy::WorkerEligible;
					hasMainThread = hasMainThread || member->executionPolicy == JobGraphExecutionPolicy::MainThread || member->executionPolicy == JobGraphExecutionPolicy::Protected;
				}
				if (hasWorker && hasMainThread && node.executionPolicy == JobGraphExecutionPolicy::WorkerEligible)
				{
					impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::TaskGroup, node.name, "Worker task group mixes worker and main-thread/protected jobs.", "Mark the group mixed-protected/protected or split the jobs into separate groups.");
				}
			}
			if (node.kind == JobGraphNodeKind::Barrier)
			{
				for (const DependencyHandle& before : node.barrierBefore)
				{
					impl->ValidateHandle(before, JobGraphNodeKind::Invalid, diagnostics, node.name);
				}
				for (const DependencyHandle& after : node.barrierAfter)
				{
					impl->ValidateHandle(after, JobGraphNodeKind::Invalid, diagnostics, node.name);
				}
			}
		}

		std::vector<JobGraphDependencyEdge> edges = impl->BuildEdges(diagnostics);
		impl->ValidateAcyclic(edges, diagnostics);
		for (Impl::Node& node : impl->nodes)
		{
			node.state = JobGraphNodeState::Pending;
		}
		impl->latestSummary = impl->BuildSummary(edges, std::move(diagnostics));
		return impl->latestSummary;
	}

	const JobGraphSummary& JobGraph::Execute(const JobGraphExecuteOptions& options)
	{
		const JobGraphSummary& validation = Validate();
		if (!validation.valid)
		{
			return impl->latestSummary;
		}

		std::vector<JobGraphDiagnostic> diagnostics = validation.diagnostics;
		std::vector<JobGraphDependencyEdge> edges = validation.dependencies;
		std::unordered_map<std::uint32_t, std::size_t> indexById;
		for (std::size_t index = 0; index < impl->nodes.size(); ++index)
		{
			indexById[impl->nodes[index].handle.id] = index;
			impl->nodes[index].state = JobGraphNodeState::Pending;
			impl->nodes[index].dispatchCount = 0u;
			impl->nodes[index].queueDurationNs = 0u;
			impl->nodes[index].runDurationNs = 0u;
			impl->nodes[index].coordinatorWaitDurationNs = 0u;
		}

		std::vector<int> remainingDependencies(impl->nodes.size(), 0);
		std::vector<std::vector<std::size_t>> outgoing(impl->nodes.size());
		std::vector<std::vector<std::size_t>> incoming(impl->nodes.size());
		for (const JobGraphDependencyEdge& edge : edges)
		{
			const std::size_t before = indexById[edge.before.id];
			const std::size_t after = indexById[edge.after.id];
			outgoing[before].push_back(after);
			incoming[after].push_back(before);
			++remainingDependencies[after];
		}

		JobGraphContext context;
		context.cancellationRequested = &impl->cancellationRequested;
		context.cancellationReason = &impl->cancellation.reason;
		std::atomic<std::uint64_t> workerTimeNs{ 0 };
		std::uint64_t protectedTimeNs = 0;
		std::uint64_t coordinatorWaitTimeNs = 0;

		auto completeNode = [&](std::size_t index, JobGraphNodeState state) {
			impl->nodes[index].state = state;
			for (std::size_t dependent : outgoing[index])
			{
				--remainingDependencies[dependent];
			}
		};

		auto dependencyFailed = [&](std::size_t index) {
			return std::any_of(incoming[index].begin(), incoming[index].end(), [&](std::size_t dependency) {
				return !IsSuccessful(impl->nodes[dependency].state);
			});
		};

		auto cancellationApplies = [&](const Impl::Node& node) {
			if (!impl->cancellationRequested.load(std::memory_order_acquire) || node.kind != JobGraphNodeKind::Job || !node.cancellable)
			{
				return false;
			}
			if (impl->cancellation.scope == JobGraphCancellationScope::Graph)
			{
				return true;
			}
			return impl->cancellation.target.IsValid() && impl->cancellation.target.id == node.handle.id;
		};

		std::size_t terminalCount = 0;
		while (terminalCount < impl->nodes.size())
		{
			std::vector<std::size_t> ready;
			for (std::size_t index = 0; index < impl->nodes.size(); ++index)
			{
				if (impl->nodes[index].state == JobGraphNodeState::Pending && remainingDependencies[index] == 0)
				{
					ready.push_back(index);
				}
			}
			std::stable_sort(ready.begin(), ready.end(), [&](std::size_t left, std::size_t right) {
				return impl->nodes[left].handle.id < impl->nodes[right].handle.id;
			});

			if (ready.empty())
			{
				for (std::size_t index = 0; index < impl->nodes.size(); ++index)
				{
					if (!IsTerminal(impl->nodes[index].state))
					{
						completeNode(index, JobGraphNodeState::Blocked);
						++terminalCount;
						impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Execution, impl->nodes[index].name, "Graph execution stalled before this node became ready.", "Inspect earlier dependency failures or validation gaps.");
					}
				}
				break;
			}

			struct RunningWorker {
				std::size_t index = 0;
				std::shared_ptr<Task<void>> task;
				std::future<void> future;
			};
			std::vector<RunningWorker> runningWorkers;

			for (std::size_t index : ready)
			{
				Impl::Node& node = impl->nodes[index];
				if (node.kind == JobGraphNodeKind::Job && node.requiresSuccessfulDependencies && dependencyFailed(index))
				{
					completeNode(index, JobGraphNodeState::Blocked);
					++terminalCount;
					impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Warning, JobGraphDiagnosticCategory::Execution, node.name, "Required dependency did not complete successfully.", "Keep this job blocked until the dependency failure is resolved.");
					continue;
				}
				if (cancellationApplies(node))
				{
					completeNode(index, JobGraphNodeState::Cancelled);
					++terminalCount;
					impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Info, JobGraphDiagnosticCategory::Cancellation, node.name, "Pending cancellable job was cancelled before execution.", "No action required unless this job should ignore graph cancellation.");
					continue;
				}
				if (node.kind == JobGraphNodeKind::TaskGroup)
				{
					completeNode(index, dependencyFailed(index) ? JobGraphNodeState::Blocked : JobGraphNodeState::Completed);
					++terminalCount;
					continue;
				}
				if (node.kind == JobGraphNodeKind::Barrier)
				{
					if (node.diagnosePendingMutations && options.mutationBarrierCallback)
					{
						const MutationFlushRuntimeState state = options.mutationBarrierCallback(node.name);
						if (state.pendingAfterFlush > 0)
						{
							impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Mutation, node.name, "Mutation barrier completed with pending mutations.", "Flush or reject pending SceneMutationQueue requests before crossing this barrier.");
						}
					}
					completeNode(index, dependencyFailed(index) ? JobGraphNodeState::Blocked : JobGraphNodeState::Completed);
					++terminalCount;
					continue;
				}

				node.state = JobGraphNodeState::Ready;
				if (node.executionPolicy == JobGraphExecutionPolicy::WorkerEligible)
				{
					node.state = JobGraphNodeState::Running;
					node.enqueuedAt = std::chrono::steady_clock::now();
					node.dispatchCount = 1u;
					auto task = ThreadPool::instance().createTask([this, index, context, &workerTimeNs]() mutable {
						const auto start = std::chrono::steady_clock::now();
						Impl::Node& workerNode = impl->nodes[index];
						workerNode.queueDurationNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - workerNode.enqueuedAt).count());
						WorkerSafetyScope workerSafetyScope(workerNode.name);
						TERRAENGINE_PROFILE_TASK_SCOPE("JobGraph::WorkerJob", TerraEngine::Profiling::ProfilerModuleId::TaskExecution, workerNode.profilingLabel.c_str());
						if (workerNode.work)
						{
							workerNode.work(context);
						}
						const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
						workerNode.runDurationNs = static_cast<std::uint64_t>(elapsed);
						workerTimeNs.fetch_add(workerNode.runDurationNs, std::memory_order_relaxed);
					});
					RunningWorker worker;
					worker.index = index;
					worker.future = task->getFuture();
					worker.task = std::move(task);
					ThreadPool::instance().enqueue(worker.task, node.usefulWorkItemCount);
					runningWorkers.push_back(std::move(worker));
				}
			}

			for (std::size_t index : ready)
			{
				Impl::Node& node = impl->nodes[index];
				if (node.kind != JobGraphNodeKind::Job || node.executionPolicy == JobGraphExecutionPolicy::WorkerEligible || IsTerminal(node.state))
				{
					continue;
				}
				try
				{
					node.state = JobGraphNodeState::Running;
					const auto start = std::chrono::steady_clock::now();
					node.dispatchCount = 1u;
					TERRAENGINE_PROFILE_TASK_SCOPE("JobGraph::ProtectedJob", TerraEngine::Profiling::ProfilerModuleId::CpuEngine, node.profilingLabel.c_str());
					if (node.work)
					{
						node.work(context);
					}
					const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
					node.runDurationNs = static_cast<std::uint64_t>(elapsed);
					protectedTimeNs += node.runDurationNs;
					completeNode(index, JobGraphNodeState::Completed);
				}
				catch (...)
				{
					completeNode(index, JobGraphNodeState::Failed);
					impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Execution, node.name, "Job threw during protected execution: " + ExceptionMessage() + ".", "Fix the job body or catch expected failures inside the job.");
				}
				++terminalCount;
			}

			for (RunningWorker& worker : runningWorkers)
			{
				Impl::Node& node = impl->nodes[worker.index];
				try
				{
					const auto start = std::chrono::steady_clock::now();
					worker.future.get();
					const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
					node.coordinatorWaitDurationNs = static_cast<std::uint64_t>(elapsed);
					coordinatorWaitTimeNs += node.coordinatorWaitDurationNs;
					completeNode(worker.index, JobGraphNodeState::Completed);
				}
				catch (...)
				{
					completeNode(worker.index, JobGraphNodeState::Failed);
					impl->AddDiagnostic(diagnostics, JobGraphDiagnosticSeverity::Error, JobGraphDiagnosticCategory::Execution, node.name, "Job threw during worker execution: " + ExceptionMessage() + ".", "Fix the job body or catch expected failures inside the job.");
				}
				++terminalCount;
			}
		}

		impl->latestSummary = impl->BuildSummary(edges, std::move(diagnostics));
		impl->latestSummary.workerTimeNs = workerTimeNs.load(std::memory_order_relaxed);
		impl->latestSummary.protectedTimeNs = protectedTimeNs;
		impl->latestSummary.coordinatorWaitTimeNs = coordinatorWaitTimeNs;
		const std::uint64_t totalMeasuredTimeNs = impl->latestSummary.workerTimeNs + impl->latestSummary.protectedTimeNs + impl->latestSummary.coordinatorWaitTimeNs;
		impl->latestSummary.workerUtilization = totalMeasuredTimeNs == 0
			? 0.0
			: static_cast<double>(impl->latestSummary.workerTimeNs) / static_cast<double>(totalMeasuredTimeNs);
		impl->latestSummary.formatted = FormatJobGraphSummary(impl->latestSummary);
		return impl->latestSummary;
	}

	const JobGraphSummary& JobGraph::LatestSummary() const
	{
		return impl->latestSummary;
	}

	JobGraph JobGraph::FromSchedulerLowering(const JobGraphSchedulerLoweringInput& input)
	{
		JobGraph graph(input.graphId, input.label);
		std::vector<SchedulerPhaseDefinition> orderedPhases = input.phases;
		std::stable_sort(orderedPhases.begin(), orderedPhases.end(), [](const SchedulerPhaseDefinition& left, const SchedulerPhaseDefinition& right) {
			return std::tie(left.order, left.name) < std::tie(right.order, right.name);
		});

		std::map<std::string, DependencyHandle> phaseGroups;
		for (const SchedulerPhaseDefinition& phase : orderedPhases)
		{
			JobGraphTaskGroupDeclaration group;
			group.name = "Phase: " + phase.name;
			group.executionPolicy = JobGraphExecutionPolicy::Protected;
			group.profilingLabel = group.name;
			phaseGroups[phase.name] = graph.AddTaskGroup(std::move(group));
		}

		DependencyHandle previousSystem;
		std::map<std::string, DependencyHandle> systemJobs;
		for (const SystemDeclaration& system : input.orderedSystems)
		{
			JobGraphJobDeclaration job;
			job.name = system.name;
			job.group = phaseGroups[system.phase];
			job.executionPolicy = PolicyFromSystem(system);
			job.cancellable = false;
			job.requiresSuccessfulDependencies = true;
			job.mayMutateSceneStructure = !system.writes.empty()
				&& std::find(system.writes.begin(), system.writes.end(), "SceneMutationQueue") != system.writes.end()
				&& job.executionPolicy == JobGraphExecutionPolicy::WorkerEligible;
			job.profilingLabel = NormalizeLabel(system.profilingLabel, system.name);
			DependencyHandle current = graph.AddJob(std::move(job));
			systemJobs[system.name] = current;
			if (previousSystem.IsValid())
			{
				graph.AddDependency(previousSystem, current);
			}
			previousSystem = current;
		}

		DependencyHandle previousPhase;
		for (const SchedulerPhaseDefinition& phase : orderedPhases)
		{
			DependencyHandle current = phaseGroups[phase.name];
			if (previousPhase.IsValid())
			{
				graph.AddDependency(previousPhase, current);
			}
			previousPhase = current;
		}

		for (const MutationFlushPoint& point : input.mutationFlushPoints)
		{
			JobGraphBarrierDeclaration barrier;
			barrier.name = "Mutation: " + point.name;
			barrier.purpose = point.position == MutationFlushPosition::FrameEnd ? JobGraphBarrierPurpose::FrameEnd : JobGraphBarrierPurpose::Mutation;
			barrier.profilingLabel = barrier.name;
			barrier.diagnosePendingMutations = point.diagnosePendingAtFrameEnd;
			if (!point.phaseName.empty() && phaseGroups.contains(point.phaseName))
			{
				barrier.before.push_back(phaseGroups[point.phaseName]);
			}
			else if (previousPhase.IsValid())
			{
				barrier.before.push_back(previousPhase);
			}
			graph.AddBarrier(std::move(barrier));
		}

		return graph;
	}
}
