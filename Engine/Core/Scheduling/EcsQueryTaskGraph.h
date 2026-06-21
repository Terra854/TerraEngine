// EcsQueryTaskGraph.h - component-aware query planning over the general job graph.
//
// This layer does not replace the existing SystemScheduler or JobGraph. It adds
// the ECS-specific contract those lower layers intentionally avoid knowing:
// component read/write declarations, deterministic object snapshots, chunking,
// deferred scene mutation boundaries, and hierarchy/transform work units.

#pragma once

#include "ComponentAccess.h"
#include "JobGraph.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SceneMutationQueue.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace TerraEngine::Scheduling {

	enum class EcsQueryExecutionPolicy {
		Protected,
		Inline,
		WorkerTaskGroup
	};

	enum class EcsQueryResultState {
		Pending,
		Running,
		Completed,
		Failed,
		Cancelled,
		Blocked
	};

	struct EcsQueryChunk {
		std::size_t chunkIndex = 0;
		std::size_t begin = 0;
		std::size_t end = 0;
		std::vector<TerraEngine::UUID> objects;
		std::string profilingLabel;
		EcsQueryResultState state = EcsQueryResultState::Pending;
	};

	using EcsQueryChunkWork = std::function<void(const EcsQueryChunk&, JobGraphContext&)>;

	struct EcsComponentQueryDeclaration {
		std::string queryName;
		ComponentAccessDeclaration access;
		std::vector<TerraEngine::UUID> matchingObjects;
		bool includeDisabled = false;
		bool preserveMatchingObjectOrder = false;
		bool copyObjectIdsIntoChunks = true;
		bool runOnceWithoutObjectSnapshot = false;
		std::size_t workerThreshold = 128;
		std::size_t preferredChunkSize = 256;
		EcsQueryChunkWork work;
	};

	struct EcsComponentQueryPlan {
		EcsComponentQueryDeclaration declaration;
		std::vector<TerraEngine::UUID> matchingObjects;
		std::vector<EcsQueryChunk> chunks;
		EcsQueryExecutionPolicy executionPolicy = EcsQueryExecutionPolicy::Protected;
		std::size_t processedObjectCount = 0;
		bool invalidatedByMutation = false;
	};

	struct EcsTransformWorkUnit {
		std::string workUnitId;
		std::uint32_t hierarchyLevel = 0;
		std::vector<TerraEngine::Scene::SceneObjectId> objects;
		std::vector<std::string> parentDependencies;
		std::string profilingLabel;
	};

	struct EcsRaceDiagnostic {
		ComponentAccessDiagnosticSeverity severity = ComponentAccessDiagnosticSeverity::Info;
		std::string code;
		std::string systemName;
		std::string queryName;
		std::string details;
		std::string blockedWork;
	};

	struct EcsSystemGraphFrameSummary {
		bool valid = true;
		bool completed = false;
		std::vector<EcsComponentQueryPlan> queryPlans;
		std::vector<EcsRaceDiagnostic> diagnostics;
		std::vector<std::string> mutationFlushes;
		std::vector<EcsTransformWorkUnit> transformWorkUnits;
		std::size_t transformWorkObjectCount = 0;
		std::size_t spatialRefreshBatchCount = 0;
		JobGraphSummary jobGraphSummary;
		std::string formatted;
	};

	struct EcsQueryTaskGraphOptions {
		std::string graphId = "ecs-query-task-graph";
		std::string label = "ECS Query Task Graph";
		bool buildFormattedSummary = true;
		bool validateAccessDeclarations = true;
	};

	TERRAENGINE_API std::string ToString(EcsQueryExecutionPolicy policy);
	TERRAENGINE_API std::string ToString(EcsQueryResultState state);
	TERRAENGINE_API std::string FormatEcsSystemGraphFrameSummary(const EcsSystemGraphFrameSummary& summary);
	TERRAENGINE_API bool EcsGraphFrameDiagnosticsEnabled();
	TERRAENGINE_API void SetEcsGraphFrameDiagnosticsRequested(bool requested);
	TERRAENGINE_API void ClearPublishedEcsGraphFrameSummaries();
	TERRAENGINE_API void PublishEcsGraphFrameSummary(const EcsSystemGraphFrameSummary& summary);
	TERRAENGINE_API std::vector<EcsSystemGraphFrameSummary> PublishedEcsGraphFrameSummaries();
	TERRAENGINE_API std::vector<std::string> PublishedEcsComponentQueryGroups();
	TERRAENGINE_API std::string FormatPublishedEcsGraphFrameSummaries();

	class TERRAENGINE_API EcsQueryTaskGraph {
	public:
		explicit EcsQueryTaskGraph(EcsQueryTaskGraphOptions options = {});

		void Reset(EcsQueryTaskGraphOptions options = {});
		void AddQuery(EcsComponentQueryDeclaration declaration);
		void AddTransformWorkUnit(EcsTransformWorkUnit workUnit);
		void EnqueueMutation(TerraEngine::Scene::SceneMutationRequest request);
		TerraEngine::Scheduling::MutationFlushRuntimeState FlushMutations(const std::string& flushPointName);

		const EcsSystemGraphFrameSummary& Validate();
		const EcsSystemGraphFrameSummary& Execute();
		const EcsSystemGraphFrameSummary& LatestSummary() const;

	private:
		EcsQueryTaskGraphOptions options;
		std::vector<EcsComponentQueryDeclaration> queries;
		std::vector<EcsTransformWorkUnit> transformUnits;
		EcsSystemGraphFrameSummary latestSummary;
	};

}
