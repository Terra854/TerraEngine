cmake_minimum_required(VERSION 3.16)
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
)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
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

FetchContent_Declare(rttr
    GIT_REPOSITORY https://github.com/rttrorg/rttr.git
    GIT_TAG v0.9.6
)

# --- EnTT --------------------------------------------------------------------
set(ENTT_INCLUDE_HEADERS OFF CACHE BOOL "Skip installing EnTT headers" FORCE)
set(ENTT_INCLUDE_NATVIS OFF CACHE BOOL "Skip adding EnTT natvis files" FORCE)

FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.15.0
)

# --- GLM ---------------------------------------------------------------------
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "Header only usage" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "Disable GLM tests" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "Skip GLM install target" FORCE)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.2
)

# --- GLFW --------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Disable GLFW docs" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW examples" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW tests" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "Disable GLFW install target" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Disable GLFW Wayland backend" FORCE)
set(GLFW_BUILD_X11 OFF CACHE BOOL "Disable GLFW X11 backend" FORCE)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)

# --- Vulkan Headers ----------------------------------------------------------
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.328.1
)

FetchContent_MakeAvailable(rttr entt glm glfw vulkan_headers)

find_package(Threads REQUIRED)

target_link_libraries(TerraEngine-Engine
    PRIVATE
        Threads::Threads
        RTTR::Core
        glfw
        EnTT::EnTT
)

FetchContent_GetProperties(glm)
if(glm_POPULATED)
    target_include_directories(TerraEngine-Engine PRIVATE ${glm_SOURCE_DIR})
endif()

FetchContent_GetProperties(vulkan_headers)
if(vulkan_headers_POPULATED)
    target_include_directories(TerraEngine-Engine PRIVATE ${vulkan_headers_SOURCE_DIR}/include)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})
