// Serializer.h — Scene Serialization System Definition
//
// Defines the core logic and records used for translating scene data between runtime objects and persistent storage.
// It handles the conversion of entities, components, and properties to and from the engine's standard JSON formats.
// The system ensures that scene state is correctly preserved during saves and accurately restored during loads.

#pragma once

#include "Core/Object/Object.h"
#include "Renderer/RenderTypes.h"
#include <nlohmann/json.hpp>

// Static utility class that handles converting live Engine objects into JSON
// documents and reconstructing them. This is used for scene saving and the
// Undo/Redo system to capture state snapshots.
class Serializer {
public:
	// Recursively converts an object and all its components/children into a
	// JSON representation.
	static nlohmann::json SerializeObject(Object& obj);

	// Scene-owned render metadata lives beside the object hierarchy in the
	// scene file, so it needs dedicated serialization helpers.
	static nlohmann::json SerializeSceneRenderSettings(const SceneRenderSettings& settings);

	// Legacy scenes may omit the sceneSettings block entirely, so deserialization
	// always falls back to a sanitized default configuration.
	static SceneRenderSettings DeserializeSceneRenderSettings(const nlohmann::json& json);
	
	// Reconstructs an object graph from JSON data. If a parent is provided,
	// the root of the deserialized branch is attached to it. Stable IDs (UUIDs)
	// are restored if present in the source data.
	static Object DeserializeObject(const nlohmann::json& json, std::optional<Object> parent = std::nullopt);
};
