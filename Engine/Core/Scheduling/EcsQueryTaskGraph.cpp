// EcsQueryTaskGraph.cpp - deterministic ECS query planning and JobGraph lowering.

#include "EcsQueryTaskGraph.h"

#include "Core/Object/ObjectManager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <tuple>
#include <utility>

namespace TerraEngine::Scheduling {
	namespace {
		void SortIds(std::vector<TerraEngine::UUID>& ids)
		{
			std::stable_sort(ids.begin(), ids.end(), [](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
				return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
			});
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		}

		EcsRaceDiagnostic FromAccessDiagnostic(const ComponentAccessDiagnostic& diagnostic)
		{
			return {
				diagnostic.severity,
				ToString(diagnostic.code),
				diagnostic.ownerSystem,
				diagnostic.queryName,
				diagnostic.message + " Action: " + diagnostic.recommendedAction,
				diagnostic.resourceName
			};
		}

		bool IsBlocking(const EcsRaceDiagnostic& diagnostic)
		{
			return diagnostic.severity == ComponentAccessDiagnosticSeverity::Error;
		}

		bool HasWriteConflict(const EcsComponentQueryDeclaration& left, const EcsComponentQueryDeclaration& right)
		{
			const ComponentAccessConflict conflict = ClassifyComponentAccessConflict(left.access, right.access);
			return conflict.requiresOrdering && conflict.kind != ComponentAccessConflictKind::MissingDeclaration;
		}

		EcsQueryExecutionPolicy SelectPolicy(const EcsComponentQueryDeclaration& declaration, bool protectedFallback)
		{
			if (protectedFallback || !declaration.access.workerEligible)
			{
				return EcsQueryExecutionPolicy::Protected;
			}
			if (declaration.matchingObjects.size() < declaration.workerThreshold)
			{
				return EcsQueryExecutionPolicy::Inline;
			}
			return EcsQueryExecutionPolicy::WorkerTaskGroup;
		}

		std::vector<TerraEngine::CompactObjectReference> BuildCompactReferences(const std::vector<TerraEngine::UUID>& objects)
		{
			ObjectManager& objectManager = ObjectManager::instance();
			std::vector<TerraEngine::CompactObjectReference> references;
			references.reserve(objects.size());
			for (const TerraEngine::UUID& objectId : objects)
			{
				if (auto object = objectManager.getObject(objectId))
				{
					references.push_back(objectManager.makeCompactReference(object->EntityId()));
					continue;
				}
				TerraEngine::CompactObjectReference unresolved;
				unresolved.stableId = objectId;
				references.push_back(unresolved);
			}
			return references;
		}

		ComponentAccessGrantResult BuildAccessGrant(const EcsComponentQueryDeclaration& declaration, EcsQueryExecutionPolicy policy)
		{
			ComponentAccessGrantRequest request;
			request.grantId = declaration.queryName + ".grant";
			request.workloadName = declaration.queryName;
			request.readComponents = declaration.access.readComponents;
			request.writeComponents = declaration.access.writeComponents;
			request.scope = ToString(policy);
			request.grantMode = policy == EcsQueryExecutionPolicy::Protected
				? ComponentAccessGrantMode::ProtectedCommit
				: declaration.access.writeComponents.empty()
				? ComponentAccessGrantMode::WorkerRead
				: ComponentAccessGrantMode::WorkerWriteDeferred;
			request.schedulerValidated = declaration.access.workerEligible && policy != EcsQueryExecutionPolicy::Protected;
			return ValidateComponentAccessGrant(std::move(request));
		}

		std::vector<EcsQueryChunk> BuildChunks(const EcsComponentQueryDeclaration& declaration, EcsQueryExecutionPolicy policy, const std::vector<TerraEngine::CompactObjectReference>& compactObjects, const ComponentAccessGrantResult& accessGrant)
		{
			const std::vector<TerraEngine::UUID>& objects = declaration.matchingObjects;
			if (objects.empty())
			{
				if (declaration.runOnceWithoutObjectSnapshot)
				{
					EcsQueryChunk chunk;
					chunk.chunkIndex = 0;
					chunk.accessGrant = accessGrant;
					chunk.profilingLabel = declaration.queryName + ".direct";
					return { std::move(chunk) };
				}
				return {};
			}

			const std::size_t chunkSize = policy == EcsQueryExecutionPolicy::WorkerTaskGroup
				? std::max<std::size_t>(1, declaration.preferredChunkSize)
				: objects.size();
			std::vector<EcsQueryChunk> chunks;
			for (std::size_t begin = 0; begin < objects.size(); begin += chunkSize)
			{
				const std::size_t end = std::min(begin + chunkSize, objects.size());
				EcsQueryChunk chunk;
				chunk.chunkIndex = chunks.size();
				chunk.begin = begin;
				chunk.end = end;
				if (declaration.copyObjectIdsIntoChunks)
				{
					chunk.objects.assign(objects.begin() + static_cast<std::ptrdiff_t>(begin), objects.begin() + static_cast<std::ptrdiff_t>(end));
					chunk.compactObjects.assign(compactObjects.begin() + static_cast<std::ptrdiff_t>(begin), compactObjects.begin() + static_cast<std::ptrdiff_t>(end));
				}
				chunk.accessGrant = accessGrant;
				chunk.profilingLabel = declaration.queryName + ".chunk" + std::to_string(chunk.chunkIndex);
				chunks.push_back(std::move(chunk));
			}
			return chunks;
		}

		std::string PolicyToJobPolicy(EcsQueryExecutionPolicy policy)
		{
			return policy == EcsQueryExecutionPolicy::WorkerTaskGroup ? "worker-eligible" : "protected";
		}

		std::mutex& PublishedSummaryMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		std::vector<EcsSystemGraphFrameSummary>& PublishedSummaries()
		{
			static std::vector<EcsSystemGraphFrameSummary> summaries;
			return summaries;
		}

		std::atomic_bool& EditorDiagnosticsRequested()
		{
			static std::atomic_bool requested = false;
			return requested;
		}
	}

	EcsQueryTaskGraph::EcsQueryTaskGraph(EcsQueryTaskGraphOptions graphOptions)
		: options(std::move(graphOptions))
	{
	}

	std::string ToString(EcsQueryExecutionPolicy policy)
	{
		switch (policy)
		{
		case EcsQueryExecutionPolicy::WorkerTaskGroup:
			return "worker-task-group";
		case EcsQueryExecutionPolicy::Inline:
			return "inline";
		case EcsQueryExecutionPolicy::Protected:
		default:
			return "protected";
		}
	}

	std::string ToString(EcsQueryResultState state)
	{
		switch (state)
		{
		case EcsQueryResultState::Running:
			return "running";
		case EcsQueryResultState::Completed:
			return "completed";
		case EcsQueryResultState::Failed:
			return "failed";
		case EcsQueryResultState::Cancelled:
			return "cancelled";
		case EcsQueryResultState::Blocked:
			return "blocked";
		case EcsQueryResultState::Pending:
		default:
			return "pending";
		}
	}

	std::string FormatEcsSystemGraphFrameSummary(const EcsSystemGraphFrameSummary& summary)
	{
		std::ostringstream stream;
		stream << "ECS query task graph summary: " << (summary.valid ? "valid" : "invalid")
			<< ", " << (summary.completed ? "completed" : "incomplete") << '\n';
		stream << "Queries:";
		for (const EcsComponentQueryPlan& plan : summary.queryPlans)
		{
			stream << " [" << plan.declaration.queryName
				<< " policy=" << ToString(plan.executionPolicy)
				<< " objects=" << plan.matchingObjects.size()
				<< " compactRefs=" << plan.compactMatchingObjects.size()
				<< " chunks=" << plan.chunks.size()
				<< " phase=" << ToString(plan.phaseKind)
				<< " label=" << plan.phaseLabel
				<< " processed=" << plan.processedObjectCount
				<< " grant=" << (plan.accessGrant.granted ? "granted" : "protected")
				<< " invalidated=" << (plan.invalidatedByMutation ? "true" : "false")
				<< "]";
		}
		stream << '\n';
		stream << "Transform work:";
		for (const EcsTransformWorkUnit& unit : summary.transformWorkUnits)
		{
			stream << " [" << unit.workUnitId << " level=" << unit.hierarchyLevel << " objects=" << unit.objects.size() << "]";
		}
		stream << " totalObjects=" << summary.transformWorkObjectCount << " spatialBatches=" << summary.spatialRefreshBatchCount;
		stream << '\n';
		stream << "Mutation flushes:";
		for (const std::string& flush : summary.mutationFlushes)
		{
			stream << " [" << flush << "]";
		}
		stream << '\n';
		if (summary.diagnostics.empty())
		{
			stream << "Diagnostics: none";
		}
		else
		{
			stream << "Diagnostics:";
			for (const EcsRaceDiagnostic& diagnostic : summary.diagnostics)
			{
				stream << "\n- " << ToString(diagnostic.severity)
					<< "/" << diagnostic.code
					<< " system='" << diagnostic.systemName
					<< "' query='" << diagnostic.queryName
					<< "' blocked='" << diagnostic.blockedWork
					<< "': " << diagnostic.details;
			}
		}
		return stream.str();
	}

	bool EcsGraphFrameDiagnosticsEnabled()
	{
		static const bool enabledByEnvironment = [] {
			const char* value = std::getenv("TERRAENGINE_ECS_GRAPH_DIAGNOSTICS");
			if (value == nullptr)
			{
				return false;
			}

			// Read once at process startup use so normal frames do not pay for an
			// environment lookup, string allocation, sorting, or summary rebuild.
			std::string normalized(value);
			std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
		}();
		return enabledByEnvironment || EditorDiagnosticsRequested().load(std::memory_order_relaxed);
	}

	void SetEcsGraphFrameDiagnosticsRequested(bool requested)
	{
		// Editor UI is the only expected caller. Atomic storage keeps the flag
		// cheap and safe if a future editor tick requests diagnostics from a
		// different thread than the engine frame loop.
		EditorDiagnosticsRequested().store(requested, std::memory_order_relaxed);
	}

	void ClearPublishedEcsGraphFrameSummaries()
	{
		std::scoped_lock lock(PublishedSummaryMutex());
		PublishedSummaries().clear();
	}

	void PublishEcsGraphFrameSummary(const EcsSystemGraphFrameSummary& summary)
	{
		EcsSystemGraphFrameSummary compact;
		compact.valid = summary.valid;
		compact.completed = summary.completed;
		compact.diagnostics = summary.diagnostics;
		compact.mutationFlushes = summary.mutationFlushes;
		compact.transformWorkUnits = summary.transformWorkUnits;
		compact.transformWorkObjectCount = summary.transformWorkObjectCount;
		compact.spatialRefreshBatchCount = summary.spatialRefreshBatchCount;
		compact.jobGraphSummary.graphId = summary.jobGraphSummary.graphId;
		compact.jobGraphSummary.label = summary.jobGraphSummary.label;
		compact.jobGraphSummary.valid = summary.jobGraphSummary.valid;
		compact.jobGraphSummary.completed = summary.jobGraphSummary.completed;
		compact.jobGraphSummary.cancelled = summary.jobGraphSummary.cancelled;
		compact.queryPlans.reserve(summary.queryPlans.size());
		for (const EcsComponentQueryPlan& plan : summary.queryPlans)
		{
			EcsComponentQueryPlan compactPlan;
			compactPlan.declaration.queryName = plan.declaration.queryName;
			compactPlan.declaration.access = plan.declaration.access;
			compactPlan.declaration.copyObjectIdsIntoChunks = plan.declaration.copyObjectIdsIntoChunks;
			compactPlan.declaration.preserveMatchingObjectOrder = plan.declaration.preserveMatchingObjectOrder;
			compactPlan.declaration.runOnceWithoutObjectSnapshot = plan.declaration.runOnceWithoutObjectSnapshot;
			compactPlan.executionPolicy = plan.executionPolicy;
			compactPlan.processedObjectCount = plan.processedObjectCount;
			compactPlan.invalidatedByMutation = plan.invalidatedByMutation;
			compactPlan.accessGrant = plan.accessGrant;
			compact.queryPlans.push_back(std::move(compactPlan));
		}
		std::scoped_lock lock(PublishedSummaryMutex());
		std::vector<EcsSystemGraphFrameSummary>& summaries = PublishedSummaries();
		const std::string compactGraphId = compact.jobGraphSummary.graphId.empty() ? "ecs-query-task-graph" : compact.jobGraphSummary.graphId;
		auto existing = std::find_if(summaries.begin(), summaries.end(), [&](const EcsSystemGraphFrameSummary& published) {
			const std::string publishedGraphId = published.jobGraphSummary.graphId.empty() ? "ecs-query-task-graph" : published.jobGraphSummary.graphId;
			return publishedGraphId == compactGraphId;
		});
		if (existing != summaries.end())
		{
			*existing = std::move(compact);
		}
		else
		{
			summaries.push_back(std::move(compact));
		}
	}

	std::vector<EcsSystemGraphFrameSummary> PublishedEcsGraphFrameSummaries()
	{
		std::scoped_lock lock(PublishedSummaryMutex());
		return PublishedSummaries();
	}

	std::vector<std::string> PublishedEcsComponentQueryGroups()
	{
		std::scoped_lock lock(PublishedSummaryMutex());
		std::vector<std::string> groups;
		for (const EcsSystemGraphFrameSummary& summary : PublishedSummaries())
		{
			for (const EcsComponentQueryPlan& plan : summary.queryPlans)
			{
				groups.push_back(plan.declaration.queryName + ".group");
			}
		}
		std::stable_sort(groups.begin(), groups.end());
		groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
		return groups;
	}

	std::string FormatPublishedEcsGraphFrameSummaries()
	{
		std::scoped_lock lock(PublishedSummaryMutex());
		const std::vector<EcsSystemGraphFrameSummary>& summaries = PublishedSummaries();
		if (summaries.empty())
		{
			return "No ECS query graphs published this frame.";
		}

		std::ostringstream stream;
		stream << "Published ECS graphs:";
		for (const EcsSystemGraphFrameSummary& summary : summaries)
		{
			const std::string graphId = summary.jobGraphSummary.graphId.empty() ? "ecs-query-task-graph" : summary.jobGraphSummary.graphId;
			stream << " [" << graphId
				<< " valid=" << (summary.valid ? "true" : "false")
				<< " completed=" << (summary.completed ? "true" : "false")
				<< " queries=" << summary.queryPlans.size()
				<< " diagnostics=" << summary.diagnostics.size()
				<< "]";
		}
		return stream.str();
	}

	void EcsQueryTaskGraph::Reset(EcsQueryTaskGraphOptions graphOptions)
	{
		options = std::move(graphOptions);
		queries.clear();
		transformUnits.clear();
		latestSummary = {};
	}

	void EcsQueryTaskGraph::AddQuery(EcsComponentQueryDeclaration declaration)
	{
		if (declaration.queryName.empty())
		{
			declaration.queryName = declaration.access.queryName;
		}
		if (declaration.access.queryName.empty())
		{
			declaration.access.queryName = declaration.queryName;
		}
		if (declaration.phaseLabel.empty())
		{
			declaration.phaseLabel = declaration.queryName + "." + ToString(declaration.phaseKind);
		}
		if (!declaration.preserveMatchingObjectOrder)
		{
			SortIds(declaration.matchingObjects);
		}
		queries.push_back(std::move(declaration));
	}

	void EcsQueryTaskGraph::AddTransformWorkUnit(EcsTransformWorkUnit workUnit)
	{
		if (workUnit.profilingLabel.empty())
		{
			workUnit.profilingLabel = workUnit.workUnitId;
		}
		transformUnits.push_back(std::move(workUnit));
	}

	void EcsQueryTaskGraph::EnqueueMutation(TerraEngine::Scene::SceneMutationRequest request)
	{
		TerraEngine::Scene::SceneMutationQueue::Instance().Enqueue(std::move(request));
	}

	MutationFlushRuntimeState EcsQueryTaskGraph::FlushMutations(const std::string& flushPointName)
	{
		MutationFlushRuntimeState state;
		TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
		state.flushAttempted = true;
		if (!queue.Empty())
		{
			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(ObjectManager::instance());
			state.message = result.applied ? "ECS mutation flush applied at " + flushPointName + "." : "ECS mutation flush rejected at " + flushPointName + ".";
			for (EcsComponentQueryPlan& plan : latestSummary.queryPlans)
			{
				plan.invalidatedByMutation = true;
			}
		}
		state.pendingAfterFlush = queue.Size();
		latestSummary.mutationFlushes.push_back(flushPointName + " pending=" + std::to_string(state.pendingAfterFlush));
		latestSummary.formatted = options.buildFormattedSummary ? FormatEcsSystemGraphFrameSummary(latestSummary) : std::string{};
		return state;
	}

	const EcsSystemGraphFrameSummary& EcsQueryTaskGraph::Validate()
	{
		latestSummary = {};
		latestSummary.transformWorkUnits = transformUnits;
		for (const EcsTransformWorkUnit& unit : transformUnits)
		{
			latestSummary.transformWorkObjectCount += unit.objects.size();
		}
		std::vector<ComponentAccessDeclaration> accessDeclarations;
		std::map<std::string, bool> workerEligibilityByQuery;

		for (EcsComponentQueryDeclaration query : queries)
		{
			bool protectedFallback = false;
			if (options.validateAccessDeclarations)
			{
				ComponentAccessValidationResult access = ValidateComponentAccessDeclaration(query.access);
				query.access = access.normalized;
				protectedFallback = access.protectedFallback;
				for (const ComponentAccessDiagnostic& diagnostic : access.diagnostics)
				{
					latestSummary.diagnostics.push_back(FromAccessDiagnostic(diagnostic));
				}
			}

			EcsComponentQueryPlan plan;
			plan.declaration = query;
			plan.matchingObjects = query.matchingObjects;
			plan.executionPolicy = SelectPolicy(query, protectedFallback);
			plan.phaseKind = plan.executionPolicy == EcsQueryExecutionPolicy::Protected
				? WorkerSystemPhaseKind::ProtectedCommit
				: query.phaseKind;
			plan.phaseLabel = query.phaseLabel;
			plan.compactMatchingObjects = BuildCompactReferences(plan.matchingObjects);
			plan.accessGrant = BuildAccessGrant(query, plan.executionPolicy);
			for (const ComponentAccessDiagnostic& diagnostic : plan.accessGrant.diagnostics)
			{
				if (plan.executionPolicy == EcsQueryExecutionPolicy::WorkerTaskGroup || plan.executionPolicy == EcsQueryExecutionPolicy::Inline)
				{
					latestSummary.diagnostics.push_back(FromAccessDiagnostic(diagnostic));
				}
			}
			plan.chunks = BuildChunks(query, plan.executionPolicy, plan.compactMatchingObjects, plan.accessGrant);
			latestSummary.queryPlans.push_back(std::move(plan));
			if (options.validateAccessDeclarations)
			{
				accessDeclarations.push_back(query.access);
				workerEligibilityByQuery[query.access.queryName] = query.access.workerEligible;
			}
		}

		if (options.validateAccessDeclarations)
		{
			const std::vector<ComponentAccessDiagnostic> setDiagnostics = ValidateComponentAccessSet(accessDeclarations);
			for (const ComponentAccessDiagnostic& diagnostic : setDiagnostics)
			{
				if (diagnostic.code == ComponentAccessDiagnosticCode::AccessConflict)
				{
					const std::string delimiter = " <-> ";
					const std::size_t split = diagnostic.queryName.find(delimiter);
					if (split != std::string::npos)
					{
						const std::string leftQuery = diagnostic.queryName.substr(0, split);
						const std::string rightQuery = diagnostic.queryName.substr(split + delimiter.size());
						const auto left = workerEligibilityByQuery.find(leftQuery);
						const auto right = workerEligibilityByQuery.find(rightQuery);
						if (left != workerEligibilityByQuery.end() && right != workerEligibilityByQuery.end()
							&& !left->second && !right->second)
						{
							// Protected query pairs are intentionally serialized
							// on the main thread. Suppress those expected
							// conflicts so editor diagnostics stay focused on
							// worker-eligible race risks and missing ordering
							// metadata.
							continue;
						}
					}
				}
				latestSummary.diagnostics.push_back(FromAccessDiagnostic(diagnostic));
			}
		}

		std::stable_sort(latestSummary.transformWorkUnits.begin(), latestSummary.transformWorkUnits.end(), [](const EcsTransformWorkUnit& left, const EcsTransformWorkUnit& right) {
			return std::tie(left.hierarchyLevel, left.workUnitId) < std::tie(right.hierarchyLevel, right.workUnitId);
		});
		latestSummary.valid = std::none_of(latestSummary.diagnostics.begin(), latestSummary.diagnostics.end(), IsBlocking);
		latestSummary.completed = false;
		latestSummary.formatted = options.buildFormattedSummary ? FormatEcsSystemGraphFrameSummary(latestSummary) : std::string{};
		return latestSummary;
	}

	const EcsSystemGraphFrameSummary& EcsQueryTaskGraph::Execute()
	{
		if (!options.validateAccessDeclarations && !options.buildFormattedSummary && transformUnits.empty())
		{
			const bool canRunDirectCallbacksOnly = std::all_of(queries.begin(), queries.end(), [](const EcsComponentQueryDeclaration& query) {
				return query.runOnceWithoutObjectSnapshot && query.matchingObjects.empty() && !query.access.workerEligible;
			});
			if (canRunDirectCallbacksOnly)
			{
				// Hot protected scans such as renderer/UI collection already
				// know they must stay on the main thread. When editor
				// diagnostics are closed, execute their real work directly and
				// avoid rebuilding validation metadata, query plans, chunks, or
				// summary strings every frame.
				latestSummary = {};
				JobGraphContext context;
				EcsQueryChunk directChunk;
				directChunk.chunkIndex = 0;
				for (const EcsComponentQueryDeclaration& query : queries)
				{
					if (query.work)
					{
						directChunk.profilingLabel = query.queryName + ".direct";
						query.work(directChunk, context);
					}
				}
				latestSummary.valid = true;
				latestSummary.completed = true;
				latestSummary.jobGraphSummary.graphId = options.graphId;
				latestSummary.jobGraphSummary.label = options.label;
				latestSummary.jobGraphSummary.valid = true;
				latestSummary.jobGraphSummary.completed = true;
				return latestSummary;
			}
		}

		Validate();
		if (!latestSummary.valid)
		{
			if (!options.buildFormattedSummary)
			{
				latestSummary.formatted = FormatEcsSystemGraphFrameSummary(latestSummary);
			}
			return latestSummary;
		}

		const bool canRunDirectly = transformUnits.empty()
			&& std::none_of(latestSummary.queryPlans.begin(), latestSummary.queryPlans.end(), [](const EcsComponentQueryPlan& plan) {
				return plan.executionPolicy == EcsQueryExecutionPolicy::WorkerTaskGroup;
			});
		if (canRunDirectly)
		{
			JobGraphContext context;
			for (EcsComponentQueryPlan& plan : latestSummary.queryPlans)
			{
				for (EcsQueryChunk& chunk : plan.chunks)
				{
					if (plan.declaration.work)
					{
						plan.declaration.work(chunk, context);
					}
					plan.processedObjectCount += chunk.end - chunk.begin;
					chunk.state = EcsQueryResultState::Completed;
				}
			}
			latestSummary.jobGraphSummary.graphId = options.graphId;
			latestSummary.jobGraphSummary.label = options.label;
			latestSummary.jobGraphSummary.valid = true;
			latestSummary.jobGraphSummary.completed = true;
			latestSummary.jobGraphSummary.formatted = options.buildFormattedSummary ? FormatJobGraphSummary(latestSummary.jobGraphSummary) : std::string{};
			latestSummary.completed = true;
			latestSummary.formatted = options.buildFormattedSummary ? FormatEcsSystemGraphFrameSummary(latestSummary) : std::string{};
			return latestSummary;
		}

		JobGraph graph(options.graphId, options.label);
		std::vector<DependencyHandle> queryGroups;
		std::vector<std::shared_ptr<std::atomic<std::size_t>>> processed;
		for (std::size_t index = 0; index < latestSummary.queryPlans.size(); ++index)
		{
			processed.push_back(std::make_shared<std::atomic<std::size_t>>(0));
		}

		for (std::size_t planIndex = 0; planIndex < latestSummary.queryPlans.size(); ++planIndex)
		{
			EcsComponentQueryPlan& plan = latestSummary.queryPlans[planIndex];
			std::vector<DependencyHandle> chunkJobs;
			for (const EcsQueryChunk& chunk : plan.chunks)
			{
				JobGraphJobDeclaration job;
				job.name = plan.declaration.queryName + ".chunk" + std::to_string(chunk.chunkIndex);
				job.executionPolicy = plan.executionPolicy == EcsQueryExecutionPolicy::WorkerTaskGroup
					? JobGraphExecutionPolicy::WorkerEligible
					: JobGraphExecutionPolicy::Protected;
				job.cancellable = true;
				job.requiresSuccessfulDependencies = true;
				job.mayMutateSceneStructure = false;
				job.profilingLabel = chunk.profilingLabel.empty() ? job.name : chunk.profilingLabel;
				std::shared_ptr<std::atomic<std::size_t>> processedCounter = processed[planIndex];
				job.work = [chunk, planIndex, processedCounter, this](JobGraphContext& context) {
					if (planIndex < latestSummary.queryPlans.size() && latestSummary.queryPlans[planIndex].declaration.work)
					{
						latestSummary.queryPlans[planIndex].declaration.work(chunk, context);
					}
					processedCounter->fetch_add(chunk.end - chunk.begin, std::memory_order_acq_rel);
				};
				chunkJobs.push_back(graph.AddJob(std::move(job)));
			}
			JobGraphTaskGroupDeclaration group;
			group.name = plan.declaration.queryName + ".group";
			group.jobs = chunkJobs;
			group.executionPolicy = plan.executionPolicy == EcsQueryExecutionPolicy::WorkerTaskGroup
				? JobGraphExecutionPolicy::WorkerEligible
				: JobGraphExecutionPolicy::Protected;
			group.profilingLabel = plan.declaration.queryName + "." + PolicyToJobPolicy(plan.executionPolicy);
			DependencyHandle groupHandle = graph.AddTaskGroup(std::move(group));
			queryGroups.push_back(groupHandle);

			for (std::size_t previous = 0; previous < planIndex; ++previous)
			{
				if (!HasWriteConflict(latestSummary.queryPlans[previous].declaration, plan.declaration))
				{
					continue;
				}
				for (const DependencyHandle& chunkHandle : chunkJobs)
				{
					graph.AddDependency(queryGroups[previous], chunkHandle);
				}
			}
		}

		std::map<std::string, DependencyHandle> transformHandles;
		for (const EcsTransformWorkUnit& unit : latestSummary.transformWorkUnits)
		{
			JobGraphJobDeclaration job;
			job.name = "Transform." + unit.workUnitId;
			job.executionPolicy = JobGraphExecutionPolicy::WorkerEligible;
			job.profilingLabel = unit.profilingLabel;
			transformHandles[unit.workUnitId] = graph.AddJob(std::move(job));
		}
		for (const EcsTransformWorkUnit& unit : latestSummary.transformWorkUnits)
		{
			const auto childHandle = transformHandles.find(unit.workUnitId);
			if (childHandle == transformHandles.end())
			{
				continue;
			}
			for (const std::string& parentId : unit.parentDependencies)
			{
				const auto parentHandle = transformHandles.find(parentId);
				if (parentHandle != transformHandles.end())
				{
					graph.AddDependency(parentHandle->second, childHandle->second);
				}
			}
		}

		latestSummary.jobGraphSummary = graph.Execute();
		for (std::size_t index = 0; index < latestSummary.queryPlans.size(); ++index)
		{
			latestSummary.queryPlans[index].processedObjectCount = processed[index]->load(std::memory_order_acquire);
			for (EcsQueryChunk& chunk : latestSummary.queryPlans[index].chunks)
			{
				chunk.state = latestSummary.jobGraphSummary.valid ? EcsQueryResultState::Completed : EcsQueryResultState::Blocked;
			}
		}
		latestSummary.valid = latestSummary.valid && latestSummary.jobGraphSummary.valid;
		latestSummary.completed = latestSummary.jobGraphSummary.completed;
		latestSummary.formatted = options.buildFormattedSummary ? FormatEcsSystemGraphFrameSummary(latestSummary) : std::string{};
		return latestSummary;
	}

	const EcsSystemGraphFrameSummary& EcsQueryTaskGraph::LatestSummary() const
	{
		return latestSummary;
	}
}
