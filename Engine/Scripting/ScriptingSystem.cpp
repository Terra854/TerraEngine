#include "ScriptingSystem.h"
#include "DotNet/DotNetHost.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "ScriptComponent.h"
#include "ThreadPool/ThreadPool.h"
#include "Core/Timer.h"
#include <filesystem>

namespace fs = std::filesystem;

ScriptingSystem& ScriptingSystem::Instance() {
    static ScriptingSystem instance;
    return instance;
}

void ScriptingSystem::Initialize() {
    TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System...");

    // Find managed assembly path relative to executable
    fs::path currentDir = fs::current_path();
    fs::path managedDir = currentDir / "../Binaries/Managed";
    
    if (!fs::exists(managedDir)) {
        managedDir = currentDir / "Build/Binaries/Managed";
    }
    if (!fs::exists(managedDir)) {
        managedDir = currentDir / "Binaries/Managed";
    }

    fs::path configPath = managedDir / "TerraEngine.Core.runtimeconfig.json";
    fs::path assemblyPath = managedDir / "TerraEngine.Core.dll";

    if (!fs::exists(configPath)) {
        TERRAENGINE_LOG(LogLevel::Warning, "Could not find runtimeconfig.json at " << configPath.string() << ". Scripting will be disabled.");
        return;
    }

    if (DotNetHost::Instance().Initialize(configPath)) {
        managedInit = (InitFn)DotNetHost::Instance().GetManagedFunction(
            assemblyPath,
            L"TerraEngine.Core.InternalCalls, TerraEngine.Core",
            L"Initialize"
        );

        managedUpdate = (UpdateFn)DotNetHost::Instance().GetManagedFunction(
            assemblyPath,
            L"TerraEngine.Core.InternalCalls, TerraEngine.Core",
            L"OnUpdate"
        );

        if (managedInit) {
            managedInit();
            TERRAENGINE_LOG(LogLevel::Info, "Scripting System initialized successfully.");
        } else {
            TERRAENGINE_LOG(LogLevel::Error, "Failed to find TerraEngine.Core.InternalCalls.Initialize in " << assemblyPath.string());
        }
    } else {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to initialize .NET Host.");
    }
}

void ScriptingSystem::Update(float deltaTime) {
    if (!managedUpdate) return;

    auto& objManager = ObjectManager::instance();
    auto scriptObjects = objManager.getObjectsWithComponents<ScriptComponent>();

    if (scriptObjects.empty()) return;

    // Execute script updates in parallel
    for (auto& obj : scriptObjects) {
        auto* script = obj.getComponent<ScriptComponent>();
        if (!script || !script->active || script->className.empty()) continue;

        // Enqueue task to thread pool
        auto task = ThreadPool::instance().createTask([this, id = (uint32_t)obj.EntityId(), name = script->className, deltaTime]() {
            managedUpdate(id, name.c_str(), deltaTime);
        });
        ThreadPool::instance().enqueue(task);
    }
}

void ScriptingSystem::Shutdown() {
    DotNetHost::Instance().Shutdown();
}

std::string ScriptingSystem::SystemName() {
    return "ScriptingSystem";
}
