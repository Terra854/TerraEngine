// DotNetHost.cpp — Implementation of the .NET CoreCLR and NativeAOT host.
//
// This file implements the logic for embedding the .NET runtime, handling both
// editor-mode hostfxr bootstrapping and export-mode NativeAOT library loading.
// It manages the resolution of managed function pointers and the population of
// the VTable used by the scripting system to communicate with managed code.

#include "DotNetHost.h"
#include "Core/Logger/Logger.h"

#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

// These headers are expected to be in External/dotnet/include or propagated by nethost target
#include <nethost.h>
#include <coreclr_delegates.h>
#include <hostfxr.h>

#include "Scripting/ExportVTable.h"

namespace
{
#ifdef _WIN32
	constexpr const wchar_t* kHostFxrLibraryName = L"hostfxr.dll";
#else
	constexpr const char* kHostFxrLibraryName = "libhostfxr.so";
#endif

	std::basic_string<char_t> ToHostFxrString(const std::wstring& value) {
#ifdef _WIN32
		return value;
#else
		std::basic_string<char_t> converted;
		converted.reserve(value.size());
		for (wchar_t character : value) {
			// Managed metadata names in this engine path are generated from C#
			// identifiers and assembly names, so the current contract is ASCII.
			// Keep the conversion explicit so Linux never passes wchar_t buffers
			// to hostfxr, whose char_t is char on Unix.
			if (character < 0 || character > 0x7f) {
				return {};
			}
			converted.push_back(static_cast<char_t>(character));
		}
		return converted;
#endif
	}

#ifdef _WIN32
	std::string BuildPlatformLoadErrorMessage() {
		const DWORD errorCode = GetLastError();
		if (errorCode == 0) {
			return "Unknown Win32 loader error.";
		}

		char* messageBuffer = nullptr;
		const DWORD messageLength = FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			errorCode,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			reinterpret_cast<LPSTR>(&messageBuffer),
			0,
			nullptr);

		std::string message = messageLength > 0 && messageBuffer != nullptr
			? std::string(messageBuffer, messageLength)
			: "Unknown Win32 loader error.";

		if (messageBuffer != nullptr) {
			LocalFree(messageBuffer);
		}
		return "Win32 error " + std::to_string(errorCode) + ": " + message;
	}
#else

	std::string BuildPlatformLoadErrorMessage() {
		const char* errorText = dlerror();
		return errorText != nullptr ? std::string(errorText) : std::string("Unknown dlopen error.");
	}
#endif
}

DotNetHost& DotNetHost::Instance() {
	static DotNetHost instance;
	return instance;
}

DotNetHost::~DotNetHost() {
	Shutdown();
}


bool DotNetHost::Initialize(const std::filesystem::path& assemblyPath) {
	if (initialized) return true;
	SetNativeError({});


	if (!load_hostfxr(assemblyPath.parent_path())) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to load hostfxr.");
		return false;
	}

	// Get the function pointers for hostfxr
	auto init_fptr = (hostfxr_initialize_for_dotnet_command_line_fn)GetNativeExport(hostfxr_handle, "hostfxr_initialize_for_dotnet_command_line");
	auto get_delegate_fptr = (hostfxr_get_runtime_delegate_fn)GetNativeExport(hostfxr_handle, "hostfxr_get_runtime_delegate");
	auto close_fptr = (hostfxr_close_fn)GetNativeExport(hostfxr_handle, "hostfxr_close");


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
	SetNativeError({});


	if (outTable == nullptr) {
		SetNativeError("DotNetHost::InitializeExportRuntime was called with a null ManagedVTable pointer.");
		TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
		return false;
	}


	if (!std::filesystem::exists(nativeAotLibaryPath)) {
		SetNativeError("NativeAOT runtime library was not found at " + nativeAotLibaryPath.string() + ". Make sure the export output includes the platform runtime library under Managed/ and its native dependencies.");
		TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
		return false;
	}

	// Load the NativeAOT shared library. Unlike hostfxr, this library
	// self-initialises its GC and runtime on DllMain so no CoreCLR host is
	// required. The library handle stays alive for the entire engine session.
	nativeAotLibraryHandle = LoadNativeLibrary(nativeAotLibaryPath);
	if (nativeAotLibraryHandle == nullptr) {
		SetNativeError("Failed to load NativeAOT runtime library " + nativeAotLibaryPath.string() + ". " + BuildPlatformLoadErrorMessage());
		TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
		return false;
	}

	// Prefer the newest capacity-aware entry point. The legacy initializer has
	// no destination-size argument, so current generators deliberately restrict
	// it to the released v1 prefix for memory safety with older native hosts.
	auto initV4Fn = reinterpret_cast<InitManagedRuntimeV4Fn>(
		GetNativeExport(nativeAotLibraryHandle, "InitManagedRuntimeV4"));
	auto initV3Fn = reinterpret_cast<InitManagedRuntimeV3Fn>(
		GetNativeExport(nativeAotLibraryHandle, "InitManagedRuntimeV3"));
	auto initFn = reinterpret_cast<InitManagedRuntimeFn>(
		GetNativeExport(nativeAotLibraryHandle, "InitManagedRuntime"));
	if (initV4Fn == nullptr && initV3Fn == nullptr && initFn == nullptr) {
		SetNativeError("The NativeAOT gameplay library exports neither InitManagedRuntimeV4, InitManagedRuntimeV3, nor the legacy InitManagedRuntime entry point. Rebuild the gameplay project and confirm the source generator produced the export bootstrap.");
		TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
		FreeNativeLibrary(nativeAotLibraryHandle);
		nativeAotLibraryHandle = nullptr;
		return false;
	}

	// Zero-initialise the VTable so any unset pointers are safely detectable.
	std::memset(outTable, 0, sizeof(ManagedVTable));

	// Call the managed init function. The NativeAOT side populates the VTable
	// struct with function pointers from the source-generated script registry.
	const int32_t result = initV4Fn != nullptr
		? initV4Fn(outTable, sizeof(ManagedVTable))
		: (initV3Fn != nullptr ? initV3Fn(outTable, kManagedVTableV3Size) : initFn(outTable));

	TERRAENGINE_LOG(LogLevel::Debug, "--- RAW VTABLE DUMP ---");
	uint64_t* rawPtr = reinterpret_cast<uint64_t*>(outTable);

	for (int i = 0; i < sizeof(ManagedVTable) / 8; ++i) {
		TERRAENGINE_LOG(LogLevel::Debug, "Offset " << (i * 8) << ": " << rawPtr[i]);
	}
	TERRAENGINE_LOG(LogLevel::Debug, "-----------------------");


	if (result != 0) {
		SetNativeError(std::string(initV4Fn != nullptr ? "InitManagedRuntimeV4" : (initV3Fn != nullptr ? "InitManagedRuntimeV3" : "InitManagedRuntime"))
			+ " returned error code " + std::to_string(result)
			+ ". The gameplay NativeAOT library failed to initialize its registry.");
		TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
		FreeNativeLibrary(nativeAotLibraryHandle);
		nativeAotLibraryHandle = nullptr;
		return false;
	}

	// Verify the VTable version is at least what the engine requires.

	if (outTable->version < kManagedVTableMinVersion) {
		SetNativeError("ManagedVTable version " + std::to_string(outTable->version) + " is below the required minimum " + std::to_string(kManagedVTableMinVersion) + ".");
		TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
		FreeNativeLibrary(nativeAotLibraryHandle);
		nativeAotLibraryHandle = nullptr;
		return false;
	}

	// The tail is append-only. Never read its function pointers until both the
	// advertised version and byte size prove the NativeAOT producer wrote them.
	if (outTable->version >= kManagedVTablePhysicsVersion) {
		if (outTable->tableSize < kManagedVTableV2Size) {
			SetNativeError("ManagedVTable v" + std::to_string(outTable->version)
				+ " reported only " + std::to_string(outTable->tableSize)
				+ " bytes; the physics-enabled host requires " + std::to_string(kManagedVTableV2Size) + ".");
			TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
			FreeNativeLibrary(nativeAotLibraryHandle);
			nativeAotLibraryHandle = nullptr;
			return false;
		}
		if (outTable->getPhysicsAbiVersion == nullptr
			|| outTable->dispatchPhysicsEventBatch == nullptr
			|| outTable->getPhysicsAbiVersion() != kManagedPhysicsAbiVersion) {
			SetNativeError("ManagedVTable physics ABI validation failed. Rebuild the gameplay NativeAOT library with the current TerraEngine managed SDK.");
			TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
			FreeNativeLibrary(nativeAotLibraryHandle);
			nativeAotLibraryHandle = nullptr;
			return false;
		}
	}

	// V3 is another append-only tail. Reject a producer that advertises the
	// animation callback without enough bytes or without the exact copied ABI;
	// v1/v2 exports remain loadable and simply do not offer automatic events.
	if (outTable->version >= kManagedVTableAnimationVersion) {
		if (outTable->tableSize < kManagedVTableV3Size) {
			SetNativeError("ManagedVTable v" + std::to_string(outTable->version)
				+ " reported only " + std::to_string(outTable->tableSize)
				+ " bytes; the animation-enabled host requires " + std::to_string(kManagedVTableV3Size) + ".");
			TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
			FreeNativeLibrary(nativeAotLibraryHandle);
			nativeAotLibraryHandle = nullptr;
			return false;
		}
		if (outTable->getAnimationAbiVersion == nullptr
			|| outTable->dispatchAnimationEventBatch == nullptr
			|| outTable->getAnimationAbiVersion() != kManagedAnimationAbiVersion) {
			SetNativeError("ManagedVTable animation ABI validation failed. Rebuild the gameplay NativeAOT library with the current TerraEngine managed SDK.");
			TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
			FreeNativeLibrary(nativeAotLibraryHandle);
			nativeAotLibraryHandle = nullptr;
			return false;
		}
	}

	// V4 roots the navigation managed facade and proves that its fixed-width
	// copied packet contract matches ScriptGlue before NativeAOT gameplay can use
	// it. V1 through V3 libraries remain valid and simply lack this optional tail.
	if (outTable->version >= kManagedVTableNavigationVersion) {
		if (outTable->tableSize < kManagedVTableV4Size) {
			SetNativeError("ManagedVTable v" + std::to_string(outTable->version)
				+ " reported only " + std::to_string(outTable->tableSize)
				+ " bytes; the navigation-enabled host requires " + std::to_string(kManagedVTableV4Size) + ".");
			TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
			FreeNativeLibrary(nativeAotLibraryHandle);
			nativeAotLibraryHandle = nullptr;
			return false;
		}
		if (outTable->getNavigationAbiVersion == nullptr
			|| outTable->getNavigationAbiVersion() != kManagedNavigationAbiVersion) {
			SetNativeError("ManagedVTable navigation ABI validation failed. Rebuild the gameplay NativeAOT library with the current TerraEngine managed SDK.");
			TERRAENGINE_LOG(LogLevel::Error, m_LastNativeError);
			FreeNativeLibrary(nativeAotLibraryHandle);
			nativeAotLibraryHandle = nullptr;
			return false;
		}
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
	
	const std::basic_string<char_t> hostTypeName = ToHostFxrString(typeName);
	const std::basic_string<char_t> hostMethodName = ToHostFxrString(methodName);
	const std::basic_string<char_t> hostDelegateTypeName = ToHostFxrString(delegateTypeName);
	if (hostTypeName.empty() || hostMethodName.empty() || (!delegateTypeName.empty() && hostDelegateTypeName.empty())) {
		TERRAENGINE_LOG(LogLevel::Error, "Managed function names must be ASCII-compatible for hostfxr lookup on this platform.");
		return nullptr;
	}

	// In .NET 5+, UNMANAGEDCALLERSONLY_METHOD tells hostfxr that the managed
	// method is decorated for direct unmanaged calls. When a delegate name is
	// supplied, pass the converted platform string instead.
	const char_t* delegate_type = delegateTypeName.empty() ? UNMANAGEDCALLERSONLY_METHOD : hostDelegateTypeName.c_str();

	int rc = load_assembly_and_get_function_pointer(
		assemblyPath.c_str(),
		hostTypeName.c_str(),
		hostMethodName.c_str(),
		delegate_type,
		nullptr,
		&fptr);


	if (rc != 0) {
		const auto narrowAscii = [](const std::wstring& value) {
			std::string text;
			text.reserve(value.size());
			for (wchar_t character : value)
			{
				text.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
			}
			return text;
		};
		TERRAENGINE_LOG(LogLevel::Error, "Failed to load managed function "
			<< narrowAscii(typeName) << "::" << narrowAscii(methodName)
			<< ": " << std::hex << rc);
		return nullptr;
	}

	return fptr;
}


void DotNetHost::Shutdown() {
	if (hostfxr_handle) {
		FreeNativeLibrary(hostfxr_handle);
		hostfxr_handle = nullptr;
	}


	if (nativeAotLibraryHandle) {
		// The NativeAOT library owns its GC heap, so unloading it cleanly
		// tears down all managed state that was created during the session.
		FreeNativeLibrary(nativeAotLibraryHandle);
		nativeAotLibraryHandle = nullptr;
	}
	exportMode = false;

	initialized = false;
	load_assembly_and_get_function_pointer = nullptr;
	m_LastNativeError.clear();
}


bool DotNetHost::load_hostfxr(const std::filesystem::path& searchPath) {
	// 1. Try to load from the specified search path (e.g., Managed directory)
	if (!searchPath.empty()) {
		std::filesystem::path localHostFxr = searchPath / std::filesystem::path(kHostFxrLibraryName);
		if (std::filesystem::exists(localHostFxr)) {
			hostfxr_handle = LoadNativeLibrary(localHostFxr);

			if (hostfxr_handle) {
				TERRAENGINE_LOG(LogLevel::Info, "Loaded local hostfxr from: " << localHostFxr.string());
				return true;
			}
		}
	}

	// 2. Fallback to nethost lookup (System installed)
	char_t buffer[4096];
	size_t buffer_size = sizeof(buffer) / sizeof(char_t);
	int rc = get_hostfxr_path(buffer, &buffer_size, nullptr);

	if (rc != 0) {
		return false;
	}

	hostfxr_handle = LoadNativeLibrary(buffer);
	return hostfxr_handle != nullptr;
}


void* DotNetHost::LoadNativeLibrary(const std::filesystem::path& path) {
#ifdef _WIN32
	HMODULE moduleHandle = LoadLibraryW(path.c_str());

	if (moduleHandle == nullptr) {
		SetNativeError("Failed to load native library " + path.string() + ". " + BuildPlatformLoadErrorMessage());
	}
	return reinterpret_cast<void*>(moduleHandle);
#else
	dlerror();
	void* moduleHandle = dlopen(path.c_str(), RTLD_NOW);
	if (moduleHandle == nullptr) {
		SetNativeError("Failed to load native library " + path.string() + ". " + BuildPlatformLoadErrorMessage());
	}
	return moduleHandle;
#endif
}

void* DotNetHost::GetNativeExport(void* libraryHandle, const char* exportName) {

	if (libraryHandle == nullptr || exportName == nullptr) {
		return nullptr;
	}

#ifdef _WIN32
	void* exportPointer = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(libraryHandle), exportName));
#else
	dlerror();
	void* exportPointer = dlsym(libraryHandle, exportName);
#endif

	if (exportPointer == nullptr) {
		SetNativeError("Failed to locate export '" + std::string(exportName) + "'. " + BuildPlatformLoadErrorMessage());
	}
	return exportPointer;
}


void DotNetHost::FreeNativeLibrary(void* libraryHandle) {
	if (libraryHandle == nullptr) {
		return;
	}

#ifdef _WIN32
	FreeLibrary(static_cast<HMODULE>(libraryHandle));
#else
	dlclose(libraryHandle);
#endif
}

void DotNetHost::SetNativeError(const std::string& message) {
	m_LastNativeError = message;
}
