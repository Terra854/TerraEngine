#include "ScriptingSystem.h"
#include "DotNet/DotNetHost.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "ScriptComponent.h"
#include "ThreadPool/ThreadPool.h"
#include "Core/Timer.h"
#include "Resource/ResourceManager.h"
#include <filesystem>

namespace fs = std::filesystem;

ScriptingSystem& ScriptingSystem::Instance() {
    static ScriptingSystem instance;
    return instance;
}

void ScriptingSystem::Initialize() {
    TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System...");

    // Find managed assembly path relative to executable
    fs::path managedDir = ResourceManager::Instance().GetAssetRoot() / "Managed";

    fs::path configPath = managedDir / "TerraEngine.CSharp.runtimeconfig.json";
    fs::path assemblyPath = managedDir / "TerraEngine.CSharp.dll";

    if (!fs::exists(configPath)) {
        TERRAENGINE_LOG(LogLevel::Warning, "Could not find runtimeconfig.json at " << configPath.string() << ". Scripting will be disabled.");
        return;
    }

    if (DotNetHost::Instance().Initialize(assemblyPath)) {
        managedInit = (InitFn)DotNetHost::Instance().GetManagedFunction(
            assemblyPath,
            L"TerraEngine.InternalCalls, TerraEngine.CSharp",
            L"Initialize"
        );

        managedUpdate = (UpdateFn)DotNetHost::Instance().GetManagedFunction(
            assemblyPath,
            L"TerraEngine.InternalCalls, TerraEngine.CSharp",
            L"OnUpdate"
        );

        if (managedInit) {
            managedInit();
            TERRAENGINE_LOG(LogLevel::Info, "Scripting System initialized successfully.");
        } else {
            TERRAENGINE_LOG(LogLevel::Error, "Failed to find TerraEngine.InternalCalls.Initialize in " << assemblyPath.string());
        }
    } else {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to initialize .NET Host.");
    }
}

void ScriptingSystem::Update(float deltaTime) {
    if (!managedUpdate) return;

    if (!m_Running) return;

    if (m_Paused) {
        if (m_StepFrames > 0) {
            m_StepFrames--;
        } else {
            return;
        }
    }

    auto& objManager = ObjectManager::instance();
    auto scriptObjects = objManager.getObjectsWithComponents<ScriptComponent>();

    if (scriptObjects.empty()) return;

    struct ScriptBatchItem {
        uint32_t id;
        std::string name;
    };

    const size_t totalScripts = scriptObjects.size();
    const size_t threadCount = std::max<size_t>(1, ThreadPool::instance().getThreadCount());
    
    // Target multiple tasks per thread to ensure efficient load balancing
    // across available workers.
    const size_t targetTasks = threadCount * 4; 
    
    // Calculate the optimal batch size based on the target task count.
    size_t batchSize = totalScripts / targetTasks;
    
    // Enforce a minimum batch size to prevent excessive scheduling overhead
    // when processing a small number of scripts.
    if (batchSize < 32) {
        batchSize = 32;
    }

    std::vector<ScriptBatchItem> currentBatch;
    currentBatch.reserve(batchSize);

    // Execute script updates in parallel batches
    for (auto& obj : scriptObjects) {
        auto* script = obj.getComponent<ScriptComponent>();
        if (!script || !script->active || script->className.empty()) continue;

        currentBatch.push_back({ (uint32_t)obj.EntityId(), script->className });

        if (currentBatch.size() >= batchSize) {
            auto task = ThreadPool::instance().createTask([items = std::move(currentBatch), this, deltaTime]() {
                for (const auto& item : items) {
                    managedUpdate(item.id, item.name.c_str(), deltaTime);
                }
            });
            ThreadPool::instance().enqueue(task);
            
            // Re-initialize for next batch (moved vector is in unspecified state)
            currentBatch = std::vector<ScriptBatchItem>();
            currentBatch.reserve(batchSize);
        }
    }

    // Process remaining items
    if (!currentBatch.empty()) {
        auto task = ThreadPool::instance().createTask([items = std::move(currentBatch), this, deltaTime]() {
            for (const auto& item : items) {
                managedUpdate(item.id, item.name.c_str(), deltaTime);
            }
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
