function(aereborn_test_group group source libraries)
    set(target aereborn_${group}_tests)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${libraries})
    foreach(case IN LISTS ARGN)
        add_test(NAME ${group}.${case} COMMAND ${target} ${case})
        set_tests_properties(${group}.${case} PROPERTIES LABELS ${group} TIMEOUT 20)
    endforeach()
endfunction()

aereborn_test_group(core engine_graph_tests.cpp aereborn::core
    audio_block_layout frame_ring engine_timing_validation connection_validation feedback_rejection commit_retry buffer_pool_capacity
    node_exception_propagation handle_uniqueness node_replacement)
aereborn_test_group(runtime clocked_engine_tests.cpp aereborn::core
    plan_switch commit_releases_replaced_node in_flight_commit clock_reentry clock_restart
    live_graph_edits pause_shared_branch stop_barrier)
aereborn_test_group(routing graph_routing_tests.cpp "aereborn::plugins;aereborn_test_plugins"
    explicit_mix_and_fanout algorithm_chain silence_and_pruning shared_silence format_validation channel_split_same_tick)
aereborn_test_group(convert convert_tests.cpp "aereborn::plugins;aereborn_test_plugins"
    pcm_formats pcm_to_pcm float_layout bit_preservation unaligned_pcm converter_validation integer_limits empty_blocks
    fixed_slices stereo_short_fir_output frame_bounds)
aereborn_test_group(resampling pcm_resampling_tests.cpp aereborn::plugins rate_continuity antialias_filter late_join)
aereborn_test_group(mix mix_tests.cpp aereborn::plugins
    matrix_identity_bits matrix_mapping matrix_validation gain_ramp_and_zero mixer_mute empty_block_ramps)
aereborn_test_group(device io_tests.cpp "aereborn::plugins;aereborn_test_plugins"
    buffer_binding future_binding binding_period_change topology_format_switch)
get_target_property(backend_definitions aereborn_backend INTERFACE_COMPILE_DEFINITIONS)
set(platform_endpoint_tests)
if("AEREBORN_HAS_ALSA=1" IN_LIST backend_definitions)
    list(APPEND platform_endpoint_tests alsa_virtual)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND platform_endpoint_tests wasapi_open_rollback)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND platform_endpoint_tests wav_write_errors)
endif()
aereborn_test_group(endpoint endpoint_tests.cpp aereborn::backend
    queue_ranges planar_playback stop_in_flight restart_graph streaming_ranges stream_step_changes
    offline_capture playback_silence virtual_device_audio lifecycle thread_errors playback_stop sync_playback_restart io_timeout
    empty_capture_timeout startup_empty_capture_timeout sync_playback_drain sync_capture_eos null_backends
    wav_round_trip wav_reader wav_file_write wav_segments
    ${platform_endpoint_tests})
aereborn_test_group(drift drift_tests.cpp aereborn::backend small_packet_jitter large_packet_jitter endpoint_integration)
aereborn_test_group(tone tone_tests.cpp aereborn_test_plugins tone_timeline tone_validation)
aereborn_test_group(allocation allocation_failure_tests.cpp aereborn::backend
    buffer_resize_rollback conversion_rollback direct_conversion_needs_no_workspace
    plan_allocation_rollback render_uses_prepared_buffers backend_binding_rollback endpoint_uses_prepared_buffers)
set_tests_properties(allocation.buffer_resize_rollback allocation.conversion_rollback
    allocation.direct_conversion_needs_no_workspace allocation.plan_allocation_rollback
    allocation.render_uses_prepared_buffers allocation.backend_binding_rollback
    allocation.endpoint_uses_prepared_buffers PROPERTIES SKIP_RETURN_CODE 77)
