cmake_minimum_required(VERSION 3.16)

project(aereborn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

aux_source_directory("${CMAKE_CURRENT_SOURCE_DIR}/source" AEREBORN_SOURCES)
aux_source_directory("${CMAKE_CURRENT_SOURCE_DIR}/include" AEREBORN_INCLUDE_SOURCES)

add_library(aereborn STATIC ${AEREBORN_SOURCES} ${AEREBORN_INCLUDE_SOURCES})
add_library(aereborn::aereborn ALIAS aereborn)

target_include_directories(aereborn
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source"
)

include(CTest)

if(BUILD_TESTING)
    aux_source_directory("${CMAKE_CURRENT_SOURCE_DIR}/tests" AEREBORN_TEST_SOURCES)
    if(AEREBORN_TEST_SOURCES)
        add_executable(aereborn_tests ${AEREBORN_TEST_SOURCES})
        target_link_libraries(aereborn_tests PRIVATE aereborn::aereborn)
        add_test(NAME aereborn_tests COMMAND aereborn_tests)
    endif()
endif()
