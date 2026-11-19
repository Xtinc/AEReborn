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
    audio_block_layout engine_timing_validation connection_validation feedback_rejection prepare_retry
    node_exception_propagation handle_uniqueness node_replacement)
aereborn_test_group(runtime clocked_engine_tests.cpp aereborn::core
    plan_switch plan_reclamation start_failure_rollback live_graph_edits pause_shared_branch stop_barrier)
aereborn_test_group(routing graph_routing_tests.cpp aereborn::plugins
    explicit_mix_and_fanout algorithm_chain silence_and_pruning format_validation channel_split_same_tick)
aereborn_test_group(convert convert_tests.cpp aereborn::plugins
    pcm_formats float_layout bit_preservation unaligned_pcm converter_validation integer_limits empty_blocks)
aereborn_test_group(resampling pcm_resampling_tests.cpp aereborn::plugins rate_continuity antialias_filter late_join)
aereborn_test_group(mix mix_tests.cpp aereborn::plugins matrix_mapping matrix_validation gain_ramp_and_zero mixer_mute)
aereborn_test_group(device io_tests.cpp aereborn::plugins buffer_binding future_binding topology_format_switch)
aereborn_test_group(tone tone_tests.cpp aereborn::plugins tone_timeline tone_validation)
aereborn_test_group(allocation allocation_failure_tests.cpp aereborn::plugins
    buffer_resize_rollback conversion_rollback identity_needs_no_workspace plan_allocation_rollback render_uses_prepared_buffers)
set_tests_properties(allocation.buffer_resize_rollback allocation.conversion_rollback allocation.identity_needs_no_workspace allocation.plan_allocation_rollback
    allocation.render_uses_prepared_buffers PROPERTIES SKIP_RETURN_CODE 77)
