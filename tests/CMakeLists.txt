function(aereborn_test_group group)
    cmake_parse_arguments(TEST "" "" "SOURCES;CASES" ${ARGN})
    set(target aereborn_${group}_tests)
    add_executable(${target} ${TEST_SOURCES})
    target_link_libraries(${target} PRIVATE aereborn::plugins)
    foreach(case IN LISTS TEST_CASES)
        add_test(NAME ${group}.${case} COMMAND ${target} ${case})
        set_tests_properties(${group}.${case} PROPERTIES LABELS ${group} TIMEOUT 20)
    endforeach()
endfunction()

aereborn_test_group(core SOURCES core_tests.cpp template_link.cpp
    CASES blocks matrices graph_validation cycles preparation exceptions identifiers topology)
aereborn_test_group(routing SOURCES routing_tests.cpp
    CASES one_to_one processor_chain one_to_many many_to_one many_to_many bus_chain_and_taps ports_and_matrices missing_endpoints mixed_device_cadence)
aereborn_test_group(adapter SOURCES adapter_tests.cpp
    CASES formats binding cadence float_copy channel_mapping configuration_errors zero_contributions integer_limits)
aereborn_test_group(resampling SOURCES resampling_tests.cpp CASES continuity antialias)
aereborn_test_group(device SOURCES device_tests.cpp CASES capture_switch playback_switch)
aereborn_test_group(virtual SOURCES virtual_tests.cpp CASES formats timeline settings)
aereborn_test_group(allocation SOURCES allocation_tests.cpp CASES configuration_rollback)
set_tests_properties(allocation.configuration_rollback PROPERTIES SKIP_RETURN_CODE 77)
