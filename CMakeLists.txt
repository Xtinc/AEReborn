cmake_minimum_required(VERSION 3.16)

project(aereborn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(aereborn STATIC
    core/command_queue.cpp
    core/engine.cpp
    core/resampler.cpp
    core/types.cpp
    plugins/convert.cpp
    plugins/endpoint.cpp
    plugins/matrix.cpp
    plugins/mixer.cpp
    backend/backend.cpp
    backend/null_backend.cpp
    backend/pull_sink.cpp
    backend/push_source.cpp
    backend/usr_capture.cpp
    backend/usr_playback.cpp
    backend/wav_backend.cpp
    backend/wav_file.cpp
    diag/inspect.cpp
    app/audio_center.cpp
    app/channel_map.cpp
    app/device_service.cpp
    app/graph_service.cpp
)
add_library(aereborn::aereborn ALIAS aereborn)

target_include_directories(aereborn
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(aereborn PUBLIC cxx_std_14)
target_link_libraries(aereborn PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        target_sources(aereborn PRIVATE backend/alsa_backend.cpp)
        target_link_libraries(aereborn PRIVATE ALSA::ALSA)
        target_compile_definitions(aereborn PUBLIC AEREBORN_HAS_ALSA=1)
        message(STATUS "AEReborn found the ALSA library at ${ALSA_LIBRARY}")
    else()
        message(STATUS "AEReborn skipped ALSA because its development library or headers were not found")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_sources(aereborn PRIVATE backend/wasapi_backend.cpp)
    target_link_libraries(aereborn PRIVATE ole32 uuid)
endif()

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
