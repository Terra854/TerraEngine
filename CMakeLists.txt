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
    Engine/Platform/Window/WindowSystem.cpp
    Engine/Rendering/RHI/RHI.cpp
    Engine/Rendering/RHI/Vulkan/VulkanRHI.cpp
    Engine/Rendering/RendererSystem.cpp
    Engine/ThreadPool/ThreadPool.cpp
)

set(ENGINE_HEADERS
    Engine/Core/MainEngine.h
    Engine/Core/SystemBase.h
    Engine/Core/Object/Component.h
    Engine/Core/Object/Object.h
    Engine/Core/Object/ObjectManager.h
    Engine/Core/Object/Transform.h
    Engine/Platform/Window/WindowSystem.h
    Engine/Rendering/RHI/RHI.h
    Engine/Rendering/RHI/RHICommon.h
    Engine/Rendering/RHI/Vulkan/VulkanRHI.h
    Engine/Rendering/RendererSystem.h
    Engine/Rendering/Shaders/Generated/MeshCubeFrag.h
    Engine/Rendering/Shaders/Generated/MeshCubeMesh.h
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
        ${CMAKE_CURRENT_SOURCE_DIR}/External
        ${CMAKE_CURRENT_SOURCE_DIR}/External/Vulkan-Headers/include
)

find_package(Threads REQUIRED)

if (WIN32)
    target_link_libraries(TerraEngine-Engine PRIVATE Threads::Threads)
else()
    find_package(X11 REQUIRED)
    find_library(DL_LIBRARY dl)
    if (NOT DL_LIBRARY)
        message(FATAL_ERROR "Failed to find the dynamic loader library (dl)")
    endif()
    target_link_libraries(TerraEngine-Engine PRIVATE Threads::Threads X11::X11 ${DL_LIBRARY})
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})
