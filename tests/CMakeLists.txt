function(aereborn_test_group group source libraries)
    get_filename_component(suite "${source}" NAME_WE)
    set(target aereborn_${group}_${suite})
    add_executable(${target} ${group}/${source})
    target_link_libraries(${target} PRIVATE ${libraries})
    if(group STREQUAL "bus" AND suite STREQUAL "wave_tests")
        target_compile_definitions(${target} PRIVATE AEREBORN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    endif()
    foreach(case IN LISTS ARGN)
        add_test(NAME ${group}.${case} COMMAND ${target} ${case})
        set_tests_properties(${group}.${case} PROPERTIES LABELS ${group} TIMEOUT 20 SKIP_RETURN_CODE 77)
    endforeach()
endfunction()

set(bus_libraries aereborn::backend aereborn_test_plugins)
aereborn_test_group(bus wave_tests.cpp "${bus_libraries}"
    four_channel_song_archive repeated_tracks_and_segments three_output_specs)
aereborn_test_group(bus routing_tests.cpp "${bus_libraries}"
    subgroup_and_monitor parallel_pcm_algorithm live_input_switch)
aereborn_test_group(bus stream_tests.cpp "${bus_libraries}"
    slow_callback_parallel_archive irregular_push_and_pull capture_gaps_keep_playback_timeline priority_mixer_silent_packets
    device_loss_and_rebind restart_shared_outputs)

aereborn_test_group(core engine_tests.cpp "aereborn::plugins;aereborn_test_plugins"
    graph_constraints port_activity commit_failure_and_retry clock_boundary_barriers fractional_frames_and_ring_wrap)
aereborn_test_group(dsp plugin_tests.cpp "aereborn::plugins"
    pcm_byte_values float_channel_bits matrix_and_mix_contracts matrix_and_mix_activity
    mixer_priority_limits mixer_priority_transitions mixer_priority_single_frame_and_reset
    fractional_rate_and_antialias convert_activity_and_tail)
aereborn_test_group(io backend_tests.cpp aereborn::backend
    endpoint_ownership_and_restart device_worker_errors wav_seek_and_padding sample_clock_drift)
aereborn_test_group(io diagnostics_tests.cpp aereborn::diag
    diagnostic_reports mixer_priority_diagnostics endpoint_diagnostics)
get_target_property(backend_definitions aereborn_backend INTERFACE_COMPILE_DEFINITIONS)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    aereborn_test_group(io platform_tests.cpp "aereborn::backend;ole32;uuid"
        wasapi_rebind_after_open_error wasapi_capture wasapi_playback)
    set_property(TEST io.wasapi_capture io.wasapi_playback APPEND PROPERTY LABELS hardware)
elseif("AEREBORN_HAS_ALSA=1" IN_LIST backend_definitions)
    aereborn_test_group(io platform_tests.cpp aereborn::backend alsa_interleaved alsa_prefer_mmap)
endif()
aereborn_test_group(regression allocation_tests.cpp aereborn::backend
    commit_allocation_rollback backend_allocation_rollback render_and_passive_io_without_heap active_workers_without_heap)
