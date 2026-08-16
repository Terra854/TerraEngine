// SceneGraphRuntime.h - deterministic scene graph contracts and helpers.
//
// This module gives runtime, editor, and export code one shared way to describe
// scene hierarchy state. Inputs are live ObjectManager entities or authored JSON
// documents; outputs are stable graph snapshots, diagnostics, traversal order,
// and summaries that can be compared by tests or boot logs.

#pragma once

#include "Core/Export.h"
#include "Components/Transform.h"
#include "Utilities/UUID.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ObjectManager;
enum class ReparentMode : std::uint8_t;

namespace TerraEngine::Scene {

	using SceneObjectId = TerraEngine::UUID;
	struct HierarchySnapshot;
	class SceneChangePacket;

	enum class SceneDiagnosticSeverity {
		Info,
		Warning,
		Error,
		Fatal
	};

	enum class SceneDiagnosticCategory {
		Hierarchy,
		Composition,
		Mutation,
		Parity,
		Serialization,
		ExportStartup
	};

	struct SceneDiagnostic {
		SceneDiagnosticSeverity severity = SceneDiagnosticSeverity::Info;
		SceneDiagnosticCategory category = SceneDiagnosticCategory::Hierarchy;
		SceneObjectId objectId = TerraEngine::EMPTY_UUID;
		std::string requestId;
		std::string message;
		std::string recommendedAction;
	};

	struct SceneGraphObject {
		SceneObjectId objectId = TerraEngine::EMPTY_UUID;
		entt::entity entity = entt::null;
		SceneObjectId parentId = TerraEngine::EMPTY_UUID;
		std::vector<SceneObjectId> children;
		std::string name;
		// Canonical world pose is the only absolute coordinate authority. Local
		// hierarchy operations use the separate bounded transform space record.
		WorldPose worldPose{};
		ParentRelativePose parentRelativePose{};
		bool active = true;
		std::uint64_t transformRevision = 0;
		std::uint64_t spatialRevision = 0;
	};

	struct SceneGraph {
		SceneObjectId sceneId = TerraEngine::EMPTY_UUID;
		// The object-manager lifetime generation rejects packets prepared for a
		// scene that was cleared or atomically replaced after this graph snapshot.
		std::uint64_t sceneGeneration = 0;
		std::uint64_t hierarchyRevision = 0;
		std::vector<SceneObjectId> rootObjects;
		std::unordered_map<SceneObjectId, SceneGraphObject> objects;
		std::vector<SceneDiagnostic> diagnostics;
		std::uint64_t revision = 0;
	};

	struct SceneSummaryObject {
		SceneObjectId objectId = TerraEngine::EMPTY_UUID;
		SceneObjectId parentId = TerraEngine::EMPTY_UUID;
		std::string name;
		std::uint32_t depth = 0;
		WorldPosition worldPosition{};
	};

	struct SceneRuntimeSummary {
		std::vector<SceneSummaryObject> objects;
		std::uint64_t graphRevision = 0;
		std::vector<SceneDiagnostic> diagnostics;
	};

	struct SceneTransformWorkUnit {
		std::string workUnitId;
		std::uint32_t hierarchyLevel = 0;
		std::vector<SceneObjectId> objects;
		std::vector<std::string> parentDependencies;
	};

	struct SceneTransformWorkPlan {
		bool valid = true;
		std::vector<SceneTransformWorkUnit> units;
		std::vector<SceneDiagnostic> diagnostics;
	};

	enum class SceneTransformDirtyReason : std::uint8_t {
		LocalTransform,
		Hierarchy,
		SceneLoad,
		FullRefresh
	};

	struct SceneDirtyTransformState {
		bool valid = true;
		bool fullRefreshRequested = true;
		SceneTransformDirtyReason reason = SceneTransformDirtyReason::FullRefresh;
		std::uint64_t sourceSceneGeneration = 0;
		std::uint64_t sourceRevision = 0;
		std::vector<SceneObjectId> dirtyRoots;
		// Excludes direct roots so consumers can distinguish authored changes
		// from descendants whose world transforms changed through propagation.
		std::vector<SceneObjectId> affectedDescendants;
		std::vector<SceneObjectId> expandedDirtyObjects;
	};

	struct SceneWorldTransformRecord {
		SceneObjectId objectId = TerraEngine::EMPTY_UUID;
		entt::entity entity = entt::null;
		WorldPose worldPose{};
		std::uint32_t hierarchyLevel = 0;
		std::uint64_t sourceRevision = 0;
	};

	struct SceneTransformPropagationResult {
		bool valid = true;
		std::uint64_t graphRevision = 0;
		std::uint32_t hierarchyLevelCount = 0;
		std::uint32_t dirtyRootCount = 0;
		std::uint32_t expandedDirtyObjectCount = 0;
		std::uint32_t processedObjectCount = 0;
		std::uint32_t skippedCleanObjectCount = 0;
		std::size_t workerJobCount = 0;
		std::size_t protectedJobCount = 0;
		std::size_t inlineJobCount = 0;
		std::uint64_t coordinatorWaitTimeNs = 0;
		std::uint64_t workerTimeNs = 0;
		double workerUtilization = 0.0;
		std::vector<SceneWorldTransformRecord> records;
		std::unordered_map<SceneObjectId, WorldPose> worldTransformsByObject;
		std::unordered_map<entt::entity, WorldPose> worldTransformsByEntity;
		std::vector<SceneDiagnostic> diagnostics;
	};

	TERRAENGINE_API std::string_view ToString(SceneDiagnosticSeverity severity);
	TERRAENGINE_API std::string_view ToString(SceneDiagnosticCategory category);
	TERRAENGINE_API std::string FormatSceneDiagnostic(const SceneDiagnostic& diagnostic);

	TERRAENGINE_API SceneGraph BuildSceneGraphFromObjectManager(ObjectManager& objects, bool computeWorldTransforms = true);
	TERRAENGINE_API SceneGraph BuildSceneGraphFromJson(const nlohmann::json& sceneJson);
	TERRAENGINE_API bool ValidateSceneGraph(SceneGraph& graph);
	TERRAENGINE_API std::vector<SceneObjectId> BuildTraversalOrder(const SceneGraph& graph);
	TERRAENGINE_API void ComputeWorldTransforms(SceneGraph& graph);
	TERRAENGINE_API SceneDirtyTransformState BuildDirtyTransformState(const SceneGraph& graph, const std::vector<SceneObjectId>& dirtyRoots = {}, SceneTransformDirtyReason reason = SceneTransformDirtyReason::FullRefresh);
	TERRAENGINE_API SceneDirtyTransformState BuildDirtyTransformState(const SceneGraph& graph, const SceneChangePacket& changes);
	TERRAENGINE_API SceneTransformWorkPlan BuildTransformWorkPlan(const SceneGraph& graph);
	TERRAENGINE_API SceneTransformWorkPlan BuildTransformWorkPlan(const SceneGraph& graph, const HierarchySnapshot& snapshot);
	TERRAENGINE_API SceneTransformPropagationResult PropagateWorldTransforms(SceneGraph& graph, const SceneDirtyTransformState& dirtyState = {});
	TERRAENGINE_API SceneRuntimeSummary BuildSceneRuntimeSummary(const SceneGraph& graph);
	TERRAENGINE_API std::uint64_t ObserveHierarchyRevision(const ObjectManager& objects);
	TERRAENGINE_API std::uint64_t ObserveHierarchyRevision(const SceneGraph& graph);
	TERRAENGINE_API bool ReparentPreserveWorld(ObjectManager& objects, entt::entity child, std::optional<entt::entity> parent);
	TERRAENGINE_API bool ReparentWithMode(ObjectManager& objects, entt::entity child, std::optional<entt::entity> parent, ReparentMode mode);

}
