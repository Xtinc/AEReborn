function(aereborn_test_group group source libraries)
    get_filename_component(suite "${source}" NAME_WE)
    set(target aereborn_${group}_${suite})
    add_executable(${target} ${group}/${source})
    target_link_libraries(${target} PRIVATE ${libraries})
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
    if(group STREQUAL "bus" AND suite STREQUAL "wave_tests")
        target_compile_definitions(${target} PRIVATE AEREBORN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    endif()
    foreach(case IN LISTS ARGN)
        add_test(NAME ${group}.${case} COMMAND ${target} ${case})
        set_tests_properties(${group}.${case} PROPERTIES LABELS ${group} TIMEOUT 20 SKIP_RETURN_CODE 77)
    endforeach()
endfunction()

add_executable(aereborn_public_types core/public_types.cpp)
target_link_libraries(aereborn_public_types PRIVATE aereborn::aereborn)
add_test(NAME core.public_types COMMAND aereborn_public_types)
set_tests_properties(core.public_types PROPERTIES LABELS core TIMEOUT 20)

aereborn_test_group(bus wave_tests.cpp aereborn_test_plugins
    four_channel_song_archive repeated_tracks_and_segments three_output_specs)
aereborn_test_group(bus routing_tests.cpp aereborn_test_plugins
    subgroup_and_monitor parallel_pcm_algorithm live_input_switch)
aereborn_test_group(bus stream_tests.cpp aereborn_test_plugins
    slow_callback_parallel_archive irregular_push_and_pull capture_gaps_keep_playback_timeline priority_mixer_silent_packets
    device_loss_and_rebind restart_shared_outputs)

aereborn_test_group(core engine_tests.cpp aereborn_test_plugins
    graph_constraints composite_node_id port_activity commit_failure_and_retry clock_boundary_barriers
    fractional_frames_and_ring_wrap)
aereborn_test_group(dsp plugin_tests.cpp aereborn::aereborn
    pcm_byte_values float_channel_bits matrix_and_mix_contracts matrix_and_mix_activity
    mixer_priority_limits mixer_priority_transitions mixer_priority_single_frame_and_reset
    fractional_rate_and_antialias convert_activity_and_tail)
aereborn_test_group(io backend_tests.cpp aereborn::aereborn
    endpoint_ownership_and_restart device_worker_errors wav_seek_and_padding sample_clock_drift)
aereborn_test_group(io diagnostics_tests.cpp aereborn::aereborn
    diagnostic_reports mixer_priority_diagnostics endpoint_diagnostics)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    aereborn_test_group(io platform_tests.cpp aereborn::aereborn
        wasapi_rebind_after_open_error wasapi_capture wasapi_playback)
    set_property(TEST io.wasapi_capture io.wasapi_playback APPEND PROPERTY LABELS hardware)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ALSA_FOUND)
    aereborn_test_group(io platform_tests.cpp aereborn::aereborn alsa_interleaved alsa_prefer_mmap)
endif()
aereborn_test_group(regression allocation_tests.cpp aereborn::aereborn
    commit_allocation_rollback backend_allocation_rollback render_and_passive_io_without_heap active_workers_without_heap)
