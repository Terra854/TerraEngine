#include "DotNetHost.h"
#include "Core/Logger/Logger.h"

#include <iostream>
#include <Windows.h>

// These headers are expected to be in External/dotnet/include or propagated by nethost target
#include <nethost.h>
#include <coreclr_delegates.h>
#include <hostfxr.h>

DotNetHost& DotNetHost::Instance() {
    static DotNetHost instance;
    return instance;
}

DotNetHost::~DotNetHost() {
    Shutdown();
}

bool DotNetHost::Initialize(const std::filesystem::path& runtimeConfigPath) {
    if (initialized) return true;

    if (!load_hostfxr()) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to load hostfxr.");
        return false;
    }

    // Get the function pointers for hostfxr
    auto init_fptr = (hostfxr_initialize_for_runtime_config_fn)get_export(hostfxr_handle, "hostfxr_initialize_for_runtime_config");
    auto get_delegate_fptr = (hostfxr_get_runtime_delegate_fn)get_export(hostfxr_handle, "hostfxr_get_runtime_delegate");
    auto close_fptr = (hostfxr_close_fn)get_export(hostfxr_handle, "hostfxr_close");

    if (!init_fptr || !get_delegate_fptr || !close_fptr) {
        TERRAENGINE_LOG(LogLevel::Error, "Failed to get hostfxr function pointers.");
        return false;
    }

    // Initialize the runtime
    int rc = init_fptr(runtimeConfigPath.c_str(), nullptr, &context_handle);
    if (rc != 0 || context_handle == nullptr) {
        TERRAENGINE_LOG(LogLevel::Error, "hostfxr_initialize_for_runtime_config failed: " << std::hex << rc);
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
    initialized = false;
    load_assembly_and_get_function_pointer = nullptr;
}

bool DotNetHost::load_hostfxr() {
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
