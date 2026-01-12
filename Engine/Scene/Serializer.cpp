#include "Serializer.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include "Core/Object/IDComponent.h"
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

nlohmann::json SerializeObjectRecursive(Object& obj) {
    ObjectManager& objManager = ObjectManager::instance();
    nlohmann::json objJson;
    objJson["name"] = objManager.getObjectName(obj);
    
    objJson["components"] = nlohmann::json::object();

    // Assumes ComponentAccessRegistryMutex is already locked by the caller (SerializeObject)
    // to prevent deadlocks during recursive traversal of complex hierarchies.
    auto& registry = ComponentAccessRegistry();

    for (auto& [compName, functions] : registry)
    {
        if (functions.hasComponent(obj))
        {
            nlohmann::json compJson = nlohmann::json::object();
            rttr::type type = rttr::type::get_by_name(compName);
            if (type.is_valid())
            {
                // Enumerate all reflected properties and convert their values into
                // JSON-compatible primitives or arrays.
                for (auto& prop : type.get_properties())
                {
                    std::string propName = prop.get_name().to_string();
                    rttr::variant val = functions.getter(obj, propName);
                    
                    if (val.is_type<float>()) compJson[propName] = val.get_value<float>();
                    else if (val.is_type<int>()) compJson[propName] = val.get_value<int>();
                    else if (val.is_type<bool>()) compJson[propName] = val.get_value<bool>();
                    else if (val.is_type<std::string>()) compJson[propName] = val.get_value<std::string>();
                    else if (val.is_type<glm::vec3>()) {
                        auto v = val.get_value<glm::vec3>();
                        compJson[propName] = {v.x, v.y, v.z};
                    }
                    else if (val.is_type<glm::quat>()) {
                        auto q = val.get_value<glm::quat>();
                        compJson[propName] = {q.w, q.x, q.y, q.z};
                    }
                    else if (val.is_type<TerraEngine::UUID>()) {
                        auto uuid = val.get_value<TerraEngine::UUID>();
                        nlohmann::json uuidJson = nlohmann::json::array();
                        for (auto b : uuid) uuidJson.push_back(b);
                        compJson[propName] = uuidJson;
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

Object Serializer::DeserializeObject(const nlohmann::json& json, std::optional<Object> parent) {
    ObjectManager& objManager = ObjectManager::instance();
    std::string name = json.value("name", "Unnamed");
    
    // Identity restoration is critical for maintaining cross-object references 
    // and correctly applying undo/redo steps to the right entities.
    TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
    bool hasUuid = false;
    if (json.contains("components") && json["components"].contains("IDComponent")) {
        auto& idJson = json["components"]["IDComponent"];
        if (idJson.contains("id")) {
            auto& uuidArr = idJson["id"];
            if (uuidArr.is_array() && uuidArr.size() == 16) {
                for (int i = 0; i < 16; ++i) uuid[i] = uuidArr[i].get<uint8_t>();
                hasUuid = true;
            }
        }
    }

    Object obj = objManager.createObject(name);
    
    if (hasUuid) {
        // Force the newly created object to use the original identity from the
        // JSON document instead of its automatically generated transient UUID.
        objManager.UpdateEntityUUID(obj.EntityId(), uuid);
    }

    if (parent) {
        objManager.setParent(obj, *parent);
    }

    if (json.contains("components")) {
        std::lock_guard lock(ComponentAccessRegistryMutex());
        auto& registry = ComponentAccessRegistry();

        for (auto& [compName, compData] : json["components"].items()) {
            if (compName == "IDComponent") continue; // ID handled above

            if (registry.count(compName)) {
                auto& functions = registry[compName];
                functions.addComponent(obj);

                rttr::type type = rttr::type::get_by_name(compName);
                if (type.is_valid()) {
                    for (auto& [propName, propVal] : compData.items()) {
                        rttr::property prop = type.get_property(propName);
                        if (prop.is_valid()) {
                            rttr::variant val;
                            // Heuristic-based type reconstruction for JSON variants.
                            if (propVal.is_number_float()) val = propVal.get<float>();
                            else if (propVal.is_number_integer()) val = propVal.get<int>();
                            else if (propVal.is_boolean()) val = propVal.get<bool>();
                            else if (propVal.is_string()) val = propVal.get<std::string>();
                            else if (propVal.is_array()) {
                                if (propVal.size() == 3) {
                                    val = glm::vec3(propVal[0], propVal[1], propVal[2]);
                                } else if (propVal.size() == 4) {
                                    if (prop.get_type() == rttr::type::get<glm::quat>()) {
                                        val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]); // w, x, y, z
                                    }
                                } else if (propVal.size() == 16 && prop.get_type() == rttr::type::get<TerraEngine::UUID>()) {
                                     TerraEngine::UUID u;
                                     for(int i=0; i<16; ++i) u[i] = propVal[i].get<uint8_t>();
                                     val = u;
                                }
                            }
                            
                            if (val.is_valid()) {
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
