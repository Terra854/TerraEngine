#include "ScriptingSystem.h"
#include "DotNet/DotNetHost.h"
#include "Core/Logger/Logger.h"
#include <filesystem>

namespace fs = std::filesystem;

ScriptingSystem& ScriptingSystem::Instance() {
    static ScriptingSystem instance;
    return instance;
}

void ScriptingSystem::Initialize() {
    TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System...");

    // Find managed assembly path relative to executable
    // In our build layout: Build/Launcher/TerraEngine-Launcher.exe
    // Managed DLLs: Build/Binaries/Managed/TerraEngine.Core.dll
    
    fs::path currentDir = fs::current_path();
    fs::path managedDir = currentDir / "../Binaries/Managed";
    
    // Fallback search
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

void ScriptingSystem::Update() {}

void ScriptingSystem::Shutdown() {
    DotNetHost::Instance().Shutdown();
}

std::string ScriptingSystem::SystemName() {
    return "ScriptingSystem";
}
