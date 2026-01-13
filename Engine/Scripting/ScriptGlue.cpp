#include "ScriptGlue.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Object.h"
#include "Core/Object/Component.h"
#include "Core/Object/Transform.h"
#include <mutex>

static std::mutex g_ScriptGlueMutex;

extern "C" {

    void Native_Log(int level, const char* message, const char* function, int line) {
        LogLevel logLevel = static_cast<LogLevel>(level);
        Logger::Instance().Log(logLevel, "C#", line, function, message, true);
    }

    uint32_t Native_Object_Create(const char* name) {
        std::lock_guard<std::mutex> lock(g_ScriptGlueMutex);
        Object obj = ObjectManager::instance().createObject(name ? name : "New Object");
        return static_cast<uint32_t>(obj.EntityId());
    }

    void Native_Object_Destroy(uint32_t objectId) {
        std::lock_guard<std::mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            ObjectManager::instance().removeObject(*objOpt);
        }
    }

    void Native_Transform_GetPosition(uint32_t objectId, float* x, float* y, float* z) {
        std::lock_guard<std::mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            if (auto* transform = objOpt->getComponent<Transform>()) {
                glm::vec3 pos = GetComponentProperty<Transform, glm::vec3>(*transform, "Position");
                *x = pos.x;
                *y = pos.y;
                *z = pos.z;
            }
        }
    }

    void Native_Transform_SetPosition(uint32_t objectId, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            if (auto* transform = objOpt->getComponent<Transform>()) {
                SetComponentProperty<Transform, glm::vec3>(*transform, "Position", glm::vec3(x, y, z));
            }
        }
    }

}