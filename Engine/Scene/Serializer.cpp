// Serializer.cpp — Scene Serialization System Implementation
//
// Implements the detailed serialization and deserialization logic for the engine's scene and prefab formats.
// It manages the recursive processing of entity hierarchies and coordinates with the reflection system to handle component data.
// This module provides the backend support for scene persistence, allowing for stable asset storage and versioning.

#include "Scene/Serializer.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

// SerializeObjectRecursive is the entity-based scene snapshot path. It reads
// authoritative state through ObjectManager so serialization does not need the
// legacy mutable Object facade.
nlohmann::json SerializeObjectRecursive(entt::entity entity) {
	ObjectManager& objManager = ObjectManager::instance();
	entt::registry& enttRegistry = objManager.getRegistry();
	nlohmann::json objJson;
	objJson["name"] = objManager.getObjectName(entity);
	objJson["tag"] = objManager.getObjectTag(entity);
	objJson["layer"] = objManager.getObjectLayer(entity);
	objJson["active"] = objManager.isObjectActive(entity);
	objJson["isStatic"] = objManager.isObjectStatic(entity);
	
	auto uuid = objManager.getUUID(entity);
	objJson["uuid"] = TerraEngine::ToHexString(uuid);
	
	objJson["components"] = nlohmann::json::object();

	// Assumes ComponentAccessRegistryMutex is already locked by the caller (SerializeObject)
	// to prevent deadlocks during recursive traversal of complex hierarchies.
	auto& registry = ComponentAccessRegistry();

	for (auto& [compName, functions] : registry)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (compName == "TagComponent") continue; // Skip TagComponent as it's now embedded in Object

		if (functions.entityHasComponent && functions.entityHasComponent(enttRegistry, entity))
		{
			nlohmann::json compJson = nlohmann::json::object();
			TerraEngine::Reflection::Type type = functions.reflectionType;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (TerraEngine::Reflection::IsValid(type))
			{
				// Enumerate all reflected properties and convert their values into
				// JSON-compatible primitives or arrays.
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const TerraEngine::Reflection::Property& prop : TerraEngine::Reflection::GetProperties(type))
				{
					std::string propName = TerraEngine::Reflection::GetPropertyName(prop);
					TerraEngine::Reflection::Variant val = functions.entityGetter(enttRegistry, entity, propName);
					
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (TerraEngine::Reflection::IsType<float>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<float>(val);
					else if (TerraEngine::Reflection::IsType<int>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<int>(val);
					else if (TerraEngine::Reflection::IsType<std::uint32_t>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::uint32_t>(val);
					else if (TerraEngine::Reflection::IsType<bool>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<bool>(val);
					else if (TerraEngine::Reflection::IsType<std::string>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::string>(val);
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (TerraEngine::Reflection::IsType<glm::vec3>(val)) {
						auto v = TerraEngine::Reflection::GetValue<glm::vec3>(val);
						compJson[propName] = {v.x, v.y, v.z};
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (TerraEngine::Reflection::IsType<glm::quat>(val)) {
						auto q = TerraEngine::Reflection::GetValue<glm::quat>(val);
						compJson[propName] = {q.w, q.x, q.y, q.z};
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(val)) {
						auto uuid = TerraEngine::Reflection::GetValue<TerraEngine::UUID>(val);
						compJson[propName] = TerraEngine::ToHexString(uuid);
					}
				}
			}
			objJson["components"][compName] = compJson;
		}
	}
	
	auto children = objManager.getChildEntities(entity);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!children.empty()) {
		objJson["children"] = nlohmann::json::array();
		for (entt::entity child : children) {
			objJson["children"].push_back(SerializeObjectRecursive(child));
		}
	}
	return objJson;
}

// Serializer::SerializeObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
nlohmann::json Serializer::SerializeObject(entt::entity entity) {
	// Acquire an exclusive lock once for the entire graph to ensure a consistent
	// snapshot while the recursion proceeds without further locking.
	std::lock_guard lock(ComponentAccessRegistryMutex());
	return SerializeObjectRecursive(entity);
}

nlohmann::json Serializer::SerializeObject(Object& obj) {
	return SerializeObject(obj.EntityId());
}

// Serializer::SerializeSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
nlohmann::json Serializer::SerializeSceneRenderSettings(const SceneRenderSettings& settings)
{
	const SceneRenderSettings sanitizedSettings = SanitizeSceneRenderSettings(settings);
	nlohmann::json sceneSettingsJson = nlohmann::json::object();

	// Sky authoring now belongs to SkyComponent on scene objects. This scene
	// settings block intentionally writes only non-sky renderer settings.
	sceneSettingsJson["indirectStrength"] = sanitizedSettings.indirectStrength;
	sceneSettingsJson["ambientOcclusionStrength"] = sanitizedSettings.ambientOcclusionStrength;
	sceneSettingsJson["ambientOcclusionQuality"] = sanitizedSettings.ambientOcclusionQuality;
	return sceneSettingsJson;
}

// Serializer::DeserializeSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SceneRenderSettings Serializer::DeserializeSceneRenderSettings(const nlohmann::json& json)
{
	SceneRenderSettings settings = MakeDefaultSceneRenderSettings();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!json.is_object())
	{
		return settings;
	}

	// Obsolete sceneSettings.sky data is unsupported by design and ignored as
	// if absent. Do not convert it into component data here.

	settings.indirectStrength = json.value("indirectStrength", settings.indirectStrength);
	settings.ambientOcclusionStrength = json.value("ambientOcclusionStrength", settings.ambientOcclusionStrength);
	settings.ambientOcclusionQuality = json.value("ambientOcclusionQuality", settings.ambientOcclusionQuality);

	return SanitizeSceneRenderSettings(settings);
}

// Serializer::DeserializeObjectEntity restores authored state through manager
// APIs so stale identity and component access stay centralized.
entt::entity Serializer::DeserializeObjectEntity(const nlohmann::json& json, std::optional<entt::entity> parent) {
	ObjectManager& objManager = ObjectManager::instance();
	entt::registry& enttRegistry = objManager.getRegistry();
	std::string name = json.value("name", "Unnamed");
	
	// Identity restoration is critical for maintaining cross-object references 
	// and correctly applying undo/redo steps to the right entities.
	TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
	bool hasUuid = false;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (json.contains("uuid")) {
		auto& uuidVal = json["uuid"];
		if (uuidVal.is_string()) {
			auto parsed = TerraEngine::ParseUUID(uuidVal.get<std::string>());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (parsed) {
				uuid = *parsed;
				hasUuid = true;
			}
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (uuidVal.is_array() && uuidVal.size() == 16) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (int i = 0; i < 16; ++i) uuid[i] = uuidVal[i].get<uint8_t>();
			hasUuid = true;
		}
	}

	Object obj = objManager.createObject(name);
	const entt::entity entity = obj.EntityId();
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasUuid) {
		// Force the newly created object to use the original identity from the
		// JSON document instead of its automatically generated transient UUID.
		objManager.UpdateEntityUUID(entity, uuid);
	}

	objManager.setObjectTag(entity, json.value("tag", "Untagged"));
	objManager.setObjectLayer(entity, json.value("layer", "Default"));
	objManager.setObjectActive(entity, json.value("active", true));
	objManager.setObjectStatic(entity, json.value("isStatic", false));

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parent) {
		objManager.setParent(entity, *parent);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (json.contains("components")) {
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto& registry = ComponentAccessRegistry();

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& [compName, compData] : json["components"].items()) {

			if (registry.count(compName)) {
				auto& functions = registry[compName];
				if (functions.entityAddComponent)
				{
					functions.entityAddComponent(enttRegistry, entity);
					objManager.notifyObjectStructureChanged(entity);
				}

					TerraEngine::Reflection::Type type = functions.reflectionType;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (TerraEngine::Reflection::IsValid(type)) {
					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (auto& [propName, propVal] : compData.items()) {
						TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(type, propName);
						if (TerraEngine::Reflection::IsValid(prop)) {
							TerraEngine::Reflection::Variant val;
							// Heuristic-based type reconstruction for JSON variants.
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (propVal.is_number_float()) val = propVal.get<float>();
							else if (propVal.is_number_integer()) {
								if (TerraEngine::Reflection::IsType<std::uint32_t>(TerraEngine::Reflection::GetPropertyType(prop))) {
									val = propVal.get<std::uint32_t>();
								} else {
									val = propVal.get<int>();
								}
							}
							else if (propVal.is_boolean()) val = propVal.get<bool>();
							// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
							else if (propVal.is_string()) {
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
									auto parsed = TerraEngine::ParseUUID(propVal.get<std::string>());
									if (parsed) val = *parsed;
								} else {
									val = propVal.get<std::string>();
								}
							}
							// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
							else if (propVal.is_array()) {
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (propVal.size() == 3) {
									val = glm::vec3(propVal[0], propVal[1], propVal[2]);
								} else if (propVal.size() == 4) {
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (TerraEngine::Reflection::IsType<glm::quat>(TerraEngine::Reflection::GetPropertyType(prop))) {
										val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]); // w, x, y, z
									}
								} else if (propVal.size() == 16 && TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
									TerraEngine::UUID u;
									// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
									for(int i=0; i<16; ++i) u[i] = propVal[i].get<uint8_t>();
									val = u;
								}
							}
							
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (TerraEngine::Reflection::IsValid(val)) {
								functions.entitySetter(enttRegistry, entity, propName, val);
							}
						}
					}
				}
			}
		}
	}

	// Recurse to build out the full hierarchy as defined in the source JSON.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (json.contains("children") && json["children"].is_array()) {
		for (auto& childJson : json["children"]) {
			DeserializeObjectEntity(childJson, entity);
		}
	}
	
	return entity;
}

Object Serializer::DeserializeObject(const nlohmann::json& json, std::optional<Object> parent) {
	std::optional<entt::entity> parentEntity;
	if (parent)
	{
		parentEntity = parent->EntityId();
	}
	const entt::entity entity = DeserializeObjectEntity(json, parentEntity);
	return *ObjectManager::instance().getObject(entity);
}
