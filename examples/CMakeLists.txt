add_executable(virtual_routing virtual_routing.cpp)
target_link_libraries(virtual_routing PRIVATE aereborn::plugins)

if(BUILD_TESTING)
    add_test(NAME virtual_routing_example COMMAND virtual_routing)
    set_tests_properties(virtual_routing_example PROPERTIES LABELS example TIMEOUT 20)
endif()
