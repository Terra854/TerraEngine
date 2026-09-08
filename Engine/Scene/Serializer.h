// Serializer.h — Scene Serialization System Definition
//
// Defines the core logic and records used for translating scene data between runtime objects and persistent storage.
// It handles the conversion of entities, components, and properties to and from the engine's standard JSON formats.
// The system ensures that scene state is correctly preserved during saves and accurately restored during loads.

#pragma once

#include "Core/Object/Object.h"
#include "Navigation/NavigationSettings.h"
#include "Renderer/RenderTypes.h"
#include "Scene/WorldCoordinate.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <functional>
#include <string>
#include <vector>

namespace TerraEngine::Scene {
	// Authored files must identify this exact contract.  We deliberately do not
	// infer legacy layouts because that can silently lose unbounded coordinates.
	inline constexpr std::string_view kTerraSceneUnboundedFormat = "TerraSceneUnboundedV1";
	inline constexpr std::string_view kTerraPrefabUnboundedFormat = "TerraPrefabUnboundedV1";
	// A prepared record owns only parsed authored data.  It is intentionally
	// independent of ObjectManager so worker jobs can build it before the one
	// protected bulk-publication boundary.
	struct PreparedSceneObjectRecord {
		nlohmann::json object;
		std::string stablePath;
		std::optional<std::size_t> parentRecordIndex;
	};

	struct PreparedSceneDecode {
		bool accepted = false;
		bool cancelled = false;
		std::string reason;
		std::uint64_t objectCount = 0U;
		std::uint64_t componentCount = 0U;
		std::uint64_t hierarchyCount = 0U;
		std::vector<PreparedSceneObjectRecord> objects;
	};
}

// Static utility class that handles converting live Engine objects into JSON
// documents and reconstructing them. This is used for scene saving and the
// Undo/Redo system to capture state snapshots.
class Serializer {
public:
	// Recursively converts an object and all its components/children into a
	// JSON representation.
	static nlohmann::json SerializeObject(entt::entity entity);
	static nlohmann::json SerializeObject(Object& obj);

	// Non-sky scene render metadata lives beside the object hierarchy. Sky data
	// is component-owned, and obsolete sceneSettings.sky input is ignored.
	static nlohmann::json SerializeSceneRenderSettings(const SceneRenderSettings& settings);

	// Scenes may omit the sceneSettings block entirely, so deserialization falls
	// back to sanitized defaults for the remaining non-sky settings.
	static SceneRenderSettings DeserializeSceneRenderSettings(const nlohmann::json& json);

	// Navigation profiles and limits are durable project values rather than live
	// provider state. Deserializers build and validate a detached candidate first;
	// malformed JSON therefore returns no value and cannot partially update state.
	static nlohmann::json SerializeNavigationProfile(const TerraEngine::Navigation::NavigationProfile& profile);
	static std::optional<TerraEngine::Navigation::NavigationProfile> DeserializeNavigationProfile(
		const nlohmann::json& json, std::string* outError = nullptr);
	static nlohmann::json SerializeNavigationSettings(const TerraEngine::Navigation::NavigationSettings& settings);
	static std::optional<TerraEngine::Navigation::NavigationSettings> DeserializeNavigationSettings(
		const nlohmann::json& json, std::string* outError = nullptr);
	
	// Reconstructs an object graph from JSON data. If a parent is provided,
	// the root of the deserialized branch is attached to it. Stable IDs (UUIDs)
	// are restored if present in the source data.
	static entt::entity DeserializeObjectEntity(const nlohmann::json& json, std::optional<entt::entity> parent = std::nullopt);
	static Object DeserializeObject(const nlohmann::json& json, std::optional<Object> parent = std::nullopt);

	// Extract a deterministic, value-only scene table from authored JSON.  The
	// optional cancellation callback is safe to invoke on a worker thread; this
	// method never reads or mutates live ECS storage.
	static TerraEngine::Scene::PreparedSceneDecode PrepareSceneDecode(
		const nlohmann::json& sceneJson,
		const std::function<bool()>& cancellationRequested = {});
	// Prefabs have their own strict authored envelope even though their object
	// graph uses the same immutable worker-side decode records as a scene.
	static TerraEngine::Scene::PreparedSceneDecode PreparePrefabDecode(
		const nlohmann::json& prefabJson,
		const std::function<bool()>& cancellationRequested = {});

	// Cells are strings so every signed 64-bit value has an exact JSON spelling.
	// Offsets remain numbers and must already be canonical before they are read.
	static nlohmann::json SerializeWorldPosition(const TerraEngine::Scene::WorldPosition& position);
	static std::optional<TerraEngine::Scene::WorldPosition> DeserializeWorldPosition(const nlohmann::json& value, std::string* outError = nullptr);
	static bool ValidateUnboundedSceneDocument(const nlohmann::json& document, std::string* outError = nullptr);
	static bool ValidateUnboundedPrefabDocument(const nlohmann::json& document, std::string* outError = nullptr);
};
