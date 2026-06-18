// SceneGraphRuntime.h - deterministic scene graph contracts and helpers.
//
// This module gives runtime, editor, and export code one shared way to describe
// scene hierarchy state. Inputs are live ObjectManager entities or authored JSON
// documents; outputs are stable graph snapshots, diagnostics, traversal order,
// and summaries that can be compared by tests or boot logs.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

	TERRAENGINE_API std::string_view ToString(SceneDiagnosticSeverity severity);
	TERRAENGINE_API std::string_view ToString(SceneDiagnosticCategory category);
	TERRAENGINE_API std::string FormatSceneDiagnostic(const SceneDiagnostic& diagnostic);

	TERRAENGINE_API SceneGraph BuildSceneGraphFromObjectManager(ObjectManager& objects);
	TERRAENGINE_API SceneGraph BuildSceneGraphFromJson(const nlohmann::json& sceneJson);
	TERRAENGINE_API bool ValidateSceneGraph(SceneGraph& graph);
	TERRAENGINE_API std::vector<SceneObjectId> BuildTraversalOrder(const SceneGraph& graph);
	TERRAENGINE_API void ComputeWorldTransforms(SceneGraph& graph);
	TERRAENGINE_API SceneRuntimeSummary BuildSceneRuntimeSummary(const SceneGraph& graph);
	TERRAENGINE_API bool ReparentPreserveWorld(ObjectManager& objects, Object child, std::optional<Object> parent);

}
