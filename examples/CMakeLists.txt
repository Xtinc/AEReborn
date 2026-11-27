add_executable(clocked_graph clocked_graph.cpp)
target_link_libraries(clocked_graph PRIVATE aereborn_test_plugins)
target_include_directories(clocked_graph PRIVATE "${PROJECT_SOURCE_DIR}")

add_executable(wave_test wave_test.cpp)
target_link_libraries(wave_test PRIVATE aereborn_test_plugins)
target_include_directories(wave_test PRIVATE "${PROJECT_SOURCE_DIR}")
