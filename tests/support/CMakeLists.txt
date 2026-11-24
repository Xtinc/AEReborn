add_library(aereborn_test_plugins STATIC tone.cpp)
target_link_libraries(aereborn_test_plugins PUBLIC aereborn::core)
