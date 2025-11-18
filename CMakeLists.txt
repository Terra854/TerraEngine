cmake_minimum_required(VERSION 3.22.1)
project(TerraEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENGINE_SOURCES
    Engine/Core/MainEngine.cpp
    Engine/Core/Timer.cpp
    Engine/Core/Logger/Logger.cpp
    Engine/Core/Object/Object.cpp
    Engine/Core/Object/ObjectManager.cpp
    Engine/Window/GlfwWindowBackend.cpp
    Engine/Window/WindowSystem.cpp
    Engine/ThreadPool/ThreadPool.cpp
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
    Engine/Window/GlfwWindowBackend.h
    Engine/Window/WindowBackend.h
    Engine/Window/WindowConfig.h
    Engine/Window/WindowSystem.h
    Engine/ThreadPool/ThreadPool.h
    Engine/Debug.h
)

set(LAUNCHER_SOURCES
    Launcher/Launch.cpp
)

add_library(TerraEngine-Engine SHARED
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
)

target_include_directories(TerraEngine-Engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine
        ${CMAKE_CURRENT_SOURCE_DIR}/External/entt/src
        ${CMAKE_CURRENT_SOURCE_DIR}/External/glm
)

set(BUILD_SHARED_LIBS ON CACHE BOOL "Prefer shared libraries for external dependencies" FORCE)

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
)

target_compile_definitions(TerraEngine-Engine
    PRIVATE
        RTTR_DLL
        GLFW_DLL
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})

add_executable(TerraEngine-Launcher
    ${LAUNCHER_SOURCES}
)

target_link_libraries(TerraEngine-Launcher
    PRIVATE
        TerraEngine-Engine
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Launcher PREFIX "TerraEngine-Launcher" FILES ${LAUNCHER_SOURCES})

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

install(TARGETS TerraEngine-Engine
    RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
    LIBRARY DESTINATION ${ENGINE_INSTALL_DESTINATION}
    ARCHIVE DESTINATION ${ENGINE_INSTALL_DESTINATION}
    BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
    INCLUDES DESTINATION ${ENGINE_INSTALL_DESTINATION}/include
)

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
