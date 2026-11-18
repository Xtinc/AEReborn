add_executable(aereborn_core_tests core_tests.cpp template_link.cpp)
target_link_libraries(aereborn_core_tests PRIVATE aereborn::core)
add_test(NAME aereborn_core_tests COMMAND aereborn_core_tests)

add_executable(aereborn_tests engine_tests.cpp)
target_link_libraries(aereborn_tests PRIVATE aereborn::plugins)
add_test(NAME aereborn_tests COMMAND aereborn_tests)

add_executable(virtual_routing virtual_routing.cpp)
target_link_libraries(virtual_routing PRIVATE aereborn::plugins)
add_test(NAME virtual_routing COMMAND virtual_routing)

add_executable(aereborn_io_adapter_tests io_adapter_tests.cpp)
target_link_libraries(aereborn_io_adapter_tests PRIVATE aereborn::plugins)
add_test(NAME aereborn_io_adapter_tests COMMAND aereborn_io_adapter_tests)
