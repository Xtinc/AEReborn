function(aereborn_test_group group source library)
    set(target aereborn_${group}_tests)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${library})
    foreach(case IN LISTS ARGN)
        add_test(NAME ${group}.${case} COMMAND ${target} ${case})
        set_tests_properties(${group}.${case} PROPERTIES LABELS ${group} TIMEOUT 20)
    endforeach()
endfunction()

aereborn_test_group(core engine_graph_tests.cpp aereborn::core
    audio_block_layout channel_matrix_validation engine_timing_validation connection_validation
    feedback_rejection prepare_retry node_exception_propagation handle_uniqueness)
aereborn_test_group(routing graph_routing_tests.cpp aereborn::plugins
    one_to_one processor_chain one_to_many many_to_one many_to_many bus_chain_and_taps
    ports_and_matrices missing_endpoints mixed_device_cadence topology_rebuild)
aereborn_test_group(adapter pcm_adapter_tests.cpp aereborn::plugins
    pcm_format_conversion buffer_binding unaligned_float_pcm channel_mapping invalid_pcm_configuration
    graph_timing_mismatch zero_contributions integer_sample_limits)
aereborn_test_group(resampling pcm_resampling_tests.cpp aereborn::plugins
    rate_and_period_continuity antialias_filter)
aereborn_test_group(device pcm_reconfiguration_tests.cpp aereborn::plugins
    capture_format_switch playback_format_switch)
aereborn_test_group(virtual virtual_pcm_tests.cpp aereborn::plugins
    source_wire_formats source_timeline sink_packet_ownership configuration_validation)
aereborn_test_group(allocation allocation_failure_tests.cpp aereborn::plugins
    buffer_resize_rollback pcm_configuration_rollback)
set_tests_properties(allocation.buffer_resize_rollback allocation.pcm_configuration_rollback
    PROPERTIES SKIP_RETURN_CODE 77)
