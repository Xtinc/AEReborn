cmake_minimum_required(VERSION 3.16)

project(aereborn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aereborn_core INTERFACE)
add_library(aereborn::core ALIAS aereborn_core)

target_include_directories(aereborn_core
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_library(aereborn_plugins STATIC
    plugins/adapter.cpp
    plugins/adapter.h
    plugins/virtual.h
    plugins/virtual.cpp
)
add_library(aereborn::plugins ALIAS aereborn_plugins)
target_link_libraries(aereborn_plugins PUBLIC aereborn::core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(BUILD_EXAMPLES "Build AEReborn examples" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
