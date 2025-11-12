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
        ${CMAKE_CURRENT_SOURCE_DIR}/External
)

find_package(Threads REQUIRED)

target_link_libraries(TerraEngine-Engine PRIVATE Threads::Threads)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Engine PREFIX "TerraEngine-Engine" FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})
