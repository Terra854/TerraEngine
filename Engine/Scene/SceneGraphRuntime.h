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
#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Object;
class ObjectManager;

namespace TerraEngine::Scene {

	using SceneObjectId = TerraEngine::UUID;

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
		glm::vec3 localPosition{0.0f, 0.0f, 0.0f};
		glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 localScale{1.0f, 1.0f, 1.0f};
		glm::vec3 worldPosition{0.0f, 0.0f, 0.0f};
		glm::quat worldRotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 worldScale{1.0f, 1.0f, 1.0f};
		bool active = true;
		std::uint64_t transformRevision = 0;
		std::uint64_t spatialRevision = 0;
	};

	struct SceneGraph {
		SceneObjectId sceneId = TerraEngine::EMPTY_UUID;
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
		glm::vec3 worldPosition{0.0f, 0.0f, 0.0f};
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
		bool fullRefreshRequested = true;
		SceneTransformDirtyReason reason = SceneTransformDirtyReason::FullRefresh;
		std::uint64_t sourceRevision = 0;
		std::vector<SceneObjectId> dirtyRoots;
		std::vector<SceneObjectId> expandedDirtyObjects;
	};

	struct SceneWorldTransformRecord {
		SceneObjectId objectId = TerraEngine::EMPTY_UUID;
		entt::entity entity = entt::null;
		Transform worldTransform{};
		glm::mat4 modelMatrix{1.0f};
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
		std::vector<SceneWorldTransformRecord> records;
		std::unordered_map<SceneObjectId, Transform> worldTransformsByObject;
		std::unordered_map<entt::entity, Transform> worldTransformsByEntity;
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
	TERRAENGINE_API SceneTransformWorkPlan BuildTransformWorkPlan(const SceneGraph& graph);
	TERRAENGINE_API SceneTransformPropagationResult PropagateWorldTransforms(SceneGraph& graph, const SceneDirtyTransformState& dirtyState = {});
	TERRAENGINE_API SceneRuntimeSummary BuildSceneRuntimeSummary(const SceneGraph& graph);
	TERRAENGINE_API bool ReparentPreserveWorld(ObjectManager& objects, Object child, std::optional<Object> parent);

}
