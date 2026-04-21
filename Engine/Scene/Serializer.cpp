#include "Serializer.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
	glm::vec3 ReadVec3OrDefault(const nlohmann::json& jsonValue, const glm::vec3& fallback)
	{
		if (!jsonValue.is_array() || jsonValue.size() != 3)
		{
			return fallback;
		}

		glm::vec3 value = fallback;
		for (std::size_t index = 0; index < 3; ++index)
		{
			if (!jsonValue[index].is_number())
			{
				return fallback;
			}

			value[index] = jsonValue[index].get<float>();
		}

		return value;
	}

	TerraEngine::UUID ReadUuidOrDefault(const nlohmann::json& jsonValue, const TerraEngine::UUID& fallback)
	{
		if (!jsonValue.is_string())
		{
			return fallback;
		}

		if (const auto parsed = TerraEngine::ParseUUID(jsonValue.get<std::string>()))
		{
			return *parsed;
		}

		return fallback;
	}
}

nlohmann::json SerializeObjectRecursive(Object& obj) {
	ObjectManager& objManager = ObjectManager::instance();
	nlohmann::json objJson;
	objJson["name"] = obj.getName();
	objJson["tag"] = obj.getTag();
	objJson["layer"] = obj.getLayer();
	objJson["active"] = obj.isActive();
	objJson["isStatic"] = obj.isStatic();
	
	auto uuid = obj.getUUID();
	objJson["uuid"] = TerraEngine::ToHexString(uuid);
	
	objJson["components"] = nlohmann::json::object();

	// Assumes ComponentAccessRegistryMutex is already locked by the caller (SerializeObject)
	// to prevent deadlocks during recursive traversal of complex hierarchies.
	auto& registry = ComponentAccessRegistry();

	for (auto& [compName, functions] : registry)
	{
		if (compName == "TagComponent") continue; // Skip TagComponent as it's now embedded in Object

		if (functions.hasComponent(obj))
		{
			nlohmann::json compJson = nlohmann::json::object();
			TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType(compName);
			if (TerraEngine::Reflection::IsValid(type))
			{
				// Enumerate all reflected properties and convert their values into
				// JSON-compatible primitives or arrays.
				for (const TerraEngine::Reflection::Property& prop : TerraEngine::Reflection::GetProperties(type))
				{
					std::string propName = TerraEngine::Reflection::GetPropertyName(prop);
					TerraEngine::Reflection::Variant val = functions.getter(obj, propName);
					
					if (TerraEngine::Reflection::IsType<float>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<float>(val);
					else if (TerraEngine::Reflection::IsType<int>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<int>(val);
					else if (TerraEngine::Reflection::IsType<std::uint32_t>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::uint32_t>(val);
					else if (TerraEngine::Reflection::IsType<bool>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<bool>(val);
					else if (TerraEngine::Reflection::IsType<std::string>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::string>(val);
					else if (TerraEngine::Reflection::IsType<glm::vec3>(val)) {
						auto v = TerraEngine::Reflection::GetValue<glm::vec3>(val);
						compJson[propName] = {v.x, v.y, v.z};
					}
					else if (TerraEngine::Reflection::IsType<glm::quat>(val)) {
						auto q = TerraEngine::Reflection::GetValue<glm::quat>(val);
						compJson[propName] = {q.w, q.x, q.y, q.z};
					}
					else if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(val)) {
						auto uuid = TerraEngine::Reflection::GetValue<TerraEngine::UUID>(val);
						compJson[propName] = TerraEngine::ToHexString(uuid);
					}
				}
			}
			objJson["components"][compName] = compJson;
		}
	}
	
	auto children = objManager.getChildren(obj);
	if (!children.empty()) {
		objJson["children"] = nlohmann::json::array();
		for (auto& child : children) {
			objJson["children"].push_back(SerializeObjectRecursive(child));
		}
	}
	return objJson;
}

nlohmann::json Serializer::SerializeObject(Object& obj) {
	// Acquire an exclusive lock once for the entire graph to ensure a consistent
	// snapshot while the recursion proceeds without further locking.
	std::lock_guard lock(ComponentAccessRegistryMutex());
	return SerializeObjectRecursive(obj);
}

nlohmann::json Serializer::SerializeSceneRenderSettings(const SceneRenderSettings& settings)
{
	const SceneRenderSettings sanitizedSettings = SanitizeSceneRenderSettings(settings);
	nlohmann::json sceneSettingsJson = nlohmann::json::object();
	nlohmann::json skyJson = nlohmann::json::object();

	skyJson["enabled"] = sanitizedSettings.sky.enabled;
	skyJson["sourceAsset"] = sanitizedSettings.sky.sourceAsset;
	skyJson["intensity"] = sanitizedSettings.sky.intensity;
	skyJson["exposure"] = sanitizedSettings.sky.exposure;
	skyJson["rotationOffsetDegrees"] = sanitizedSettings.sky.rotationOffsetDegrees;
	skyJson["horizonTint"] = {
		sanitizedSettings.sky.horizonTint.x,
		sanitizedSettings.sky.horizonTint.y,
		sanitizedSettings.sky.horizonTint.z
	};
	skyJson["zenithTint"] = {
		sanitizedSettings.sky.zenithTint.x,
		sanitizedSettings.sky.zenithTint.y,
		sanitizedSettings.sky.zenithTint.z
	};
	skyJson["groundTint"] = {
		sanitizedSettings.sky.groundTint.x,
		sanitizedSettings.sky.groundTint.y,
		sanitizedSettings.sky.groundTint.z
	};
	skyJson["directionalLightObjectId"] = TerraEngine::ToHexString(sanitizedSettings.sky.directionalLightObjectId);

	sceneSettingsJson["sky"] = std::move(skyJson);
	sceneSettingsJson["indirectStrength"] = sanitizedSettings.indirectStrength;
	sceneSettingsJson["ambientOcclusionStrength"] = sanitizedSettings.ambientOcclusionStrength;
	sceneSettingsJson["ambientOcclusionQuality"] = sanitizedSettings.ambientOcclusionQuality;
	return sceneSettingsJson;
}

SceneRenderSettings Serializer::DeserializeSceneRenderSettings(const nlohmann::json& json)
{
	SceneRenderSettings settings = MakeDefaultSceneRenderSettings();
	if (!json.is_object())
	{
		return settings;
	}

	if (const auto skyIt = json.find("sky"); skyIt != json.end() && skyIt->is_object())
	{
		SceneSkySettings sky = settings.sky;
		sky.enabled = skyIt->value("enabled", sky.enabled);
		sky.sourceAsset = skyIt->value("sourceAsset", sky.sourceAsset);
		sky.intensity = skyIt->value("intensity", sky.intensity);
		sky.exposure = skyIt->value("exposure", sky.exposure);
		sky.rotationOffsetDegrees = skyIt->value("rotationOffsetDegrees", sky.rotationOffsetDegrees);
		sky.horizonTint = ReadVec3OrDefault(skyIt->value("horizonTint", nlohmann::json{}), sky.horizonTint);
		sky.zenithTint = ReadVec3OrDefault(skyIt->value("zenithTint", nlohmann::json{}), sky.zenithTint);
		sky.groundTint = ReadVec3OrDefault(skyIt->value("groundTint", nlohmann::json{}), sky.groundTint);
		sky.directionalLightObjectId = ReadUuidOrDefault(
			skyIt->value("directionalLightObjectId", nlohmann::json{}),
			sky.directionalLightObjectId);
		settings.sky = SanitizeSceneSkySettings(sky);
	}

	settings.indirectStrength = json.value("indirectStrength", settings.indirectStrength);
	settings.ambientOcclusionStrength = json.value("ambientOcclusionStrength", settings.ambientOcclusionStrength);
	settings.ambientOcclusionQuality = json.value("ambientOcclusionQuality", settings.ambientOcclusionQuality);

	return SanitizeSceneRenderSettings(settings);
}

Object Serializer::DeserializeObject(const nlohmann::json& json, std::optional<Object> parent) {
	ObjectManager& objManager = ObjectManager::instance();
	std::string name = json.value("name", "Unnamed");
	
	// Identity restoration is critical for maintaining cross-object references 
	// and correctly applying undo/redo steps to the right entities.
	TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
	bool hasUuid = false;

	if (json.contains("uuid")) {
		auto& uuidVal = json["uuid"];
		if (uuidVal.is_string()) {
			auto parsed = TerraEngine::ParseUUID(uuidVal.get<std::string>());
			if (parsed) {
				uuid = *parsed;
				hasUuid = true;
			}
		}
		else if (uuidVal.is_array() && uuidVal.size() == 16) {
			for (int i = 0; i < 16; ++i) uuid[i] = uuidVal[i].get<uint8_t>();
			hasUuid = true;
		}
	}

	Object obj = objManager.createObject(name);
	
	if (hasUuid) {
		// Force the newly created object to use the original identity from the
		// JSON document instead of its automatically generated transient UUID.
		objManager.UpdateEntityUUID(obj.EntityId(), uuid);
	}

	obj.setTag(json.value("tag", "Untagged"));
	obj.setLayer(json.value("layer", "Default"));
	obj.setActive(json.value("active", true));
	obj.setStatic(json.value("isStatic", false));

	if (parent) {
		objManager.setParent(obj, *parent);
	}

	if (json.contains("components")) {
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto& registry = ComponentAccessRegistry();

		for (auto& [compName, compData] : json["components"].items()) {

			if (registry.count(compName)) {
				auto& functions = registry[compName];
				functions.addComponent(obj);

				TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType(compName);
				if (TerraEngine::Reflection::IsValid(type)) {
					for (auto& [propName, propVal] : compData.items()) {
						TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(type, propName);
						if (TerraEngine::Reflection::IsValid(prop)) {
							TerraEngine::Reflection::Variant val;
							// Heuristic-based type reconstruction for JSON variants.
							if (propVal.is_number_float()) val = propVal.get<float>();
							else if (propVal.is_number_integer()) {
								if (TerraEngine::Reflection::IsType<std::uint32_t>(TerraEngine::Reflection::GetPropertyType(prop))) {
									val = propVal.get<std::uint32_t>();
								} else {
									val = propVal.get<int>();
								}
							}
							else if (propVal.is_boolean()) val = propVal.get<bool>();
							else if (propVal.is_string()) {
								if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
									auto parsed = TerraEngine::ParseUUID(propVal.get<std::string>());
									if (parsed) val = *parsed;
								} else {
									val = propVal.get<std::string>();
								}
							}
							else if (propVal.is_array()) {
								if (propVal.size() == 3) {
									val = glm::vec3(propVal[0], propVal[1], propVal[2]);
								} else if (propVal.size() == 4) {
									if (TerraEngine::Reflection::IsType<glm::quat>(TerraEngine::Reflection::GetPropertyType(prop))) {
										val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]); // w, x, y, z
									}
								} else if (propVal.size() == 16 && TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
									TerraEngine::UUID u;
									for(int i=0; i<16; ++i) u[i] = propVal[i].get<uint8_t>();
									val = u;
								}
							}
							
							if (TerraEngine::Reflection::IsValid(val)) {
								functions.setter(obj, propName, val);
							}
						}
					}
				}
			}
		}
	}

	// Recurse to build out the full hierarchy as defined in the source JSON.
	if (json.contains("children") && json["children"].is_array()) {
		for (auto& childJson : json["children"]) {
			DeserializeObject(childJson, obj);
		}
	}
	
	return obj;
}
