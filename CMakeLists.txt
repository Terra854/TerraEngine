cmake_minimum_required(VERSION 3.22.1)
project(TerraEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENGINE_SOURCES
    Engine/Core/Launch.cpp
    Engine/Core/MainEngine.cpp
    Engine/Core/Object/Object.cpp
    Engine/Core/Object/ObjectManager.cpp
    Engine/ThreadPool/ThreadPool.cpp
)

set(ENGINE_HEADERS
    Engine/Core/MainEngine.h
    Engine/Core/SystemBase.h
    Engine/Core/Object/Component.h
    Engine/Core/Object/Object.h
    Engine/Core/Object/ObjectManager.h
    Engine/Core/Object/Transform.h
    Engine/ThreadPool/ThreadPool.h
    Engine/Debug.h
)

add_executable(TerraEngine-Engine
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
)

target_include_directories(TerraEngine-Engine
    PRIVATE
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

add_subdirectory(External/glfw)

# --- Vulkan Headers ----------------------------------------------------------
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "Disable Vulkan-Headers tests" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "Disable Vulkan-Headers install target" FORCE)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "Disable Vulkan C++20 module" FORCE)

add_subdirectory(External/Vulkan-Headers)

find_package(Threads REQUIRED)

target_link_libraries(TerraEngine-Engine
    PRIVATE
        Threads::Threads
        RTTR::Core
        glfw
        Vulkan::Headers
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})

set(ENGINE_RUNTIME_DEPENDENCIES_SET TerraEngineEngineRuntimeDeps)
set(ENGINE_INSTALL_DESTINATION ${CMAKE_BINARY_DIR})

install(TARGETS TerraEngine-Engine
    RUNTIME DESTINATION ${ENGINE_INSTALL_DESTINATION}
    BUNDLE DESTINATION ${ENGINE_INSTALL_DESTINATION}
    RUNTIME_DEPENDENCY_SET ${ENGINE_RUNTIME_DEPENDENCIES_SET}
)

install(RUNTIME_DEPENDENCY_SET ${ENGINE_RUNTIME_DEPENDENCIES_SET}
    DESTINATION ${ENGINE_INSTALL_DESTINATION}
    PRE_INCLUDE_REGEXES "^.*$"
    POST_EXCLUDE_REGEXES
        "^api-ms-win-.*$"
        "^ext-ms-.*$"
        ".*/System32/.*"
        ".*/system32/.*"
        ".*/Windows/.*"
        "^/System/.*"
        "^/usr/lib/.*"
        "^/usr/lib64/.*"
        "^/lib/.*"
        "^/lib64/.*"
)
