cmake_minimum_required(VERSION 3.16)

project(aereborn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(ASIO_INCLUDE_DIR asio.hpp HINTS "${CMAKE_CURRENT_SOURCE_DIR}/asio-1.30.2/include")
if(NOT EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR "Standalone Asio is required. Provide asio-1.30.2/include or set ASIO_INCLUDE_DIR.")
endif()
find_package(Threads REQUIRED)

add_library(aereborn_core STATIC
    core/engine.cpp
    core/runtime.cpp
    core/types.cpp
)
add_library(aereborn::core ALIAS aereborn_core)

target_include_directories(aereborn_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(aereborn_core SYSTEM PRIVATE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(aereborn_core PRIVATE ASIO_STANDALONE)
target_link_libraries(aereborn_core PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(aereborn_core PRIVATE ws2_32 mswsock)
endif()

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
