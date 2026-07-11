cmake_minimum_required(VERSION 3.22.1)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(TerraEngine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Linux links several vendored static libraries into the TerraEngine shared
# object. Position-independent code is required for that link step, so enable it
# before any third-party targets are declared.
if(UNIX AND NOT APPLE)
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include(CTest)

# Configure-time guard: centralize platform names so native, managed, and
# install rules all agree about the host layout they are producing.
if(WIN32)
	set(TERRAENGINE_HOST_OS "Windows")
	set(TERRAENGINE_DOTNET_RID "win-x64")
	set(TERRAENGINE_DOTNET_FALLBACK_OS "win")
	set(TERRAENGINE_DOTNET_TARGET_OS "win")
	set(TERRAENGINE_EXECUTABLE_SUFFIX ".exe")
	set(TERRAENGINE_SHARED_LIBRARY_SUFFIX ".dll")
elseif(UNIX AND NOT APPLE)
	set(TERRAENGINE_HOST_OS "Linux")
	set(TERRAENGINE_DOTNET_RID "linux-x64")
	set(TERRAENGINE_DOTNET_FALLBACK_OS "linux")
	set(TERRAENGINE_DOTNET_TARGET_OS "linux")
	set(TERRAENGINE_EXECUTABLE_SUFFIX "")
	set(TERRAENGINE_SHARED_LIBRARY_SUFFIX ".so")
else()
	message(FATAL_ERROR "TerraEngine currently supports Windows and Linux hosts only.")
endif()

set(TERRAENGINE_MANAGED_RUNTIME_LIBRARY "Assembly-CSharp${TERRAENGINE_SHARED_LIBRARY_SUFFIX}")
set(TERRAENGINE_ENGINE_RUNTIME_LIBRARY "$<TARGET_FILE_NAME:TerraEngine-Engine>")
set(TERRAENGINE_LOCAL_RPATH "\$ORIGIN")

string(REPLACE "\\" "/" TERRAENGINE_NORMALIZED_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
# Configure-time guard: select platform or option behavior before generating build files.
if(TERRAENGINE_NORMALIZED_INSTALL_PREFIX MATCHES [[/External/[^/]+/install/?$]])
	# Older build trees inherited a dependency-specific install prefix. Reset it
	# so regenerated caches no longer carry obsolete third-party paths forward.
	set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install prefix" FORCE)
endif()

include(ProcessorCount)
ProcessorCount(TERRAENGINE_CPU_COUNT)
# Configure-time guard: select platform or option behavior before generating build files.
if(TERRAENGINE_CPU_COUNT GREATER 0 AND NOT DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
	# Default to using all detected CPU cores for builds so compilation scales with
	# available hardware without requiring callers to remember -j/--parallel
	# arguments manually. Especially useful for those who open the project with Visual Studio
	set(CMAKE_BUILD_PARALLEL_LEVEL ${TERRAENGINE_CPU_COUNT} CACHE STRING "Default parallel build jobs" FORCE)
endif()

set(ENGINE_SOURCES
	Engine/Core/MainEngine.cpp
	Engine/Core/FatalErrorReport.cpp
	Engine/Core/Timer.cpp
	Engine/Core/Scheduling/ComponentAccess.cpp
	Engine/Core/Scheduling/EcsQueryTaskGraph.cpp
	Engine/Core/Scheduling/FrameWorkload.cpp
	Engine/Core/Scheduling/JobGraph.cpp
	Engine/Core/Scheduling/SystemScheduler.cpp
	Engine/Core/Scheduling/WorkerSafety.cpp
	Engine/Core/Logger/Logger.cpp
	Engine/Core/Object/Component.cpp
	Engine/Core/Object/ActiveMembership.cpp
	Engine/Core/Object/Object.cpp
	Engine/Core/Object/ObjectManager.cpp
	Engine/Core/Object/ComponentRegistry.cpp
	Engine/Resource/ResourceManager.cpp
	Engine/Resource/MaterialRuntimeContract.cpp
	Engine/Resource/ResourceResidency.cpp
	Engine/Resource/ResourceStreaming.cpp
	Engine/Resource/ResourceManagerMaterial.cpp
	Engine/Resource/ResourceManagerMesh.cpp
	Engine/Resource/ResourceManagerShader.cpp
	Engine/Resource/ResourceManagerShaderCollection.cpp
	Engine/Resource/ResourceManagerTexture.cpp
	Engine/Runtime/BootSummary.cpp
	Engine/Runtime/ExportStartupCoordinator.cpp
	Engine/Scene/SceneGraphRuntime.cpp
	Engine/Scene/HierarchySnapshot.cpp
	Engine/Scene/RuntimeLayout.cpp
	Engine/Scene/PrefabComposition.cpp
	Engine/Scene/SceneMutationQueue.cpp
	Engine/Scene/TransactionalMutation.cpp
	Engine/Scene/SceneManager.cpp
	Engine/Scene/Serializer.cpp
	Engine/Scene/SpatialIndex.cpp
	Engine/Input/InputSystem.cpp
	Engine/Input/GLFW/GlfwInputBackend.cpp
	Engine/Utilities/PlatformUtils.cpp
	Engine/Window/WindowSystem.cpp
	Engine/Window/GLFW/GlfwWindowBackend.cpp
	Engine/ThreadPool/ThreadPool.cpp
	Engine/Profiling/Profiler.cpp
	Engine/Profiling/ProfilerCapture.cpp
	Engine/Renderer/RenderCaptureEncoding.cpp
	Engine/Renderer/RendererRecovery.cpp
	Engine/Renderer/RendererRecoveryPolicy.cpp
	Engine/Renderer/RenderSystem.cpp
	Engine/Renderer/Graph/RenderDiagnostics.cpp
	Engine/Renderer/Graph/RenderFrameBreakdown.cpp
	Engine/Renderer/Graph/RenderFrameSnapshot.cpp
	Engine/Renderer/Graph/RenderGraph.cpp
	Engine/Renderer/Graph/RenderSubmissionPlan.cpp
	Engine/Renderer/Graph/RenderWorkerTaskGroup.cpp
	Engine/Renderer/Graph/RenderWorkPacket.cpp
	Engine/Renderer/RHI/Vulkan/VulkanContext.cpp
	Engine/Renderer/RHI/Vulkan/VulkanPresentationLifecycle.cpp
	Engine/Renderer/RHI/Vulkan/VulkanCommandRecording.cpp
	Engine/Renderer/RHI/Vulkan/VulkanDescriptorPlanning.cpp
	Engine/Renderer/RHI/Vulkan/VulkanFrameDiagnostics.cpp
	Engine/Renderer/RHI/Vulkan/VulkanPipelineStateKeys.cpp
	Engine/Renderer/RHI/Vulkan/VulkanReadbackConversion.cpp
	Engine/Renderer/RHI/Vulkan/VulkanRecordingContext.cpp
	Engine/Renderer/RHI/Vulkan/VulkanRendererWorkCache.cpp
	Engine/Renderer/RHI/Vulkan/VulkanTransitionPlanning.cpp
	Engine/Renderer/RHI/Vulkan/VulkanUploadPlanning.cpp
	Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp
	Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp
	Engine/Renderer/RHI/Vulkan/VulkanHiZ.cpp
	Engine/Renderer/RHI/Vulkan/VulkanVRS.cpp
	Engine/DotNet/DotNetHost.cpp
	Engine/Scripting/ScriptingSystem.cpp
	Engine/Scripting/ScriptGlue.cpp
	Engine/UI/UIStyle.cpp
	Engine/UI/UIElement.cpp
	Engine/UI/UIParser.cpp
	Engine/UI/UILayoutEngine.cpp
	Engine/UI/UIRenderer.cpp
	Engine/UI/UISystem.cpp
	# --- ContentMount (TEPAK reader integration for export builds) ---
	Engine/Runtime/ContentMount/ContentMount.cpp
	Engine/Runtime/ContentMount/archive/PackReader.cpp
	Engine/Runtime/ContentMount/archive/ExtentReader.cpp
	Engine/Runtime/ContentMount/archive/ArchiveSerialization.cpp
	Engine/Runtime/ContentMount/codec/CodecProvider.cpp
	Engine/Runtime/ContentMount/io/PathNormalizer.cpp
	Engine/Runtime/ContentMount/util/WorkerPool.cpp
)

set(ENGINE_HEADERS
	Engine/Core/MainEngine.h
	Engine/Core/FatalErrorReport.h
	Engine/Core/Logger/Logger.h
	Engine/Core/SystemBase.h
	Engine/Core/Timer.h
	Engine/Core/Scheduling/ComponentAccess.h
	Engine/Core/Scheduling/EcsQueryTaskGraph.h
	Engine/Core/Scheduling/FrameWorkload.h
	Engine/Core/Scheduling/JobGraph.h
	Engine/Core/Scheduling/SystemScheduler.h
	Engine/Core/Scheduling/WorkerSafety.h
	Engine/Core/Object/Component.h
	Engine/Core/Object/ActiveMembership.h
	Engine/Core/Object/Object.h
	Engine/Core/Object/ObjectManager.h
	Engine/Components/Transform.h
	Engine/Components/PrefabLinkComponent.h
	Engine/Components/SkyComponent.h
	Engine/Input/InputBackend.h
	Engine/Input/InputCodes.h
	Engine/Input/InputSystem.h
	Engine/Resource/ResourceManager.h
	Engine/Resource/MaterialRuntimeContract.h
	Engine/Resource/ResourceResidency.h
	Engine/Resource/ResourceStreaming.h
	Engine/Resource/ShaderCollection.h
	Engine/Scene/SceneGraphRuntime.h
	Engine/Scene/HierarchySnapshot.h
	Engine/Scene/RuntimeLayout.h
	Engine/Scene/PrefabComposition.h
	Engine/Scene/SceneMutationQueue.h
	Engine/Scene/TransactionalMutation.h
	Engine/Scene/SceneManager.h
	Engine/Components/LightComponent.h
	Engine/Components/UIComponent.h
	Engine/Scene/Serializer.h
	Engine/Scene/SpatialBounds.h
	Engine/Scene/SpatialDiagnostics.h
	Engine/Scene/SpatialIndex.h
	Engine/Scene/SpatialQuery.h
	Engine/Input/GLFW/GlfwInputBackend.h
	Engine/Utilities/PlatformUtils.h
	Engine/Utilities/UUID.h
	Engine/Window/WindowBackend.h
	Engine/Window/WindowConfig.h
	Engine/Window/WindowSystem.h
	Engine/Window/GLFW/GlfwWindowBackend.h
	Engine/ThreadPool/ThreadPool.h
	Engine/Profiling/Profiler.h
	Engine/Profiling/ProfilerCapture.h
	Engine/Profiling/ProfilerScope.h
	Engine/Renderer/RenderCaptureEncoding.h
	Engine/Renderer/RendererRecovery.h
	Engine/Renderer/RendererRecoveryPolicy.h
	Engine/Renderer/RenderSystem.h
	Engine/Renderer/RenderTypes.h
	Engine/Renderer/Graph/RenderDiagnostics.h
	Engine/Renderer/Graph/RenderFrameBreakdown.h
	Engine/Renderer/Graph/RenderFrameSnapshot.h
	Engine/Renderer/Graph/RenderGraph.h
	Engine/Renderer/Graph/RenderSubmissionPlan.h
	Engine/Renderer/Graph/RenderWorkerTaskGroup.h
	Engine/Renderer/Graph/RenderWorkPacket.h
	Engine/Renderer/RHI/RHIDefinitions.h
	Engine/Renderer/RHI/RHIContext.h
	Engine/Renderer/RHI/Vulkan/VulkanContext.h
	Engine/Renderer/RHI/Vulkan/VulkanCommandRecording.h
	Engine/Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h
	Engine/Renderer/RHI/Vulkan/VulkanFrameDiagnostics.h
	Engine/Renderer/RHI/Vulkan/VulkanPipelineStateKeys.h
	Engine/Renderer/RHI/Vulkan/VulkanRecordingContext.h
	Engine/Renderer/RHI/Vulkan/VulkanRendererWorkCache.h
	Engine/Renderer/RHI/Vulkan/VulkanTransitionPlanning.h
	Engine/Renderer/RHI/Vulkan/VulkanUploadPlanning.h
	Engine/Renderer/RHI/Vulkan/VulkanCommonIncludes.h
	Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h
	Engine/Renderer/RHI/Vulkan/VulkanHiZ.h
	Engine/Renderer/RHI/Vulkan/VulkanVRS.h
	Engine/DotNet/DotNetHost.h
	Engine/Scripting/ScriptingSystem.h
	Engine/Components/ScriptComponent.h
	Engine/Scripting/ExportVTable.h
	Engine/UI/UIStyle.h
	Engine/UI/UIElement.h
	Engine/UI/UIParser.h
	Engine/UI/UILayoutEngine.h
	Engine/UI/UIRenderer.h
	Engine/UI/UISystem.h
	Engine/Runtime/ContentMount/ContentMount.h
	Engine/Runtime/BootSummary.h
	Engine/Runtime/ExportStartupCoordinator.h
	Engine/Debug.h
)

set(BASISU_SOURCES
	External/basisu/encoder/basisu_astc_hdr_6x6_enc.cpp
	External/basisu/encoder/basisu_astc_hdr_common.cpp
	External/basisu/encoder/basisu_astc_ldr_common.cpp
	External/basisu/encoder/basisu_astc_ldr_encode.cpp
	External/basisu/encoder/basisu_backend.cpp
	External/basisu/encoder/basisu_basis_file.cpp
	External/basisu/encoder/basisu_bc7enc.cpp
	External/basisu/encoder/basisu_comp.cpp
	External/basisu/encoder/basisu_enc.cpp
	External/basisu/encoder/basisu_etc.cpp
	External/basisu/encoder/basisu_frontend.cpp
	External/basisu/encoder/basisu_gpu_texture.cpp
	External/basisu/encoder/basisu_kernels_sse.cpp
	External/basisu/encoder/basisu_opencl.cpp
	External/basisu/encoder/basisu_pvrtc1_4.cpp
	External/basisu/encoder/basisu_resampler.cpp
	External/basisu/encoder/basisu_resample_filters.cpp
	External/basisu/encoder/basisu_ssim.cpp
	External/basisu/encoder/basisu_uastc_enc.cpp
	External/basisu/encoder/basisu_uastc_hdr_4x4_enc.cpp
	External/basisu/encoder/jpgd.cpp
	External/basisu/encoder/pvpngreader.cpp
	External/basisu/encoder/3rdparty/android_astc_decomp.cpp
	External/basisu/encoder/3rdparty/tinyexr.cpp
	External/basisu/transcoder/basisu_transcoder.cpp
	External/basisu/zstd/zstd.c
)

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_library(basisu STATIC ${BASISU_SOURCES})
target_include_directories(basisu PUBLIC
	External/basisu/encoder
	External/basisu/transcoder
	External/basisu/zstd
	External/basisu/encoder/3rdparty
)
# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_compile_definitions(basisu PUBLIC BASISU_SUPPORT_ENCODING=1 BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=0)
# Configure-time guard: select platform or option behavior before generating build files.
if(MSVC)
	# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
	target_compile_options(basisu PRIVATE /W0 /FS)
endif()

set(LAUNCHER_SOURCES
	Launcher/Launch.cpp
)

# --- TEPAK Dependencies (xxhash and zstd) ------------------------------------
# These are used by the TEPAK reader/writer source copies in Engine/Runtime/
# ContentMount/ and Editor/Export/. They now live alongside the other vendored
# engine dependencies under External/ instead of inside the standalone TEPAK/
# project folder.
set(TEPAK_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External")
add_subdirectory("${TEPAK_EXTERNAL_DIR}/xxhash/build/cmake" "${CMAKE_BINARY_DIR}/xxhash")
# TerraEngine's CTest surface should stay limited to project-owned engine and
# editor suites. zstd defaults its upstream tests on when BUILD_TESTING is
# enabled, so turn those off before adding the vendored dependency.
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Disable vendored zstd tests" FORCE)
add_subdirectory("${TEPAK_EXTERNAL_DIR}/zstd/build/cmake" "${CMAKE_BINARY_DIR}/zstd")

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_library(TerraEngine-Engine SHARED
	${ENGINE_SOURCES}
	${ENGINE_HEADERS}
)

# Header-only dependencies stay bundled so texture decoding and OBJ parsing never
# rely on external package managers.
target_include_directories(TerraEngine-Engine
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Engine
		${CMAKE_CURRENT_SOURCE_DIR}/Engine/Core/Object
		${CMAKE_CURRENT_SOURCE_DIR}/External/entt/src
		${CMAKE_CURRENT_SOURCE_DIR}/External/glm
		${CMAKE_CURRENT_SOURCE_DIR}/External/json/single_include
		${CMAKE_CURRENT_SOURCE_DIR}/External/stb
		${CMAKE_CURRENT_SOURCE_DIR}/External/tinyobjloader
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/transcoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/zstd
	PRIVATE
		# Copied TEPAK reader sources use internal relative includes that
		# expect the source root as an include path.
		${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/ContentMount
)

set(BUILD_SHARED_LIBS ON CACHE BOOL "Prefer shared libraries for external dependencies" FORCE)

# --- Yoga --------------------------------------------------------------------
# Build the static core target directly so the engine gets the layout library
# without pulling Yoga's test targets into the main solution.
add_subdirectory(External/yoga/yoga ${CMAKE_BINARY_DIR}/yoga)
set_target_properties(yogacore PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# --- pugixml -----------------------------------------------------------------
# Keep pugixml self-contained and static so UI parsing never depends on an
# external DLL at runtime.
set(TERRAENGINE_BUILD_SHARED_LIBS_SAVED_PUGI ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build pugixml as a static library" FORCE)
set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "Disable pugixml tests" FORCE)
set(PUGIXML_INSTALL OFF CACHE BOOL "Disable pugixml install target" FORCE)
add_subdirectory(External/pugixml)
set(BUILD_SHARED_LIBS ${TERRAENGINE_BUILD_SHARED_LIBS_SAVED_PUGI} CACHE BOOL "Prefer shared libraries for external dependencies" FORCE)
# Configure-time guard: select platform or option behavior before generating build files.
if(TARGET pugixml-static)
	set_target_properties(pugixml-static PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

# --- volk --------------------------------------------------------------------
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT DEFINED VULKAN_HEADERS_INSTALL_DIR)
	set(VULKAN_HEADERS_INSTALL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External/Vulkan-Headers)
endif()
set(VOLK_INSTALL OFF CACHE BOOL "Disable volk install" FORCE)
set(VOLK_STATIC_DEFINES "VK_NO_PROTOTYPES" CACHE STRING "Defines for volk" FORCE)
add_subdirectory(External/volk)

# --- VMA ---------------------------------------------------------------------
set(VMA_STATIC_VULKAN_FUNCTIONS OFF CACHE BOOL "VMA: Use dynamic function pointers" FORCE)
set(VMA_DYNAMIC_VULKAN_FUNCTIONS ON CACHE BOOL "VMA: Fetch function pointers" FORCE)
add_subdirectory(External/VMA)

# --- GLFW --------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Disable GLFW docs" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW examples" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW tests" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "Disable GLFW install target" FORCE)
if(UNIX AND NOT APPLE)
	# Linux needs one native window backend for editor and launcher smoke runs.
	# X11 is the broad default; Wayland can be enabled explicitly by users with
	# the required compositor and development headers.
	option(TERRAENGINE_LINUX_ENABLE_WAYLAND "Enable GLFW Wayland backend on Linux" OFF)
	set(GLFW_BUILD_X11 ON CACHE BOOL "Enable GLFW X11 backend on Linux" FORCE)
	set(GLFW_BUILD_WAYLAND ${TERRAENGINE_LINUX_ENABLE_WAYLAND} CACHE BOOL "Enable GLFW Wayland backend on Linux" FORCE)
else()
	set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Disable GLFW Wayland backend" FORCE)
	set(GLFW_BUILD_X11 OFF CACHE BOOL "Disable GLFW X11 backend" FORCE)
endif()
set(GLFW_BUILD_SHARED_LIBS ON CACHE BOOL "Build GLFW as a shared library" FORCE)
set(GLFW_BUILD_DLL ON CACHE BOOL "Enable GLFW DLL export" FORCE)

add_subdirectory(External/glfw)

# --- Vulkan Headers ----------------------------------------------------------
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "Disable Vulkan-Headers tests" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "Disable Vulkan-Headers install target" FORCE)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "Disable Vulkan C++20 module" FORCE)

add_subdirectory(External/Vulkan-Headers)

# --- meshoptimizer -----------------------------------------------------------
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "Disable meshoptimizer demo" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "Disable meshoptimizer gltfpack tool" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "Disable meshoptimizer install target" FORCE)
add_subdirectory(External/meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# --- assimp ------------------------------------------------------------------
# Build Assimp as a static, import-only dependency so the mesh compiler gains
# broad format support without adding a system dependency or extra runtime DLLs.
set(TERRAENGINE_BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Prefer shared libraries for external dependencies" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "Disable Assimp exporters" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "Disable Assimp tests" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "Disable Assimp tools" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "Disable Assimp samples" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "Disable Assimp docs" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "Allow upstream Assimp warnings" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "Disable Assimp install target" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "Build Assimp's bundled zlib" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "Build only the importers TerraEngine uses" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "Enable Assimp OBJ importer" FORCE)
# FBX package import reuses the existing bundled Assimp importer; no External/
# dependency change is required for this feature.
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "Enable Assimp FBX importer" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "Enable Assimp glTF importer" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE BOOL "Enable Assimp COLLADA importer" FORCE)
set(ASSIMP_BUILD_PLY_IMPORTER ON CACHE BOOL "Enable Assimp PLY importer" FORCE)
set(ASSIMP_BUILD_STL_IMPORTER ON CACHE BOOL "Enable Assimp STL importer" FORCE)
add_subdirectory(External/assimp)
set(BUILD_SHARED_LIBS ${TERRAENGINE_BUILD_SHARED_LIBS_SAVED} CACHE BOOL "Prefer shared libraries for external dependencies" FORCE)
set_target_properties(assimp PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# --- fastgltf ---------------------------------------------------------------
# Terra routes glTF/GLB package imports through fastgltf so the editor can read
# package-level mesh, material, texture, and embedded-data relationships instead
# of flattening glTF through the legacy Assimp mesh-only path.
set(SIMDJSON_INSTALL OFF CACHE BOOL "Disable simdjson install target" FORCE)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "Disable simdjson developer targets" FORCE)
set(SIMDJSON_SINGLEHEADER ON CACHE BOOL "Use vendored simdjson single header output mode" FORCE)
add_subdirectory(External/simdjson)
set_target_properties(simdjson PROPERTIES CXX_SCAN_FOR_MODULES OFF)

set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "Disable fastgltf tests" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "Disable fastgltf examples" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "Disable fastgltf docs" FORCE)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "Disable fastgltf install target" FORCE)
set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "Compile fastgltf as C++20" FORCE)
add_subdirectory(External/fastgltf)
set_target_properties(fastgltf PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# --- mikktspace --------------------------------------------------------------
# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_library(mikktspace STATIC External/mikktspace/mikktspace.c External/mikktspace/mikktspace.h)
target_include_directories(mikktspace PUBLIC External/mikktspace)
if(MSVC)
	# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
	target_compile_options(mikktspace PRIVATE /W0) # Disable warnings for external C code
else()
	target_compile_options(mikktspace PRIVATE -w)
endif()

find_package(Threads REQUIRED)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(TerraEngine-Engine
	PUBLIC
		Threads::Threads
		glfw
		Vulkan::Headers
		volk
		GPUOpen::VulkanMemoryAllocator
		nethost
		meshoptimizer
		basisu
		yogacore
		pugixml::pugixml
	PRIVATE
		# TEPAK reader dependencies (zstd for decompression, xxhash for hashing)
		libzstd_static
		xxhash
)

# Configure-time guard: select platform or option behavior before generating build files.
if(WIN32)
	# PlatformUtils uses ShellExecuteW/SHGetKnownFolderPath/MiniDumpWriteDump so the engine must
	# link the corresponding Windows SDK libraries explicitly.
	# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
	target_link_libraries(TerraEngine-Engine PRIVATE shell32 ole32 dbghelp)
endif()

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_compile_definitions(TerraEngine-Engine
	PUBLIC
		TERRAENGINE_SHARED
	PRIVATE
		GLFW_DLL
		TERRAENGINE_ENGINE_BUILD
		BASISD_SUPPORT_KTX2=0
		BASISD_SUPPORT_KTX2_ZSTD=0
		# Required by copied TEPAK sources to access xxhash/zstd internal APIs
		XXH_STATIC_LINKING_ONLY
		ZSTD_STATIC_LINKING_ONLY
)

set_target_properties(TerraEngine-Engine PROPERTIES
	OUTPUT_NAME "TerraEngine"
	DEBUG_POSTFIX ""
	BUILD_RPATH "${TERRAENGINE_LOCAL_RPATH}"
	INSTALL_RPATH "${TERRAENGINE_LOCAL_RPATH}"
)

# Configure-time guard: select platform or option behavior before generating build files.
if(WIN32)
	# Ensure Windows generates an import library even if individual symbols are
	# not manually decorated; this pairs with the TERRAENGINE_API macro so the
	# launcher can link against the DLL reliably.
	set_target_properties(TerraEngine-Engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
	target_compile_options(TerraEngine-Engine PRIVATE /FS /bigobj)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})
source_group("External/basisu" FILES ${BASISU_SOURCES})

# --- Managed Source ----------------------------------------------------------
# The following files are managed C# source files. CMake will generate the
# .sln and .csproj files for these during the build process if configured.

set(DOTNET_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/External/dotnet/runtime/src/native/corehost)

# Configure configure.h required by nethost/pal.h
set(CLI_CMAKE_COMMIT_HASH "unknown")
set(CLI_CMAKE_FALLBACK_OS "${TERRAENGINE_DOTNET_FALLBACK_OS}")
set(CLI_CMAKE_PKG_RID "${TERRAENGINE_DOTNET_RID}")
set(CLR_CMAKE_TARGET_OS "${TERRAENGINE_DOTNET_TARGET_OS}")
set(CLR_CMAKE_TARGET_ARCH "x64")
configure_file(
    ${DOTNET_RUNTIME_ROOT}/configure.h.in
    ${CMAKE_BINARY_DIR}/External/dotnet/runtime/src/native/corehost/configure.h
)

# The Unix PAL source includes hostmisc/config.h for small libc feature
# switches. Linux dirent entries expose d_type on supported filesystems, and
# getauxval is available from glibc, so nethost can use the same fast paths as
# the upstream runtime build.
set(HAVE_DIRENT_D_TYPE 1)
set(HAVE_GETAUXVAL 1)
configure_file(
	${DOTNET_RUNTIME_ROOT}/hostmisc/config.h.in
	${CMAKE_BINARY_DIR}/External/dotnet/runtime/src/native/corehost/config.h
)

# Generate runtime_version.h required by nethost/utils.h
file(WRITE ${CMAKE_BINARY_DIR}/External/dotnet/runtime/src/native/corehost/runtime_version.h
"#define RuntimeProductVersion \"10.0.1\"
#define RuntimeBuildVersion \"10.0.1\"
")

# Generate _version.h required by nethost/utils.cpp (on Windows)
file(WRITE ${CMAKE_BINARY_DIR}/External/dotnet/runtime/src/native/corehost/_version.h
"#define VER_PRODUCTVERSION_STR \"10.0.1\"
#define VER_FILEVERSION_STR \"10.0.1.0\"
#define VER_PRODUCTVERSION 10,0,1,0
#define VER_FILEVERSION 10,0,1,0
")

# Generate _version.c required by nethost/utils.cpp on Unix. The upstream
# source embeds this static version string directly, so keep the local file in
# the generated nethost include directory instead of editing vendored sources.
file(WRITE ${CMAKE_BINARY_DIR}/External/dotnet/runtime/src/native/corehost/_version.c
"static char sccsid[] __attribute__((used)) = \"@(#)Version 10.0.1\";
")

set(NETHOST_SOURCES
    ${DOTNET_RUNTIME_ROOT}/nethost/nethost.cpp
    ${DOTNET_RUNTIME_ROOT}/hostmisc/trace.cpp
    ${DOTNET_RUNTIME_ROOT}/hostmisc/utils.cpp
    ${DOTNET_RUNTIME_ROOT}/fxr_resolver.cpp
    ${DOTNET_RUNTIME_ROOT}/fxr/fx_ver.cpp
)

# Configure-time guard: select platform or option behavior before generating build files.
if(WIN32)
    list(APPEND NETHOST_SOURCES ${DOTNET_RUNTIME_ROOT}/hostmisc/pal.windows.cpp)
    list(APPEND NETHOST_SOURCES ${DOTNET_RUNTIME_ROOT}/hostmisc/longfile.windows.cpp)
else()
    list(APPEND NETHOST_SOURCES ${DOTNET_RUNTIME_ROOT}/hostmisc/pal.unix.cpp)
endif()

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_library(nethost STATIC ${NETHOST_SOURCES})
set_target_properties(nethost PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nethost PUBLIC
    ${DOTNET_RUNTIME_ROOT}
    ${DOTNET_RUNTIME_ROOT}/hostmisc
    ${DOTNET_RUNTIME_ROOT}/nethost
    ${DOTNET_RUNTIME_ROOT}/fxr
    ${DOTNET_RUNTIME_ROOT}/..
    ${CMAKE_BINARY_DIR}/External/dotnet/runtime/src/native/corehost
)
# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_compile_definitions(nethost PUBLIC
    NETHOST_USE_AS_STATIC
)
# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_compile_definitions(nethost PRIVATE
    FEATURE_LIBHOST
)

# Configure-time guard: select platform or option behavior before generating build files.
if(WIN32)
    target_compile_definitions(nethost PRIVATE TARGET_WINDOWS TARGET_AMD64)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(nethost PRIVATE TARGET_LINUX TARGET_AMD64)
    target_link_libraries(nethost PUBLIC ${CMAKE_DL_LIBS})
endif()

set(MANAGED_CORE_SOURCES
	Engine/Managed/Source/Application.cs
	Engine/Managed/Source/InternalCalls.cs
	Engine/Managed/Source/Log.cs
	Engine/Managed/Source/Object.cs
	Engine/Managed/Source/Component.cs
	Engine/Managed/Source/Transform.cs
	Engine/Managed/Source/Camera.cs
	Engine/Managed/Source/Light.cs
	Engine/Managed/Source/MeshRenderer.cs
	Engine/Managed/Source/Math.cs
	Engine/Managed/Source/Scene.cs
	Engine/Managed/Source/Script.cs
	Engine/Managed/Source/Time.cs
	Engine/Managed/Source/Graphics.cs
	Engine/Managed/Source/UI.cs
	Engine/Managed/Source/SerializationAttributes.cs
	Engine/Managed/Source/ScriptReflection.cs
	Engine/Managed/Source/Export/ExportVTable.cs
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Managed PREFIX "Engine/Managed" FILES ${MANAGED_CORE_SOURCES})

# Custom target to build the C# project
# We use 'dotnet build' directly to avoid CMake C# generator limitations (VS only)
set(MANAGED_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Build/Binaries/Managed/$<CONFIG>")
set(EXPORT_GENERATOR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Build/Intermediate/Managed/TerraEngine.Export.Generator/$<CONFIG>")
add_custom_target(TerraEngine.Managed.Output
	COMMAND ${CMAKE_COMMAND} -E rm -rf "${MANAGED_OUTPUT_DIR}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${MANAGED_OUTPUT_DIR}"
	COMMENT "Preparing managed publish output directory"
)
add_custom_target(TerraEngine ALL
    COMMAND dotnet publish "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Managed/TerraEngine.csproj" -c $<CONFIG> -r ${TERRAENGINE_DOTNET_RID} --self-contained true -o "${MANAGED_OUTPUT_DIR}"
    COMMENT "Publishing Managed Assembly: TerraEngine (Self-Contained)"
    SOURCES ${MANAGED_CORE_SOURCES}
)
add_dependencies(TerraEngine TerraEngine.Managed.Output)

# Build the editor-owned Roslyn source generator that gameplay projects compile with.
# The runtime never loads this DLL directly. Instead, the editor stages it into each
# script workspace so Roslyn can emit the export bootstrap code during project builds.
add_custom_target(TerraEngine.Export.Generator ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory "${MANAGED_OUTPUT_DIR}/Analyzers"
	# Send the analyzer output to a CMake-owned directory so Debug, Release, and
	# platform-qualified MSBuild layouts all stage the same DLL path.
	COMMAND dotnet build "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Managed/TerraEngine.Export.Generator/TerraEngine.Export.Generator.csproj" -c $<CONFIG> -nologo -p:OutDir="${EXPORT_GENERATOR_OUTPUT_DIR}/"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${EXPORT_GENERATOR_OUTPUT_DIR}/TerraEngine.Export.Generator.dll"
		"${MANAGED_OUTPUT_DIR}/Analyzers/TerraEngine.Export.Generator.dll"
	COMMENT "Building Managed Analyzer: TerraEngine.Export.Generator"
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/Editor/Managed/TerraEngine.Export.Generator/ScriptRegistryGenerator.cs"
		"${CMAKE_CURRENT_SOURCE_DIR}/Editor/Managed/TerraEngine.Export.Generator/TerraEngine.Export.Generator.csproj"
)
add_dependencies(TerraEngine.Export.Generator TerraEngine.Managed.Output)

set(MANAGED_EDITOR_SOURCES
	Editor/Managed/TerraEngine.Editor/Source/EditorApplication.cs
)

add_custom_target(TerraEngine.Editor.Managed ALL
	# The vendored Hexa.NET.ImGui project is multi-targeted. Serializing MSBuild
	# keeps .NET 10 Linux publishes deterministic instead of failing during the
	# parallel restore graph walk with no emitted compiler error.
	COMMAND dotnet publish "${CMAKE_CURRENT_SOURCE_DIR}/Editor/Managed/TerraEngine.Editor/TerraEngine.Editor.csproj" -c $<CONFIG> -r ${TERRAENGINE_DOTNET_RID} --self-contained true -o "${MANAGED_OUTPUT_DIR}" -m:1
	COMMENT "Publishing Managed Assembly: TerraEngine.Editor (Self-Contained)"
	SOURCES ${MANAGED_EDITOR_SOURCES}
)
add_dependencies(TerraEngine.Editor.Managed TerraEngine.Managed.Output)
add_dependencies(TerraEngine.Editor.Managed TerraEngine)
add_dependencies(TerraEngine.Editor.Managed TerraEngine.Export.Generator)

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_executable(TerraEngine-Launcher
    ${LAUNCHER_SOURCES}
)
set_target_properties(TerraEngine-Launcher PROPERTIES
	BUILD_RPATH "${TERRAENGINE_LOCAL_RPATH}"
	INSTALL_RPATH "${TERRAENGINE_LOCAL_RPATH}"
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(TerraEngine-Launcher
    PRIVATE
        TerraEngine-Engine
)

add_dependencies(TerraEngine-Launcher
	TerraEngineMeshCompiler
	TerraEngineTextureCompiler
	TerraEngineModelPackageCompiler
	TerraEngineShaderCompiler
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Launcher PREFIX "TerraEngine-Launcher" FILES ${LAUNCHER_SOURCES})

# --- Editor Tools ------------------------------------------------------------
# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_executable(TerraEngineMeshCompiler
	Editor/TerraEngineMeshCompiler.cpp
	Editor/AssetCompilers/MeshCompiler.cpp
)

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_executable(TerraEngineTextureCompiler
	Editor/TerraEngineTextureCompiler.cpp
	Editor/AssetCompilers/TextureCompiler.cpp
)

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_executable(TerraEngineModelPackageCompiler
	Editor/TerraEngineModelPackageCompiler.cpp
	Editor/AssetCompilers/ModelPackageImportTypes.cpp
	Editor/AssetCompilers/ModelPackageImporter.cpp
	Editor/AssetCompilers/FbxPackageImporter.cpp
	Editor/AssetCompilers/GltfImportTypes.cpp
	Editor/AssetCompilers/GltfPackageImporter.cpp
	Editor/AssetCompilers/MaterialAssetWriter.cpp
	Editor/AssetCompilers/MeshCompiler.cpp
	Editor/AssetCompilers/TextureCompiler.cpp
	Editor/TerraEngine-Editor/Assets/AssetImportResult.cpp
)

# Target boundary: this declaration defines one buildable unit and its install/runtime behavior.
add_executable(TerraEngineShaderCompiler
	Editor/TerraEngineShaderCompiler.cpp
	Editor/AssetCompilers/ShaderCompiler.cpp
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_include_directories(TerraEngineMeshCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/AssetCompilers
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(TerraEngineMeshCompiler
	PRIVATE
		meshoptimizer
		assimp::assimp
		mikktspace
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_include_directories(TerraEngineTextureCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/AssetCompilers
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/encoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/transcoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/zstd
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(TerraEngineTextureCompiler
	PRIVATE
		basisu
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_include_directories(TerraEngineModelPackageCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Engine
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/AssetCompilers
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets
		${CMAKE_CURRENT_SOURCE_DIR}/External/json/single_include
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/encoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/transcoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/zstd
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(TerraEngineModelPackageCompiler
	PRIVATE
		assimp::assimp
		fastgltf::fastgltf
		meshoptimizer
		mikktspace
		basisu
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_include_directories(TerraEngineShaderCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/AssetCompilers
)

# --- ImGui -------------------------------------------------------------------
set(IMGUI_STATIC "no" CACHE STRING "Build cimgui shared" FORCE)
set(IMGUI_WCHAR32 OFF CACHE BOOL "Use 32-bit WCHAR" FORCE)
set(CIMGUI_VARGS0 1 CACHE STRING "CIMGUI VARGS0" FORCE)

add_subdirectory(External/cimgui)

# Ensure cimgui exports C++ ImGui symbols so TerraEngine-Editor can use them.
# Also clear postfixes so managed wrappers can find the runtime library by its expected name.
set_target_properties(cimgui PROPERTIES
	DEBUG_POSTFIX ""
	PREFIX ""
	BUILD_RPATH "${TERRAENGINE_LOCAL_RPATH}"
	INSTALL_RPATH "${TERRAENGINE_LOCAL_RPATH}"
)

# Resolve 'inconsistent dll linkage' properly by compiling backends and ImGuizmo
# into the cimgui.dll itself instead of the Editor.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External/cimgui/imgui)

target_sources(cimgui PRIVATE
	External/ImGuizmo/ImGuizmo.cpp
	${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
	${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_include_directories(cimgui PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	External/ImGuizmo
	${IMGUI_DIR}/backends
)

if(WIN32)
	# Windows keeps explicit import/export declarations so the editor and
	# managed wrapper consume the same DLL ABI as before.
	target_compile_definitions(cimgui
		PRIVATE
			"IMGUI_API=__declspec(dllexport)"
			"IMGUI_IMPL_API=extern \"C\" __declspec(dllexport)"
			IMGUI_IMPL_VULKAN_NO_PROTOTYPES
	)
	target_compile_definitions(cimgui INTERFACE
			"IMGUI_API=__declspec(dllimport)"
			"IMGUI_IMPL_API=extern \"C\" __declspec(dllimport)"
	)
else()
	# GCC/Clang do not understand __declspec; default visibility is enough for
	# the Linux shared object built from the vendored cimgui sources.
	target_compile_definitions(cimgui
		PRIVATE
			"IMGUI_API="
			"IMGUI_IMPL_API=extern \"C\""
			IMGUI_IMPL_VULKAN_NO_PROTOTYPES
	)
	target_compile_definitions(cimgui INTERFACE
			"IMGUI_API="
			"IMGUI_IMPL_API=extern \"C\""
	)
endif()

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(cimgui glfw Vulkan::Headers)
if(UNIX AND NOT APPLE)
	# The vendored ImGui GLFW backend calls X11 functions through GLFW native
	# access on Linux, so cimgui must link libX11 explicitly for editor links.
	find_package(X11 REQUIRED)
	target_link_libraries(cimgui X11::X11)
endif()

set_target_properties(glfw PROPERTIES DEBUG_POSTFIX "")

# --- Editor ------------------------------------------------------------------
set(IMGUI_FILE_DIALOG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External/ImGuiFileDialog)

# The editor, headless command, and editor tests share exactly one export
# implementation. Keeping filesystem publication and validation in this
# UI-independent library prevents entry-point drift.
add_library(TerraEngine-ExportSupport STATIC
	Editor/AssetCompilers/ModelPackageImportTypes.cpp
	Editor/TerraEngine-Editor/Assets/AssetImportResult.cpp
	Editor/Export/BuildProfile.cpp
	Editor/Export/ExportEvidence.cpp
	Editor/Export/ExportOutputTransaction.cpp
	Editor/Export/ExportPackageValidator.cpp
	Editor/Export/ExportPipeline.cpp
	Editor/Export/ExportRuntimeLayout.cpp
	Editor/Export/ExportScriptBindingRewriter.cpp
	Editor/Export/ExportTypes.cpp
	Editor/Export/ExportValidator.cpp
	Editor/Export/TepakPackager.cpp
	Editor/Export/archive/ArchiveSerialization.cpp
	Editor/Export/archive/ExtentReader.cpp
	Editor/Export/archive/PackagingRequest.cpp
	Editor/Export/archive/PackPlanner.cpp
	Editor/Export/archive/PackReader.cpp
	Editor/Export/archive/PackWriter.cpp
	Editor/Export/codec/CodecProvider.cpp
	Editor/Export/io/PathNormalizer.cpp
	Editor/Export/util/WorkerPool.cpp
	Editor/Export/validation/PackValidator.cpp
)
target_include_directories(TerraEngine-ExportSupport
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/AssetCompilers
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/Export
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets
)
target_compile_features(TerraEngine-ExportSupport PUBLIC cxx_std_20)
target_compile_definitions(TerraEngine-ExportSupport
	PRIVATE
		XXH_STATIC_LINKING_ONLY
		ZSTD_STATIC_LINKING_ONLY
)
if(MSVC)
	target_compile_options(TerraEngine-ExportSupport PRIVATE /FS)
endif()
target_link_libraries(TerraEngine-ExportSupport
	PUBLIC
		TerraEngine-Engine
	PRIVATE
		libzstd_static
		xxhash
)

add_executable(TerraEngine-Export
	Tools/HeadlessExport/HeadlessExportCommandLine.cpp
	Tools/HeadlessExport/Main.cpp
)
target_include_directories(TerraEngine-Export PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Tools/HeadlessExport
)
target_compile_features(TerraEngine-Export PRIVATE cxx_std_20)
target_link_libraries(TerraEngine-Export PRIVATE TerraEngine-ExportSupport)
set_target_properties(TerraEngine-Export PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
	BUILD_RPATH "${TERRAENGINE_LOCAL_RPATH}"
	INSTALL_RPATH "${TERRAENGINE_LOCAL_RPATH}"
)
if(MSVC)
	target_compile_options(TerraEngine-Export PRIVATE /FS)
endif()
if(WIN32)
	add_custom_command(TARGET TerraEngine-Export POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:TerraEngine-Export>
			$<TARGET_FILE_DIR:TerraEngine-Export>
		COMMAND_EXPAND_LISTS
	)
endif()
add_dependencies(TerraEngine-Export TerraEngineModelPackageCompiler TerraEngine-Launcher)

add_executable(TerraEngine-Editor
	Editor/TerraEngine-Editor/Core/EditorMain.cpp
	Editor/TerraEngine-Editor/Core/EditorCommandLine.cpp
	Editor/TerraEngine-Editor/Core/EditorSystem.cpp
	Editor/TerraEngine-Editor/Core/EditorStartupRendererConfiguration.cpp
	Editor/TerraEngine-Editor/Core/CheckedImGuiVulkanBridge.cpp
	Editor/TerraEngine-Editor/Core/ViewportTextureRegistry.cpp
	Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp
	Editor/TerraEngine-Editor/Automation/EditorMcpBridge.cpp
	Editor/TerraEngine-Editor/Automation/EditorMcpTransport.cpp
	Editor/TerraEngine-Editor/Subsystems/ProjectHubSubsystem.cpp
	Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp
	Editor/TerraEngine-Editor/Subsystems/PrefabAuthoringSubsystem.cpp
	Editor/TerraEngine-Editor/Subsystems/ScriptCompilationSubsystem.cpp
	Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.cpp
	Editor/TerraEngine-Editor/Project/ProjectManager.cpp
	Editor/TerraEngine-Editor/Project/ScriptWorkspaceService.cpp
	Editor/TerraEngine-Editor/Assets/AssetDatabase.cpp
	Editor/TerraEngine-Editor/Assets/FileWatcher.cpp
	Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp
	Editor/TerraEngine-Editor/Profiling/ProfilerCaptureStore.cpp
	Editor/TerraEngine-Editor/Framework/EditorWindow.cpp
	Editor/TerraEngine-Editor/Windows/HierarchyWindow.cpp
	Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp
	Editor/TerraEngine-Editor/Windows/ProjectBrowserWindow.cpp
	Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp
	Editor/TerraEngine-Editor/Windows/SceneViewOrientationWidget.cpp
	Editor/TerraEngine-Editor/Windows/SceneViewOverlayIcons.cpp
	Editor/TerraEngine-Editor/Windows/SceneViewOverlayMarkers.cpp
	Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.cpp
	Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp
	Editor/TerraEngine-Editor/Windows/BuildProfilesWindow.cpp
	Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp
	Editor/TerraEngine-Editor/Windows/SchedulerDiagnosticsWindow.cpp
	Editor/TerraEngine-Editor/Windows/ResourceResidencyWindow.cpp
	Editor/TerraEngine-Editor/Windows/UIBuilderWindow.cpp
	Editor/TerraEngine-Editor/Windows/ProjectSettingsWindow.cpp
	Editor/TerraEngine-Editor/Windows/PreferencesWindow.cpp
	Editor/AssetCompilers/MeshCompiler.cpp
	Editor/AssetCompilers/TextureCompiler.cpp
	Editor/AssetCompilers/ShaderCompiler.cpp
	Editor/AssetCompilers/ModelPackageImporter.cpp
	Editor/AssetCompilers/FbxPackageImporter.cpp
	Editor/AssetCompilers/GltfImportTypes.cpp
	Editor/AssetCompilers/GltfPackageImporter.cpp
	Editor/AssetCompilers/MaterialAssetWriter.cpp
	${IMGUI_FILE_DIALOG_DIR}/ImGuiFileDialog.cpp
)
set_target_properties(TerraEngine-Editor PROPERTIES
	BUILD_RPATH "${TERRAENGINE_LOCAL_RPATH}"
	INSTALL_RPATH "${TERRAENGINE_LOCAL_RPATH}"
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_include_directories(TerraEngine-Editor
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Core
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Subsystems
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Project
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Assets
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Automation
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Profiling
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/AssetCompilers
		${CMAKE_CURRENT_SOURCE_DIR}/External/ImGuizmo
		${IMGUI_FILE_DIALOG_DIR}
		${IMGUI_DIR}/backends
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/encoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/transcoder
		${CMAKE_CURRENT_SOURCE_DIR}/External/basisu/zstd
		# Export pipeline and copied TEPAK writer sources
		${CMAKE_CURRENT_SOURCE_DIR}/Editor/Export
)

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_compile_definitions(TerraEngine-Editor PRIVATE
	IMGUI_IMPL_VULKAN_NO_PROTOTYPES
	USE_STD_FILESYSTEM
	GLM_ENABLE_EXPERIMENTAL
	BASISU_SUPPORT_ENCODING=1
	# Required by copied TEPAK sources for export asset packaging
	XXH_STATIC_LINKING_ONLY
	ZSTD_STATIC_LINKING_ONLY
)

# Configure-time guard: select platform or option behavior before generating build files.
if(MSVC)
	# The editor target builds a large number of C++ files directly, including
	# vendored sources, so /FS avoids transient Debug PDB write races on Windows.
	# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
	target_compile_options(TerraEngine-Editor PRIVATE /FS)
endif()

# Target setting: keep include paths, compile options, and linked libraries local to the target that needs them.
target_link_libraries(TerraEngine-Editor
	PRIVATE
		TerraEngine-Engine
		TerraEngine-ExportSupport
		cimgui
		glfw
		Vulkan::Headers
		assimp::assimp
		fastgltf::fastgltf
		meshoptimizer
		mikktspace
		basisu
		# TEPAK writer dependencies for export asset packaging
		libzstd_static
		xxhash
)

if(WIN32)
	# Deployment rule: editor-only runtime DLLs, such as fastgltf, must sit beside
	# TerraEngine-Editor.exe in developer builds so launch/test flows do not rely
	# on PATH or globally installed binaries.
	add_custom_command(TARGET TerraEngine-Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_RUNTIME_DLLS:TerraEngine-Editor>
			$<TARGET_FILE_DIR:TerraEngine-Editor>
		COMMAND_EXPAND_LISTS
	)

	target_link_libraries(TerraEngine-Editor PRIVATE ws2_32)
endif()

add_dependencies(TerraEngine-Editor TerraEngineModelPackageCompiler TerraEngineMeshCompiler TerraEngineTextureCompiler TerraEngineShaderCompiler)

if(BUILD_TESTING)
	# Test targets live under one repository-level Tests tree while preserving
	# separate engine/editor executables for focused CTest labels.
	add_subdirectory(Tests/Engine)
	add_subdirectory(Tests/Editor)
endif()

# --- Build Configurations & Optimizations ---
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Enable Interprocedural Optimization (IPO/LTCG) for Release builds if supported by the compiler
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
endif()

# Configure-time guard: select platform or option behavior before generating build files.
if(MSVC)
    # Stronger optimizations for MSVC in Release modes
    add_compile_options(
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/Ot>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/Ox>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/GS->
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/Oi>
    )
    add_link_options(
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/OPT:REF>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/OPT:ICF>
    )
else()
    # Stronger optimizations for GCC/Clang
    add_compile_options(
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O3>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-ffast-math>
    )
endif()

set(ENGINE_INSTALL_DESTINATION ${CMAKE_BINARY_DIR}/Build)

set(DEFAULT_SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/Engine/Shaders/Default")
	# Configure-time guard: select platform or option behavior before generating build files.
	if(WIN32)
		set(TERRAENGINE_DXC_PATH "${CMAKE_SOURCE_DIR}/External/dxc/windows/bin/x64/dxc.exe")
	else()
		set(TERRAENGINE_DXC_PATH "${CMAKE_SOURCE_DIR}/External/dxc/linux/bin/dxc")
	endif()

set(SHADER_INSTALL_SCRIPT "${CMAKE_BINARY_DIR}/CMakeFiles/install_default_shaders_$<CONFIG>.cmake")
file(GENERATE OUTPUT "${SHADER_INSTALL_SCRIPT}" CONTENT "
set(SHADER_COMPILER \"$<TARGET_FILE:TerraEngineShaderCompiler>\")
set(SHADER_SRC_DIR \"${DEFAULT_SHADER_SRC_DIR}\")
set(EDITOR_SHADER_SRC_DIR \"${CMAKE_SOURCE_DIR}/Editor/Shaders\")
set(UI_SHADER_SRC_DIR \"${CMAKE_SOURCE_DIR}/Engine/Shaders/UI\")
set(SKY_SHADER_SRC_DIR \"${CMAKE_SOURCE_DIR}/Engine/Shaders/Sky\")
set(SHADER_INSTALL_ROOT \"${ENGINE_INSTALL_DESTINATION}\")
set(DXC_PATH \"${TERRAENGINE_DXC_PATH}\")

set(out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders\")
set(editor_out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders/Editor\")
set(lighting_out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders/Lighting\")
set(postprocess_out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders/PostProcess\")
set(virtualgeometry_out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders/VirtualGeometry\")
set(ui_out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders/UI\")
set(sky_out_dir \"\${SHADER_INSTALL_ROOT}/Editor/Shaders/Sky\")
set(lighting_src_dir \"${CMAKE_SOURCE_DIR}/Engine/Shaders/Lighting\")
set(postprocess_src_dir \"${CMAKE_SOURCE_DIR}/Engine/Shaders/PostProcess\")
set(virtualgeometry_src_dir \"${CMAKE_SOURCE_DIR}/Engine/Shaders/VirtualGeometry\")
file(MAKE_DIRECTORY \"\${out_dir}\")
file(MAKE_DIRECTORY \"\${editor_out_dir}\")
file(MAKE_DIRECTORY \"\${lighting_out_dir}\")
file(MAKE_DIRECTORY \"\${postprocess_out_dir}\")
file(MAKE_DIRECTORY \"\${virtualgeometry_out_dir}\")
file(MAKE_DIRECTORY \"\${ui_out_dir}\")
file(MAKE_DIRECTORY \"\${sky_out_dir}\")
file(REMOVE
	\"\${out_dir}/Default.task.spv\"
	\"\${out_dir}/Default.mesh.spv\"
	\"\${out_dir}/Default.frag.spv\"
	\"\${lighting_out_dir}/ClusterGrid.comp.tspf\"
	\"\${lighting_out_dir}/ClusterDepthMask.comp.tspf\"
	\"\${lighting_out_dir}/LightCull.comp.tspf\"
	\"\${lighting_out_dir}/ClusterLightCount.comp.tspf\"
	\"\${lighting_out_dir}/ClusterLightPrefix.comp.tspf\"
	\"\${lighting_out_dir}/ClusterLightFill.comp.tspf\"
	\"\${postprocess_out_dir}/HiZGenerate.comp.tspf\"
	\"\${postprocess_out_dir}/VRSGenerate.comp.tspf\"
	\"\${postprocess_out_dir}/GTAO.comp.spv\"
	\"\${postprocess_out_dir}/GTAOBlur.comp.spv\"
	\"\${virtualgeometry_out_dir}/VirtualGeometryPacketCompaction.comp.spv\"
	\"\${ui_out_dir}/UI_Static.vert.spv\"
	\"\${ui_out_dir}/UI_Static.frag.spv\"
	\"\${ui_out_dir}/UI_Instanced.vert.spv\"
	\"\${ui_out_dir}/UI_Instanced.frag.spv\"
	\"\${ui_out_dir}/UI_SDF_Text.vert.spv\"
	\"\${ui_out_dir}/UI_SDF_Text.frag.spv\"
	\"\${ui_out_dir}/UI_Textured.vert.spv\"
	\"\${ui_out_dir}/UI_Textured.frag.spv\"
	\"\${sky_out_dir}/SkyBackground.task.tspf\"
	\"\${sky_out_dir}/SkyBackground.mesh.tspf\"
	\"\${sky_out_dir}/SkyBackground.frag.tspf\"
	\"\${editor_out_dir}/SceneViewGrid.vert.tspf\"
	\"\${editor_out_dir}/SceneViewGrid.frag.tspf\")

execute_process(
	COMMAND \"\${SHADER_COMPILER}\" -i \"\${SHADER_SRC_DIR}/Default.task.hlsl\" -o \"\${out_dir}/Default.task.tspf\" -s ts --dxc \"\${DXC_PATH}\"
	RESULT_VARIABLE compile_ts
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ts EQUAL 0)
	message(FATAL_ERROR \"Failed to compile Default.task.hlsl with TerraEngineShaderCompiler during install\")
endif()

execute_process(
	COMMAND \"\${SHADER_COMPILER}\" -i \"\${SHADER_SRC_DIR}/Default.mesh.hlsl\" -o \"\${out_dir}/Default.mesh.tspf\" -s ms --dxc \"\${DXC_PATH}\"
	RESULT_VARIABLE compile_ms
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ms EQUAL 0)
	message(FATAL_ERROR \"Failed to compile Default.mesh.hlsl with TerraEngineShaderCompiler during install\")
endif()

execute_process(
	COMMAND \"\${SHADER_COMPILER}\" -i \"\${SHADER_SRC_DIR}/Default.frag.hlsl\" -o \"\${out_dir}/Default.frag.tspf\" -s ps --dxc \"\${DXC_PATH}\"
	RESULT_VARIABLE compile_ps
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ps EQUAL 0)
	message(FATAL_ERROR \"Failed to compile Default.frag.hlsl with TerraEngineShaderCompiler during install\")
endif()

execute_process(
	COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${lighting_out_dir}/ClusterGrid.comp.spv\" -spirv \"\${lighting_src_dir}/ClusterGrid.comp.hlsl\"
	RESULT_VARIABLE compile_cluster_grid
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_cluster_grid EQUAL 0)
	message(FATAL_ERROR \"Failed to compile ClusterGrid.comp.hlsl during install\")
endif()

execute_process(
	COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${lighting_out_dir}/ClusterDepthMask.comp.spv\" -spirv \"\${lighting_src_dir}/ClusterDepthMask.comp.hlsl\"
	RESULT_VARIABLE compile_cluster_depth
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_cluster_depth EQUAL 0)
	message(FATAL_ERROR \"Failed to compile ClusterDepthMask.comp.hlsl during install\")
endif()

execute_process(
	COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${lighting_out_dir}/LightCull.comp.spv\" -spirv \"\${lighting_src_dir}/LightCull.comp.hlsl\"
	RESULT_VARIABLE compile_light_cull
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_light_cull EQUAL 0)
	message(FATAL_ERROR \"Failed to compile LightCull.comp.hlsl during install\")
endif()

execute_process(
	COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${lighting_out_dir}/ClusterLightCount.comp.spv\" -spirv \"\${lighting_src_dir}/ClusterLightCount.comp.hlsl\"
	RESULT_VARIABLE compile_cluster_count
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_cluster_count EQUAL 0)
	message(FATAL_ERROR \"Failed to compile ClusterLightCount.comp.hlsl during install\")
endif()

execute_process(
	COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${lighting_out_dir}/ClusterLightPrefix.comp.spv\" -spirv \"\${lighting_src_dir}/ClusterLightPrefix.comp.hlsl\"
	RESULT_VARIABLE compile_cluster_prefix
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_cluster_prefix EQUAL 0)
	message(FATAL_ERROR \"Failed to compile ClusterLightPrefix.hlsl during install\")
endif()

execute_process(
	COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${lighting_out_dir}/ClusterLightFill.comp.spv\" -spirv \"\${lighting_src_dir}/ClusterLightFill.comp.hlsl\"
	RESULT_VARIABLE compile_cluster_fill
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_cluster_fill EQUAL 0)
	message(FATAL_ERROR \"Failed to compile ClusterLightFill.comp.hlsl during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${postprocess_out_dir}/HiZGenerate.comp.spv\" -spirv \"\${postprocess_src_dir}/HiZGenerate.comp.hlsl\"
    RESULT_VARIABLE compile_hiz
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_hiz EQUAL 0)
    message(FATAL_ERROR \"Failed to compile HiZGenerate.comp.hlsl during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${postprocess_out_dir}/VRSGenerate.comp.spv\" -spirv \"\${postprocess_src_dir}/VRSGenerate.comp.hlsl\"
    RESULT_VARIABLE compile_vrs
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_vrs EQUAL 0)
    message(FATAL_ERROR \"Failed to compile VRSGenerate.comp.hlsl during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${postprocess_out_dir}/GTAO.comp.spv\" -spirv \"\${postprocess_src_dir}/GTAO.comp.hlsl\"
    RESULT_VARIABLE compile_gtao
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_gtao EQUAL 0)
    message(FATAL_ERROR \"Failed to compile GTAO.comp.hlsl during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${postprocess_out_dir}/GTAOBlur.comp.spv\" -spirv \"\${postprocess_src_dir}/GTAOBlur.comp.hlsl\"
    RESULT_VARIABLE compile_gtao_blur
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_gtao_blur EQUAL 0)
    message(FATAL_ERROR \"Failed to compile GTAOBlur.comp.hlsl during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T cs_6_0 -E main -Fo \"\${virtualgeometry_out_dir}/VirtualGeometryPacketCompaction.comp.spv\" -spirv \"\${virtualgeometry_src_dir}/VirtualGeometryPacketCompaction.comp.hlsl\"
    RESULT_VARIABLE compile_vg_packet_compaction
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_vg_packet_compaction EQUAL 0)
    message(FATAL_ERROR \"Failed to compile VirtualGeometryPacketCompaction.comp.hlsl during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T vs_6_0 -E VSMain -Fo \"\${ui_out_dir}/UI_Static.vert.spv\" -spirv \"\${UI_SHADER_SRC_DIR}/UI_Static.hlsl\"
    RESULT_VARIABLE compile_ui_static_vs
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ui_static_vs EQUAL 0)
    message(FATAL_ERROR \"Failed to compile UI_Static.hlsl vertex shader during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T ps_6_0 -E PSMain -Fo \"\${ui_out_dir}/UI_Static.frag.spv\" -spirv \"\${UI_SHADER_SRC_DIR}/UI_Static.hlsl\"
    RESULT_VARIABLE compile_ui_static_ps
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ui_static_ps EQUAL 0)
    message(FATAL_ERROR \"Failed to compile UI_Static.hlsl fragment shader during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T vs_6_0 -E VSMain -Fo \"\${ui_out_dir}/UI_Instanced.vert.spv\" -spirv \"\${UI_SHADER_SRC_DIR}/UI_Instanced.hlsl\"
    RESULT_VARIABLE compile_ui_instanced_vs
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ui_instanced_vs EQUAL 0)
    message(FATAL_ERROR \"Failed to compile UI_Instanced.hlsl vertex shader during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T ps_6_0 -E PSMain -Fo \"\${ui_out_dir}/UI_Instanced.frag.spv\" -spirv \"\${UI_SHADER_SRC_DIR}/UI_Instanced.hlsl\"
    RESULT_VARIABLE compile_ui_instanced_ps
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ui_instanced_ps EQUAL 0)
    message(FATAL_ERROR \"Failed to compile UI_Instanced.hlsl fragment shader during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T vs_6_0 -E VSMain -Fo \"\${ui_out_dir}/UI_SDF_Text.vert.spv\" -spirv \"\${UI_SHADER_SRC_DIR}/UI_SDF_Text.hlsl\"
    RESULT_VARIABLE compile_ui_text_vs
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ui_text_vs EQUAL 0)
    message(FATAL_ERROR \"Failed to compile UI_SDF_Text.hlsl vertex shader during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T ps_6_0 -E PSMain -Fo \"\${ui_out_dir}/UI_SDF_Text.frag.spv\" -spirv \"\${UI_SHADER_SRC_DIR}/UI_SDF_Text.hlsl\"
    RESULT_VARIABLE compile_ui_text_ps
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_ui_text_ps EQUAL 0)
    message(FATAL_ERROR \"Failed to compile UI_SDF_Text.hlsl fragment shader during install\")
endif()

execute_process(
	COMMAND \"\${SHADER_COMPILER}\" -i \"\${SKY_SHADER_SRC_DIR}/SkyBackground.task.hlsl\" -o \"\${sky_out_dir}/SkyBackground.task.tspf\" -s ts -e TSMain --dxc \"\${DXC_PATH}\"
	RESULT_VARIABLE compile_sky_ts
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_sky_ts EQUAL 0)
	message(FATAL_ERROR \"Failed to compile SkyBackground.task.hlsl with TerraEngineShaderCompiler during install\")
endif()

execute_process(
	COMMAND \"\${SHADER_COMPILER}\" -i \"\${SKY_SHADER_SRC_DIR}/SkyBackground.mesh.hlsl\" -o \"\${sky_out_dir}/SkyBackground.mesh.tspf\" -s ms -e MSMain --dxc \"\${DXC_PATH}\"
	RESULT_VARIABLE compile_sky_ms
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_sky_ms EQUAL 0)
	message(FATAL_ERROR \"Failed to compile SkyBackground.mesh.hlsl with TerraEngineShaderCompiler during install\")
endif()

execute_process(
	COMMAND \"\${SHADER_COMPILER}\" -i \"\${SKY_SHADER_SRC_DIR}/SkyBackground.frag.hlsl\" -o \"\${sky_out_dir}/SkyBackground.frag.tspf\" -s ps -e PSMain --dxc \"\${DXC_PATH}\"
	RESULT_VARIABLE compile_sky_ps
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_sky_ps EQUAL 0)
	message(FATAL_ERROR \"Failed to compile SkyBackground.frag.hlsl with TerraEngineShaderCompiler during install\")
endif()

# Compile Grid Shaders using raw DXC (bypassing asset compiler for internal usage)
execute_process(
    COMMAND \"\${DXC_PATH}\" -T vs_6_0 -E VSMain -Fo \"\${editor_out_dir}/SceneViewGrid.vert.spv\" -spirv \"\${EDITOR_SHADER_SRC_DIR}/SceneViewGrid.hlsl\"
    RESULT_VARIABLE compile_grid_vs
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_grid_vs EQUAL 0)
    message(FATAL_ERROR \"Failed to compile SceneViewGrid.vert.spv during install\")
endif()

execute_process(
    COMMAND \"\${DXC_PATH}\" -T ps_6_0 -E PSMainWithDepth -Fo \"\${editor_out_dir}/SceneViewGrid.frag.spv\" -spirv \"\${EDITOR_SHADER_SRC_DIR}/SceneViewGrid.hlsl\"
    RESULT_VARIABLE compile_grid_ps
)
# Configure-time guard: select platform or option behavior before generating build files.
if(NOT compile_grid_ps EQUAL 0)
    message(FATAL_ERROR \"Failed to compile SceneViewGrid.frag.spv during install\")
endif()
")
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(SCRIPT "${SHADER_INSTALL_SCRIPT}")
	# Configure-time guard: select platform or option behavior before generating build files.
	if(TARGET install)
		add_dependencies(install TerraEngineShaderCompiler)
	endif()

	# Configure-time guard: select platform or option behavior before generating build files.
	if(WIN32)
		# Keep the Windows install tree focused on runtime files only; the import library
		# stays in the build tree while the DLL is delivered via the runtime destination.
		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(TARGETS TerraEngine-Engine
			RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			INCLUDES DESTINATION ${ENGINE_INSTALL_DESTINATION}/include
		)
	else()
		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(TARGETS TerraEngine-Engine
			RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			ARCHIVE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			INCLUDES DESTINATION ${ENGINE_INSTALL_DESTINATION}/include
		)
	endif()

	if(WIN32)
		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
			DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
			OPTIONAL
		)
	endif()

	# --- Launcher Install ---
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(TARGETS TerraEngine-Launcher
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher
		BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher
	)

	if(WIN32)
		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Launcher>
			DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher
			OPTIONAL
		)
	endif()

	# Install Engine DLLs to Launcher folder so it can run
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES $<TARGET_FILE:TerraEngine-Engine>
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher
	)
	if(WIN32)
		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
			DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher
			OPTIONAL
		)
	endif()

	# --- Editor Install ---
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(TARGETS TerraEngine-Editor
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
		BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)
	# Install the non-interactive exporter beside the editor tools so it can
	# derive Engine, Launcher, shader, font, and managed paths from argv[0].
	install(TARGETS TerraEngine-Export
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
		BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)

	# Install Engine DLLs to Editor folder so it can run
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES $<TARGET_FILE:TerraEngine-Engine>
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)
	if(WIN32)
		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Editor>
			DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
			OPTIONAL
		)

		# Install rule: every code change must keep the install tree reproducible for downstream users.
		install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
			DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
			OPTIONAL
		)
	endif()

	# Install Managed Assemblies to Editor (Self-Contained)
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(DIRECTORY "${MANAGED_OUTPUT_DIR}/"
		DESTINATION "${ENGINE_INSTALL_DESTINATION}/Editor/Managed"
		PATTERN "runtimes" EXCLUDE
	)

	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Editor/TerraEngine-Editor/Project/Templates/"
		DESTINATION "${ENGINE_INSTALL_DESTINATION}/Editor/Project/Templates"
	)

	# Install Engine's export-mode interop sources as templates so the editor
	# can reliably sync them into user projects.
	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Managed/Source/Export/"
		DESTINATION "${ENGINE_INSTALL_DESTINATION}/Editor/Project/Templates/Export"
		FILES_MATCHING PATTERN "*.cs"
	)

	# Install rule: keep virtual-geometry validation notes with the installed
	# editor/runtime tree so clean-checkout QA does not depend on source paths.
	install(FILES
		"${CMAKE_CURRENT_SOURCE_DIR}/Docs/Renderer/VirtualGeometry.md"
		"${CMAKE_CURRENT_SOURCE_DIR}/Docs/TerraEngineMeshFile.md"
		"${CMAKE_CURRENT_SOURCE_DIR}/Docs/Testing.md"
		DESTINATION "${ENGINE_INSTALL_DESTINATION}/Docs"
	)
	install(FILES
		"${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/VirtualGeometry/README.md"
		DESTINATION "${ENGINE_INSTALL_DESTINATION}/Tests/Fixtures/VirtualGeometry"
	)

	# Install cimgui (TerraEngine-ImGui)
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES $<TARGET_FILE:cimgui> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor)
	install(FILES $<TARGET_FILE:cimgui> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor/Managed)
	install(FILES $<TARGET_FILE:cimgui> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher)

	# Ship a bundled fallback UI font so runtime UI text rendering works in both
	# the editor and exported builds without depending on any OS-installed font.
	install(FILES
		${CMAKE_CURRENT_SOURCE_DIR}/External/cimgui/imgui/misc/fonts/Roboto-Medium.ttf
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor/Fonts
	)
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES
		${CMAKE_CURRENT_SOURCE_DIR}/External/cimgui/imgui/misc/fonts/Roboto-Medium.ttf
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher/Fonts
	)

	# Install dependencies (glfw) to both
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(TARGETS glfw
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
		LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}/Engine
	)
	# Copy to Launcher/Editor/Managed
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES $<TARGET_FILE:glfw> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher)
	install(FILES $<TARGET_FILE:glfw> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor)
	install(FILES $<TARGET_FILE:glfw> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor/Managed)
	if(UNIX AND NOT APPLE)
		foreach(TERRAENGINE_GLFW_RUNTIME_DIR Launcher Editor Editor/Managed)
			# Linux binaries need the GLFW SONAME link beside the copied real
			# shared object, otherwise the loader looks for libglfw.so.3 and
			# ignores the libglfw.so.3.4 file.
			install(CODE "file(REMOVE \"${ENGINE_INSTALL_DESTINATION}/${TERRAENGINE_GLFW_RUNTIME_DIR}/libglfw.so.3\")")
			install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink libglfw.so.3.4 \"${ENGINE_INSTALL_DESTINATION}/${TERRAENGINE_GLFW_RUNTIME_DIR}/libglfw.so.3\")")
			install(CODE "file(REMOVE \"${ENGINE_INSTALL_DESTINATION}/${TERRAENGINE_GLFW_RUNTIME_DIR}/libglfw.so\")")
			install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink libglfw.so.3 \"${ENGINE_INSTALL_DESTINATION}/${TERRAENGINE_GLFW_RUNTIME_DIR}/libglfw.so\")")
		endforeach()
	endif()

	if(UNIX AND NOT APPLE)
		# Linux executables resolve local shared objects from their run
		# directory during smoke validation, so copy vendored runtime libraries
		# beside every native consumer that needs them.
		install(FILES $<TARGET_FILE:volk> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Launcher OPTIONAL)
		install(FILES $<TARGET_FILE:volk> DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor OPTIONAL)
	endif()

# Install rule: every code change must keep the install tree reproducible for downstream users.
install(TARGETS TerraEngineMeshCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
)
# Install rule: every code change must keep the install tree reproducible for downstream users.
install(TARGETS TerraEngineTextureCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
)
# Install rule: every code change must keep the install tree reproducible for downstream users.
install(TARGETS TerraEngineModelPackageCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
)
# Install rule: every code change must keep the install tree reproducible for downstream users.
install(TARGETS TerraEngineShaderCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
)

# Configure-time guard: select platform or option behavior before generating build files.
if(WIN32)
	install(PROGRAMS
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/windows/bin/x64/dxc.exe
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/windows/bin/x64/dxcompiler.dll
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/windows/bin/x64/dxil.dll
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)
	# Also need DXC for Launcher? No, Launcher just runs. Editor compiles.
else()
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(PROGRAMS
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/linux/bin/dxc
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)
	# Ship the DXC runtime libraries beside the compiler binary so it can run without system installs.
	# Install rule: every code change must keep the install tree reproducible for downstream users.
	install(FILES
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/linux/lib/libdxcompiler.so
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/linux/lib/libdxil.so
		DESTINATION ${ENGINE_INSTALL_DESTINATION}/Editor
	)
endif()
