cmake_minimum_required(VERSION 3.16)

project(aereborn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aereborn_core STATIC
    core/block.cpp
    core/graph.cpp
    core/block.h
    core/graph.h
    core/engine.h
)
add_library(aereborn::core ALIAS aereborn_core)

target_include_directories(aereborn_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_library(aereborn_plugins INTERFACE)
add_library(aereborn::plugins ALIAS aereborn_plugins)
target_link_libraries(aereborn_plugins INTERFACE aereborn::core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
