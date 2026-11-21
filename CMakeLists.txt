cmake_minimum_required(VERSION 3.16)

project(aereborn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(aereborn_core STATIC
    core/engine.cpp
    core/types.cpp
)
add_library(aereborn::core ALIAS aereborn_core)

target_include_directories(aereborn_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(aereborn_core PRIVATE Threads::Threads)

add_library(aereborn_plugins STATIC
    plugins/convert.cpp
    plugins/device_stream.cpp
    plugins/drift.cpp
    plugins/io.cpp
    plugins/matrix.cpp
    plugins/mixer.cpp
)
add_library(aereborn::plugins ALIAS aereborn_plugins)
target_link_libraries(aereborn_plugins PUBLIC aereborn::core)

include(CTest)
option(BUILD_EXAMPLES "Build AEReborn examples" ON)

if(BUILD_TESTING OR BUILD_EXAMPLES)
    add_subdirectory(tests/support)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
