#include "DotNetHost.h"
#include "Core/Logger/Logger.h"

#include <iostream>
#include <Windows.h>

// These headers are expected to be in External/dotnet/include or propagated by nethost target
#include <nethost.h>
#include <coreclr_delegates.h>
#include <hostfxr.h>

#include "Scripting/ExportVTable.h"

DotNetHost& DotNetHost::Instance() {
    static DotNetHost instance;
    return instance;
}

DotNetHost::~DotNetHost() {
    Shutdown();
}

bool DotNetHost::Initialize(const std::filesystem::path& assemblyPath) {
    if (initialized) return true;

    if (!load_hostfxr(assemblyPath.parent_path())) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to load hostfxr.");
        return false;
    }

    // Get the function pointers for hostfxr
    auto init_fptr = (hostfxr_initialize_for_dotnet_command_line_fn)get_export(hostfxr_handle, "hostfxr_initialize_for_dotnet_command_line");
    auto get_delegate_fptr = (hostfxr_get_runtime_delegate_fn)get_export(hostfxr_handle, "hostfxr_get_runtime_delegate");
    auto close_fptr = (hostfxr_close_fn)get_export(hostfxr_handle, "hostfxr_close");

    if (!init_fptr || !get_delegate_fptr || !close_fptr) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to get hostfxr function pointers.");
        return false;
    }

    // Initialize the runtime
    const char_t* argv[] = { assemblyPath.c_str() };
    int rc = init_fptr(1, argv, nullptr, &context_handle);
    if (rc != 0 || context_handle == nullptr) {
        TERRAENGINE_LOG(LogLevel::Error, "hostfxr_initialize_for_dotnet_command_line failed: " << std::hex << rc);
        close_fptr(context_handle);
        return false;
    }

    // Get the load_assembly_and_get_function_pointer delegate
    rc = get_delegate_fptr(
        context_handle,
        hdt_load_assembly_and_get_function_pointer,
        (void**)&load_assembly_and_get_function_pointer);

    if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to get load_assembly_and_get_function_pointer delegate: " << std::hex << rc);
        close_fptr(context_handle);
        return false;
    }

    close_fptr(context_handle);
    context_handle = nullptr;

    initialized = true;
    TERRAENGINE_LOG(LogLevel::Info, ".NET Runtime initialized successfully.");
    return true;
}

bool DotNetHost::InitializeExportRuntime(const std::filesystem::path& nativeAotLibaryPath, ManagedVTable* outTable) {
    if (initialized) return true;

    if (outTable == nullptr) {
        TERRAENGINE_LOG(LogLevel::Error, "DotNetHost::InitializeExportRuntime called with null VTable pointer.");
        return false;
    }

    if (!std::filesystem::exists(nativeAotLibaryPath)) {
        TERRAENGINE_LOG(LogLevel::Error, "NativeAOT library not found: " << nativeAotLibaryPath.string());
        return false;
    }

    // Load the NativeAOT shared library. Unlike hostfxr, this library
    // self-initialises its GC and runtime on DllMain so no CoreCLR host is
    // required. The library handle stays alive for the entire engine session.
    nativeAotLibraryHandle = load_library(nativeAotLibaryPath.c_str());
    if (nativeAotLibraryHandle == nullptr) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to load NativeAOT library: " << nativeAotLibaryPath.string());
        return false;
    }

    // Resolve the single exported C function that populates the VTable.
    auto initFn = reinterpret_cast<InitManagedRuntimeFn>(
        get_export(nativeAotLibraryHandle, "InitManagedRuntime"));
    if (initFn == nullptr) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to find InitManagedRuntime in NativeAOT library.");
        FreeLibrary(static_cast<HMODULE>(nativeAotLibraryHandle));
        nativeAotLibraryHandle = nullptr;
        return false;
    }

    // Zero-initialise the VTable so any unset pointers are safely detectable.
    std::memset(outTable, 0, sizeof(ManagedVTable));

    // Call the managed init function. The NativeAOT side populates the VTable
    // struct with function pointers from the source-generated script registry.
    const int32_t result = initFn(outTable);

    TERRAENGINE_LOG(LogLevel::Debug, "--- RAW VTABLE DUMP ---");
    uint64_t* rawPtr = reinterpret_cast<uint64_t*>(outTable);
    for (int i = 0; i < sizeof(ManagedVTable) / 8; ++i) {
        TERRAENGINE_LOG(LogLevel::Debug, "Offset " << (i * 8) << ": " << rawPtr[i]);
    }
    TERRAENGINE_LOG(LogLevel::Debug, "-----------------------");

    if (result != 0) {
        TERRAENGINE_LOG(LogLevel::Error, "InitManagedRuntime returned error code " << result);
        FreeLibrary(static_cast<HMODULE>(nativeAotLibraryHandle));
        nativeAotLibraryHandle = nullptr;
        return false;
    }

    // Verify the VTable version is at least what the engine requires.
    if (outTable->version < kManagedVTableMinVersion) {
        TERRAENGINE_LOG(LogLevel::Error, "ManagedVTable version " << outTable->version
            << " is below the required minimum " << kManagedVTableMinVersion);
        FreeLibrary(static_cast<HMODULE>(nativeAotLibraryHandle));
        nativeAotLibraryHandle = nullptr;
        return false;
    }

    initialized = true;
    exportMode = true;
    TERRAENGINE_LOG(LogLevel::Info, "NativeAOT export runtime initialized (VTable v" << outTable->version << ").");
    return true;
}

void* DotNetHost::GetManagedFunction(
    const std::filesystem::path& assemblyPath,
    const std::wstring& typeName,
    const std::wstring& methodName,
    const std::wstring& delegateTypeName) {

    if (!initialized || !load_assembly_and_get_function_pointer) {
        TERRAENGINE_LOG(LogLevel::Error, "DotNetHost not initialized.");
        return nullptr;
    }

    void* fptr = nullptr;
    
    // In .NET 5+, we can use UNMANAGEDCALLERSONLY_METHOD if delegateTypeName is empty
    const wchar_t* delegate_type = delegateTypeName.empty() ? UNMANAGEDCALLERSONLY_METHOD : delegateTypeName.c_str();

    int rc = load_assembly_and_get_function_pointer(
        assemblyPath.c_str(),
        typeName.c_str(),
        methodName.c_str(),
        delegate_type,
        nullptr,
        &fptr);

    if (rc != 0) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to load managed function: " << std::hex << rc);
        return nullptr;
    }

    return fptr;
}

void DotNetHost::Shutdown() {
    if (hostfxr_handle) {
        FreeLibrary((HMODULE)hostfxr_handle);
        hostfxr_handle = nullptr;
    }

    if (nativeAotLibraryHandle) {
        // The NativeAOT library owns its GC heap, so unloading it cleanly
        // tears down all managed state that was created during the session.
        FreeLibrary(static_cast<HMODULE>(nativeAotLibraryHandle));
        nativeAotLibraryHandle = nullptr;
    }
    exportMode = false;

    initialized = false;
    load_assembly_and_get_function_pointer = nullptr;
}

bool DotNetHost::load_hostfxr(const std::filesystem::path& searchPath) {
    // 1. Try to load from the specified search path (e.g., Managed directory)
    if (!searchPath.empty()) {
        std::filesystem::path localHostFxr = searchPath / "hostfxr.dll";
        if (std::filesystem::exists(localHostFxr)) {
            hostfxr_handle = load_library(localHostFxr.c_str());
            if (hostfxr_handle) {
                TERRAENGINE_LOG(LogLevel::Info, "Loaded local hostfxr from: " << localHostFxr.string());
                return true;
            }
        }
    }

    // 2. Fallback to nethost lookup (System installed)
    wchar_t buffer[MAX_PATH];
    size_t buffer_size = sizeof(buffer) / sizeof(wchar_t);
    int rc = get_hostfxr_path(buffer, &buffer_size, nullptr);
    if (rc != 0) {
        return false;
    }

    hostfxr_handle = load_library(buffer);
    return hostfxr_handle != nullptr;
}

void* DotNetHost::load_library(const wchar_t* path) {
    HMODULE h = LoadLibraryW(path);
    return (void*)h;
}

void* DotNetHost::get_export(void* h, const char* name) {
    void* f = GetProcAddress((HMODULE)h, name);
    return f;
}
