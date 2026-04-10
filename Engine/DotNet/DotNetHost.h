#pragma once

#include "Core/Export.h"
#include <string>
#include <vector>
#include <filesystem>
#include <coreclr_delegates.h>

// Forward-declare the export-mode VTable struct so the header stays lightweight.
// The full definition lives in Scripting/ExportVTable.h and is only needed by
// callers that actually consume the VTable after initialization.
struct ManagedVTable;

/**
 * @class DotNetHost
 * @brief Manages the embedding of the .NET CoreCLR runtime using hostfxr.
 * 
 * This class provides a centralized way to initialize the .NET runtime and
 * load managed assemblies. It is designed to be used as a shared utility
 * across different engine systems (e.g., Scripting, Editor).
 *
 * The host also supports export-mode NativeAOT bootstrapping by loading a
 * native gameplay library via LoadLibrary and calling its InitManagedRuntime
 * entry point to populate a ManagedVTable struct.
 */
class TERRAENGINE_API DotNetHost {
public:
    static DotNetHost& Instance();

    /**
     * @brief Initializes the .NET runtime (editor mode via hostfxr).
     * @param assemblyPath Path to the managed assembly (.dll).
     * @return true if initialization was successful, false otherwise.
     */
    bool Initialize(const std::filesystem::path& assemblyPath);

    /**
     * @brief Initializes the managed runtime in export mode by loading a NativeAOT
     *        shared library and calling its InitManagedRuntime entry point.
     * @param nativeAotLibaryPath Absolute path to the NativeAOT .dll.
     * @param outTable Pointer to the ManagedVTable struct to populate.
     * @return true if the NativeAOT library loaded and the VTable was populated.
     */
    bool InitializeExportRuntime(const std::filesystem::path& nativeAotLibaryPath, ManagedVTable* outTable);

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

    /// Returns true when the host was initialized in export-mode (NativeAOT).
    bool IsExportMode() const { return exportMode; }
    const std::string& GetLastNativeError() const { return m_LastNativeError; }

private:
    DotNetHost() = default;
    ~DotNetHost();

    void* LoadNativeLibrary(const std::filesystem::path& path);
    void* GetNativeExport(void* libraryHandle, const char* exportName);
    void FreeNativeLibrary(void* libraryHandle);
    void SetNativeError(const std::string& message);

    bool load_hostfxr(const std::filesystem::path& searchPath = "");

    void* hostfxr_handle = nullptr;
    load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer = nullptr;
    void* context_handle = nullptr;

    bool initialized = false;

    // Export-mode state: the NativeAOT library handle stays alive for the
    // entire engine session so the managed GC heap remains valid.
    void* nativeAotLibraryHandle = nullptr;
    bool exportMode = false;
    std::string m_LastNativeError;
};
