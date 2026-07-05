cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(jam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

option(JAM_BUILD_CLIENT "Build the desktop GUI client" ON)
option(JAM_BUILD_TESTS "Build and register jam self-tests" ON)

# Release binaries are bundled inside Electron and must not depend on
# build-machine dylib paths.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "Build GLFW as a static library" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog as a static library" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build opus as a static library" FORCE)

include(cmake/server.cmake)

if(JAM_BUILD_CLIENT)
    include(cmake/client.cmake)
endif()

add_executable(join_token_tool join_token_tool.cpp)
target_link_libraries(join_token_tool PRIVATE token_crypto)

if(JAM_BUILD_TESTS)
    add_executable(client_manager_self_test client_manager_self_test.cpp)
    target_link_libraries(client_manager_self_test PRIVATE asio concurrentqueue token_crypto)

    add_executable(server_rate_limiter_self_test server_rate_limiter_self_test.cpp)
    target_link_libraries(server_rate_limiter_self_test PRIVATE asio)

    add_executable(server_metrics_self_test server_metrics_self_test.cpp)

    add_executable(logger_self_test logger_self_test.cpp)
    target_link_libraries(logger_self_test PRIVATE spdlog::spdlog)

    add_executable(periodic_timer_self_test periodic_timer_self_test.cpp)
    target_link_libraries(periodic_timer_self_test PRIVATE asio spdlog::spdlog)

    add_executable(crash_reporter_self_test crash_reporter_self_test.cpp)
    if(WIN32)
        target_link_libraries(crash_reporter_self_test PRIVATE Dbghelp)
    endif()

    add_executable(join_reliability_self_test join_reliability_self_test.cpp)

    add_executable(opus_network_clock_self_test opus_network_clock_self_test.cpp)

    add_executable(audio_packet_self_test audio_packet_self_test.cpp)

    add_executable(session_crypto_self_test session_crypto_self_test.cpp)
    target_link_libraries(session_crypto_self_test PRIVATE token_crypto)

    add_executable(participant_packet_queue_self_test participant_packet_queue_self_test.cpp)
    target_link_libraries(participant_packet_queue_self_test PRIVATE concurrentqueue spdlog::spdlog opus)

    add_executable(participant_manager_self_test participant_manager_self_test.cpp)
    target_link_libraries(participant_manager_self_test PRIVATE concurrentqueue spdlog::spdlog opus)

    add_executable(sequence_tracker_self_test sequence_tracker_self_test.cpp)

    add_executable(udp_port_self_test udp_port_self_test.cpp)

    add_executable(udp_socket_config_self_test udp_socket_config_self_test.cpp)
    target_link_libraries(udp_socket_config_self_test PRIVATE asio)
    if(WIN32)
        target_link_libraries(udp_socket_config_self_test PRIVATE Qwave)
    endif()

    add_executable(jitter_policy_self_test jitter_policy_self_test.cpp)

    add_executable(redundant_receive_policy_self_test redundant_receive_policy_self_test.cpp)
    target_link_libraries(redundant_receive_policy_self_test PRIVATE concurrentqueue spdlog::spdlog opus)
endif()

if(JAM_BUILD_TESTS)
    add_executable(recording_writer_self_test recording_writer_self_test.cpp)
    target_link_libraries(recording_writer_self_test PRIVATE concurrentqueue)

    add_executable(audio_analysis_self_test audio_analysis_self_test.cpp)

    add_executable(audio_backend_policy_self_test audio_backend_policy_self_test.cpp)

    add_executable(juce_audio_adapter_self_test juce_audio_adapter_self_test.cpp)
endif()

if(JAM_BUILD_TESTS)
    enable_testing()

    function(jam_add_executable_test target_name)
        add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
    endfunction()

    jam_add_executable_test(client_manager_self_test)
    jam_add_executable_test(server_rate_limiter_self_test)
    jam_add_executable_test(server_metrics_self_test)
    jam_add_executable_test(logger_self_test)
    jam_add_executable_test(periodic_timer_self_test)
    jam_add_executable_test(crash_reporter_self_test)
    jam_add_executable_test(join_reliability_self_test)
    jam_add_executable_test(opus_network_clock_self_test)
    jam_add_executable_test(audio_packet_self_test)
    jam_add_executable_test(session_crypto_self_test)
    jam_add_executable_test(participant_packet_queue_self_test)
    jam_add_executable_test(participant_manager_self_test)
    jam_add_executable_test(sequence_tracker_self_test)
    jam_add_executable_test(udp_port_self_test)
    jam_add_executable_test(udp_socket_config_self_test)
    jam_add_executable_test(jitter_policy_self_test)
    jam_add_executable_test(redundant_receive_policy_self_test)
    jam_add_executable_test(recording_writer_self_test)
    jam_add_executable_test(audio_analysis_self_test)
    jam_add_executable_test(audio_backend_policy_self_test)
    jam_add_executable_test(juce_audio_adapter_self_test)
endif()
