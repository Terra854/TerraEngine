#pragma once

#include "Core/Object/Object.h"
#include <nlohmann/json.hpp>

// Static utility class that handles converting live Engine objects into JSON
// documents and reconstructing them. This is used for scene saving and the
// Undo/Redo system to capture state snapshots.
class Serializer {
public:
    // Recursively converts an object and all its components/children into a
    // JSON representation.
    static nlohmann::json SerializeObject(Object& obj);
    
    // Reconstructs an object graph from JSON data. If a parent is provided,
    // the root of the deserialized branch is attached to it. Stable IDs (UUIDs)
    // are restored if present in the source data.
    static Object DeserializeObject(const nlohmann::json& json, std::optional<Object> parent = std::nullopt);
};
