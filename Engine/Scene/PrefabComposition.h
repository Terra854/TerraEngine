// PrefabComposition.h - prefab source, instance, override, and authoring contracts.
//
// Runtime composition stays separate from the editor UI so exported builds can
// resolve prefab instances without depending on ImGui or editor-only metadata.

#pragma once

#include "Core/Export.h"
#include "SceneGraphRuntime.h"
#include "Utilities/UUID.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Scene {

	enum class PrefabOverrideKind {
		Property,
		ComponentAdded,
		ComponentRemoved,
		ObjectAdded,
		ObjectRemoved
	};

	struct PrefabOverride {
		TerraEngine::UUID targetObjectId = TerraEngine::EMPTY_UUID;
		std::string componentName;
		std::string propertyPath;
		std::string authoredValue;
		PrefabOverrideKind kind = PrefabOverrideKind::Property;
		bool stale = false;
		// Nested physics collections address a record by this stable authored ID,
		// never by its mutable vector index.  Empty means a normal component field.
		TerraEngine::UUID recordId = TerraEngine::EMPTY_UUID;
		// Connected constraints retain their source identity in prefab defaults and
		// resolve this value through sourceToInstanceObjectIds on instantiation.
		TerraEngine::UUID connectedObjectSourceId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID connectedObjectInstanceId = TerraEngine::EMPTY_UUID;
	};

	struct PrefabSource {
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		std::filesystem::path sourcePath;
		SceneGraph sourceGraph;
		// Component defaults are keyed by stable source object UUID and component
		// name. Nested physics records remain canonical JSON with their record IDs.
		std::unordered_map<TerraEngine::UUID, std::unordered_map<std::string, nlohmann::json>> componentDefaults;
		std::vector<TerraEngine::UUID> nestedSourceIds;
		bool deleted = false;
	};

	struct PrefabInstance {
		TerraEngine::UUID instanceId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID rootObjectId = TerraEngine::EMPTY_UUID;
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID> sourceToInstanceObjectIds;
		std::vector<PrefabOverride> overrides;
		bool convertedToObjects = false;
	};

	struct PrefabAuthoringSession {
		TerraEngine::UUID sessionId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		std::filesystem::path editingPath;
		bool dirty = false;
		std::vector<SceneDiagnostic> diagnostics;
	};

	struct PrefabCompositionResult {
		bool success = true;
		PrefabInstance instance;
		SceneGraph resolvedGraph;
		std::unordered_map<TerraEngine::UUID, std::unordered_map<std::string, nlohmann::json>> resolvedComponentDefaults;
		std::vector<SceneDiagnostic> diagnostics;
	};

	TERRAENGINE_API bool ValidatePrefabSource(PrefabSource& source);
	TERRAENGINE_API PrefabCompositionResult ResolvePrefabInstance(const PrefabSource& source, const PrefabInstance& instance);
	TERRAENGINE_API std::vector<SceneDiagnostic> DetectPrefabDependencyCycles(const std::unordered_map<TerraEngine::UUID, PrefabSource>& sources);
	TERRAENGINE_API void MarkStaleOverrides(PrefabInstance& instance, const PrefabSource& source);
	TERRAENGINE_API void ApplyOverridesToDefaults(PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& selectedOverrides);
	TERRAENGINE_API bool TryApplyOverridesToDefaults(PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& selectedOverrides, std::vector<SceneDiagnostic>& diagnostics);
	TERRAENGINE_API std::optional<TerraEngine::UUID> RemapConnectedObjectUuid(const PrefabInstance& instance, TerraEngine::UUID sourceObjectId);
	TERRAENGINE_API bool ValidatePrefabOverrideBatch(const PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& overrides, std::vector<SceneDiagnostic>& diagnostics);
	TERRAENGINE_API void ResetOverridesToDefaults(PrefabInstance& instance, const std::optional<TerraEngine::UUID>& targetObjectId = std::nullopt);
	TERRAENGINE_API std::vector<SceneDiagnostic> ValidatePrefabDelete(const PrefabSource& source, const std::vector<PrefabInstance>& instances, bool convertInstances);

}
