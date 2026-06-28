// Serializer.h — Scene Serialization System Definition
//
// Defines the core logic and records used for translating scene data between runtime objects and persistent storage.
// It handles the conversion of entities, components, and properties to and from the engine's standard JSON formats.
// The system ensures that scene state is correctly preserved during saves and accurately restored during loads.

#pragma once

#include "Core/Object/Object.h"
#include "Renderer/RenderTypes.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <optional>

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
	
	// Reconstructs an object graph from JSON data. If a parent is provided,
	// the root of the deserialized branch is attached to it. Stable IDs (UUIDs)
	// are restored if present in the source data.
	static entt::entity DeserializeObjectEntity(const nlohmann::json& json, std::optional<entt::entity> parent = std::nullopt);
	static Object DeserializeObject(const nlohmann::json& json, std::optional<Object> parent = std::nullopt);
};
