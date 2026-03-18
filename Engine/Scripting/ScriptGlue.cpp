#include "ScriptGlue.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Object.h"
#include "Core/Object/Component.h"
#include "Core/Object/Transform.h"
#include <mutex>
#include <shared_mutex>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"

static std::shared_mutex g_ScriptGlueMutex;

enum class ScriptType : int
{
    Bool = 0,
    Int = 1,
    Float = 2,
    String = 3,
    Vector2 = 4,
    Vector3 = 5,
    Vector4 = 6,
    Quaternion = 7,
    EntityID = 8
};

static rttr::variant MarshallFromNative(void* data, ScriptType type)
{
    switch (type)
    {
    case ScriptType::Bool: return *static_cast<bool*>(data);
    case ScriptType::Int: return *static_cast<int*>(data);
    case ScriptType::Float: return *static_cast<float*>(data);
    case ScriptType::String: return std::string(static_cast<const char*>(data));
    case ScriptType::Vector2: return *static_cast<glm::vec2*>(data);
    case ScriptType::Vector3: return *static_cast<glm::vec3*>(data);
    case ScriptType::Vector4: return *static_cast<glm::vec4*>(data);
    case ScriptType::Quaternion: return *static_cast<glm::quat*>(data);
    case ScriptType::EntityID: return *static_cast<uint32_t*>(data);
    }
    return {};
}

static void MarshallToNative(const rttr::variant& var, void* data, ScriptType type)
{
    if (!var.is_valid()) return;

    switch (type)
    {
    case ScriptType::Bool: *static_cast<bool*>(data) = var.convert<bool>(); break;
    case ScriptType::Int: *static_cast<int*>(data) = var.convert<int>(); break;
    case ScriptType::Float: *static_cast<float*>(data) = var.convert<float>(); break;
    case ScriptType::String: 
        // Strings are tricky with void*. Assuming caller provided buffer or we can't easily do it.
        // For GetProperty with String, we might need a separate call or fixed buffer.
        // For now, ignoring String output to void*.
        break;
    case ScriptType::Vector2: *static_cast<glm::vec2*>(data) = var.convert<glm::vec2>(); break;
    case ScriptType::Vector3: *static_cast<glm::vec3*>(data) = var.convert<glm::vec3>(); break;
    case ScriptType::Vector4: *static_cast<glm::vec4*>(data) = var.convert<glm::vec4>(); break;
    case ScriptType::Quaternion: *static_cast<glm::quat*>(data) = var.convert<glm::quat>(); break;
    case ScriptType::EntityID: *static_cast<uint32_t*>(data) = var.convert<uint32_t>(); break;
    }
}

extern "C" {

    void Native_Log(int level, const char* message, const char* function, int line) {
        LogLevel logLevel = static_cast<LogLevel>(level);
        Logger::Instance().Log(logLevel, "C#", line, function, message, true);
    }

    uint32_t Native_Object_Create(const char* name) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        Object obj = ObjectManager::instance().createObject(name ? name : "New Object");
        return static_cast<uint32_t>(obj.EntityId());
    }

    void Native_Object_Destroy(uint32_t objectId) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            ObjectManager::instance().removeObject(*objOpt);
        }
    }

    bool Native_HasComponent(uint32_t objectId, const char* componentType) {
        std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                return it->second.hasComponent(*objOpt);
            }
        }
        return false;
    }

    bool Native_AddComponent(uint32_t objectId, const char* componentType) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                it->second.addComponent(*objOpt);
                return true;
            }
        }
        return false;
    }

    void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type) {
        std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                rttr::variant val = it->second.getter(*objOpt, propertyName);
                MarshallToNative(val, outValue, static_cast<ScriptType>(type));
            }
        }
    }

    void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type) {
        std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                rttr::variant val = MarshallFromNative(inValue, static_cast<ScriptType>(type));
                it->second.setter(*objOpt, propertyName, val);
            }
        }
    }

    void Native_Graphics_SetQualityPreset(uint32_t preset) {
        RenderSystem::Instance().SetQualityPreset(static_cast<QualityPreset>(preset));
    }

    uint32_t Native_Graphics_GetQualityPreset() {
        return static_cast<uint32_t>(RenderSystem::Instance().GetQualityPreset());
    }

    void Native_Graphics_SetRenderSettings(RenderSettings* settings) {
        if (settings) {
            RenderSystem::Instance().SetRenderSettings(*settings);
        }
    }

    void Native_Graphics_GetRenderSettings(RenderSettings* outSettings) {
        if (outSettings) {
            *outSettings = RenderSystem::Instance().GetRenderSettings();
        }
    }

    const char* Native_ResourceManager_GetAssetRoot() {
        static std::string assetRoot;
        assetRoot = ResourceManager::Instance().GetAssetRoot().string();
        return assetRoot.c_str();
    }

}
