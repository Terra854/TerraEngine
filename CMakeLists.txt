cmake_minimum_required(VERSION 3.16)
project(TerraEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(TerraEngine
    Engine/Launch.cpp
    Engine/MainEngine.cpp
    Engine/Object.cpp
    Engine/ObjectManager.cpp
    Engine/ThreadPool.cpp
)

target_include_directories(TerraEngine
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine
        ${CMAKE_CURRENT_SOURCE_DIR}/External
)

find_package(Threads REQUIRED)

target_link_libraries(TerraEngine PRIVATE Threads::Threads)
