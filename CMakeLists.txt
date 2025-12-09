cmake_minimum_required(VERSION 3.22.1)
project(TerraEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(ProcessorCount)
ProcessorCount(TERRAENGINE_CPU_COUNT)
if(TERRAENGINE_CPU_COUNT GREATER 0 AND NOT DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
	# Default to using all detected CPU cores for builds so compilation scales with
	# available hardware without requiring callers to remember -j/--parallel
	# arguments manually. Especially useful for those who open the project with Visual Studio
	set(CMAKE_BUILD_PARALLEL_LEVEL ${TERRAENGINE_CPU_COUNT} CACHE STRING "Default parallel build jobs" FORCE)
endif()

set(ENGINE_SOURCES
Engine/Core/MainEngine.cpp
Engine/Core/Timer.cpp
Engine/Core/Logger/Logger.cpp
Engine/Core/Object/Object.cpp
Engine/Core/Object/ObjectManager.cpp
Engine/Resource/ResourceManager.cpp
Engine/Resource/ResourceManagerMaterial.cpp
Engine/Resource/ResourceManagerMesh.cpp
Engine/Resource/ResourceManagerShader.cpp
Engine/Resource/ResourceManagerShaderCollection.cpp
Engine/Resource/ResourceManagerTexture.cpp
Engine/Scene/SceneManager.cpp
Engine/Input/InputSystem.cpp
Engine/Input/GLFW/GlfwInputBackend.cpp
Engine/Window/WindowSystem.cpp
Engine/Window/GLFW/GlfwWindowBackend.cpp
Engine/ThreadPool/ThreadPool.cpp
Engine/Renderer/RenderSystem.cpp
Engine/Renderer/RHI/Vulkan/VulkanContext.cpp
)

set(ENGINE_HEADERS
Engine/Core/MainEngine.h
Engine/Core/Logger/Logger.h
Engine/Core/SystemBase.h
Engine/Core/Timer.h
Engine/Core/Object/Component.h
Engine/Core/Object/Object.h
Engine/Core/Object/ObjectManager.h
Engine/Core/Object/Transform.h
Engine/Input/InputBackend.h
Engine/Input/InputCodes.h
Engine/Input/InputSystem.h
Engine/Resource/ResourceManager.h
Engine/Resource/ShaderCollection.h
Engine/Scene/SceneManager.h
Engine/Input/GLFW/GlfwInputBackend.h
Engine/Utilities/UUID.h
Engine/Window/WindowBackend.h
Engine/Window/WindowConfig.h
Engine/Window/WindowSystem.h
Engine/Window/GLFW/GlfwWindowBackend.h
Engine/ThreadPool/ThreadPool.h
Engine/Renderer/RenderSystem.h
Engine/Renderer/RenderTypes.h
Engine/Renderer/RHI/RHIDefinitions.h
Engine/Renderer/RHI/RHIContext.h
Engine/Renderer/RHI/Vulkan/VulkanContext.h
Engine/Debug.h
)

set(LAUNCHER_SOURCES
	Launcher/Launch.cpp
)

add_library(TerraEngine-Engine SHARED
	${ENGINE_SOURCES}
	${ENGINE_HEADERS}
)

# Header-only dependencies stay bundled so texture decoding and OBJ parsing never
# rely on external package managers.
target_include_directories(TerraEngine-Engine
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Engine
		${CMAKE_CURRENT_SOURCE_DIR}/External/entt/src
		${CMAKE_CURRENT_SOURCE_DIR}/External/glm
		${CMAKE_CURRENT_SOURCE_DIR}/External/json/single_include
		${CMAKE_CURRENT_SOURCE_DIR}/External/stb
		${CMAKE_CURRENT_SOURCE_DIR}/External/tinyobjloader
)

set(BUILD_SHARED_LIBS ON CACHE BOOL "Prefer shared libraries for external dependencies" FORCE)

# --- volk --------------------------------------------------------------------
set(VOLK_INSTALL OFF CACHE BOOL "Disable volk install" FORCE)
set(VOLK_STATIC_DEFINES "VK_NO_PROTOTYPES" CACHE STRING "Defines for volk" FORCE)
add_subdirectory(External/volk)

# --- VMA ---------------------------------------------------------------------
set(VMA_STATIC_VULKAN_FUNCTIONS OFF CACHE BOOL "VMA: Use dynamic function pointers" FORCE)
set(VMA_DYNAMIC_VULKAN_FUNCTIONS ON CACHE BOOL "VMA: Fetch function pointers" FORCE)
add_subdirectory(External/VMA)

# --- RTTR --------------------------------------------------------------------
set(BUILD_RTTR_DYNAMIC ON CACHE BOOL "Build RTTR as a shared library" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "Disable RTTR static library" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "Disable RTTR unit tests" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "Disable RTTR benchmarks" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable RTTR examples" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "Disable RTTR documentation" FORCE)
set(BUILD_INSTALLER OFF CACHE BOOL "Disable RTTR installer" FORCE)
set(BUILD_PACKAGE OFF CACHE BOOL "Disable RTTR package builder" FORCE)

add_subdirectory(External/rttr)

# --- GLFW --------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Disable GLFW docs" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW examples" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW tests" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "Disable GLFW install target" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Disable GLFW Wayland backend" FORCE)
set(GLFW_BUILD_X11 OFF CACHE BOOL "Disable GLFW X11 backend" FORCE)
set(GLFW_BUILD_SHARED_LIBS ON CACHE BOOL "Build GLFW as a shared library" FORCE)
set(GLFW_BUILD_DLL ON CACHE BOOL "Enable GLFW DLL export" FORCE)

add_subdirectory(External/glfw)

# --- Vulkan Headers ----------------------------------------------------------
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "Disable Vulkan-Headers tests" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "Disable Vulkan-Headers install target" FORCE)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "Disable Vulkan C++20 module" FORCE)

add_subdirectory(External/Vulkan-Headers)

find_package(Threads REQUIRED)

target_link_libraries(TerraEngine-Engine
	PUBLIC
		Threads::Threads
		RTTR::Core
		glfw
		Vulkan::Headers
		volk
		GPUOpen::VulkanMemoryAllocator
)

target_compile_definitions(TerraEngine-Engine
	PUBLIC
		TERRAENGINE_SHARED
	PRIVATE
		RTTR_DLL
		GLFW_DLL
		TERRAENGINE_ENGINE_BUILD
)

if(WIN32)
	# Ensure Windows generates an import library even if individual symbols are
	# not manually decorated; this pairs with the TERRAENGINE_API macro so the
	# launcher can link against the DLL reliably.
	set_target_properties(TerraEngine-Engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})

add_executable(TerraEngine-Launcher
    ${LAUNCHER_SOURCES}
)

target_link_libraries(TerraEngine-Launcher
    PRIVATE
        TerraEngine-Engine
)

add_dependencies(TerraEngine-Launcher
	TerraEngineMeshCompiler
	TerraEngineTextureCompiler
	TerraEngineShaderCompiler
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Launcher PREFIX "TerraEngine-Launcher" FILES ${LAUNCHER_SOURCES})

# --- Editor Tools ------------------------------------------------------------
add_executable(TerraEngineMeshCompiler
	Editor/TerraEngineMeshCompiler.cpp
)

add_executable(TerraEngineTextureCompiler
	Editor/TerraEngineTextureCompiler.cpp
)

add_executable(TerraEngineShaderCompiler
	Editor/TerraEngineShaderCompiler.cpp
)

target_include_directories(TerraEngineMeshCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(TerraEngineTextureCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(TerraEngineShaderCompiler
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
)

set(ENGINE_INSTALL_BASE ${CMAKE_BINARY_DIR}/Build)
if(CMAKE_CONFIGURATION_TYPES)
    set(ENGINE_INSTALL_DESTINATION ${ENGINE_INSTALL_BASE}/$<CONFIG>)
else()
    if(CMAKE_BUILD_TYPE)
        set(ENGINE_INSTALL_DESTINATION ${ENGINE_INSTALL_BASE}/${CMAKE_BUILD_TYPE})
    else()
        set(ENGINE_INSTALL_DESTINATION ${ENGINE_INSTALL_BASE}/$<CONFIG>)
    endif()
endif()

	if(WIN32)
		# Keep the Windows install tree focused on runtime files only; the import library
		# stays in the build tree while the DLL is delivered via the runtime destination.
		install(TARGETS TerraEngine-Engine
			RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
			LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}
			BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
			INCLUDES DESTINATION ${ENGINE_INSTALL_DESTINATION}/include
		)
	else()
		install(TARGETS TerraEngine-Engine
			RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
			LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}
			ARCHIVE DESTINATION ${ENGINE_INSTALL_DESTINATION}
			BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
			INCLUDES DESTINATION ${ENGINE_INSTALL_DESTINATION}/include
		)
	endif()

	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
		DESTINATION ${ENGINE_INSTALL_DESTINATION}
		OPTIONAL
	)

	install(TARGETS TerraEngine-Launcher
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
		BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)

	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Launcher>
		DESTINATION ${ENGINE_INSTALL_DESTINATION}
		OPTIONAL
	)

	install(TARGETS glfw
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
		LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)

	install(TARGETS rttr_core
		RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
		LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)

install(TARGETS TerraEngineMeshCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
)
install(TARGETS TerraEngineTextureCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
)
install(TARGETS TerraEngineShaderCompiler
	RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
	BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
)

if(WIN32)
	install(PROGRAMS
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/windows/bin/x64/dxc.exe
		DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)
	install(FILES
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/windows/bin/x64/dxcompiler.dll
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/windows/bin/x64/dxil.dll
		DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)
else()
	install(PROGRAMS
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/linux/bin/dxc
		DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)
	# Ship the DXC runtime libraries beside the compiler binary so it can run without system installs.
	install(FILES
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/linux/lib/libdxcompiler.so
		${CMAKE_CURRENT_SOURCE_DIR}/External/dxc/linux/lib/libdxil.so
		DESTINATION ${ENGINE_INSTALL_DESTINATION}
	)
endif()
