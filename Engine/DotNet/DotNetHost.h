#pragma once

#include "Core/Export.h"
#include <string>
#include <vector>
#include <filesystem>
#include <coreclr_delegates.h>

/**
 * @class DotNetHost
 * @brief Manages the embedding of the .NET CoreCLR runtime using hostfxr.
 * 
 * This class provides a centralized way to initialize the .NET runtime and
 * load managed assemblies. It is designed to be used as a shared utility
 * across different engine systems (e.g., Scripting, Editor).
 */
class TERRAENGINE_API DotNetHost {
public:
    static DotNetHost& Instance();

    /**
     * @brief Initializes the .NET runtime using a specific runtime configuration.
     * @param runtimeConfigPath Path to the .runtimeconfig.json file.
     * @return true if initialization was successful, false otherwise.
     */
    bool Initialize(const std::filesystem::path& runtimeConfigPath);

    /**
     * @brief Loads a managed assembly and provides a function pointer to a static method.
     * @param assemblyPath Absolute path to the .dll assembly.
     * @param typeName Fully qualified name of the type (e.g., "Namespace.Class, AssemblyName").
     * @param methodName Name of the static method to retrieve.
     * @param delegateTypeName Fully qualified name of the delegate type (standard is UNMANAGED_CALLERS_ONLY_METHOD).
     * @return Pointer to the managed function, or nullptr on failure.
     */
    void* GetManagedFunction(
        const std::filesystem::path& assemblyPath,
        const std::wstring& typeName,
        const std::wstring& methodName,
        const std::wstring& delegateTypeName = L"");

    /**
     * @brief Shuts down the .NET runtime and cleans up hostfxr resources.
     */
    void Shutdown();

    bool IsInitialized() const { return initialized; }

private:
    DotNetHost() = default;
    ~DotNetHost();

    void* load_library(const wchar_t* path);
    void* get_export(void* h, const char* name);

    bool load_hostfxr();

    void* hostfxr_handle = nullptr;
    load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer = nullptr;
    void* context_handle = nullptr;

    bool initialized = false;
};
