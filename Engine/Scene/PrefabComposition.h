// PrefabComposition.h - prefab source, instance, override, and authoring contracts.
//
// Runtime composition stays separate from the editor UI so exported builds can
// resolve prefab instances without depending on ImGui or editor-only metadata.

#pragma once

#include "Core/Export.h"
#include "SceneGraphRuntime.h"
#include "Utilities/UUID.h"

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
	};

	struct PrefabSource {
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		std::filesystem::path sourcePath;
		SceneGraph sourceGraph;
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
		std::vector<SceneDiagnostic> diagnostics;
	};

	TERRAENGINE_API bool ValidatePrefabSource(PrefabSource& source);
	TERRAENGINE_API PrefabCompositionResult ResolvePrefabInstance(const PrefabSource& source, const PrefabInstance& instance);
	TERRAENGINE_API std::vector<SceneDiagnostic> DetectPrefabDependencyCycles(const std::unordered_map<TerraEngine::UUID, PrefabSource>& sources);
	TERRAENGINE_API void MarkStaleOverrides(PrefabInstance& instance, const PrefabSource& source);
	TERRAENGINE_API void ApplyOverridesToDefaults(PrefabSource& source, const PrefabInstance& instance, const std::vector<PrefabOverride>& selectedOverrides);
	TERRAENGINE_API void ResetOverridesToDefaults(PrefabInstance& instance, const std::optional<TerraEngine::UUID>& targetObjectId = std::nullopt);
	TERRAENGINE_API std::vector<SceneDiagnostic> ValidatePrefabDelete(const PrefabSource& source, const std::vector<PrefabInstance>& instances, bool convertInstances);

}
