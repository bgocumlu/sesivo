cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(jam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Keep dependency population noisy enough to diagnose slow downloads/clones.
set(FETCHCONTENT_QUIET OFF CACHE BOOL "Show FetchContent population output")

option(JAM_BUILD_CLIENT "Build the desktop GUI client" ON)
option(JAM_BUILD_TESTS "Build and register jam self-tests" OFF)

set(JAM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(JAM_COMMON_DIR "${JAM_SRC_DIR}/common")
set(JAM_CLIENT_DIR "${JAM_SRC_DIR}/client")
set(JAM_SERVER_DIR "${JAM_SRC_DIR}/server")
set(JAM_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")

function(jam_add_project_includes target_name)
    target_include_directories(${target_name} PRIVATE
        ${JAM_COMMON_DIR}
        ${JAM_CLIENT_DIR}
        ${JAM_SERVER_DIR}
    )
endfunction()

# Release binaries are bundled inside Electron and must not depend on
# build-machine dylib paths.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog as a static library" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build opus as a static library" FORCE)

include(cmake/server.cmake)

if(JAM_BUILD_CLIENT)
    include(cmake/client.cmake)
endif()

if(JAM_BUILD_TESTS)
    function(jam_add_self_test target_name source_name)
        add_executable(${target_name} ${JAM_TEST_DIR}/${source_name})
        jam_add_project_includes(${target_name})
    endfunction()

    jam_add_self_test(client_manager_self_test client_manager_self_test.cpp)
    target_link_libraries(client_manager_self_test PRIVATE asio concurrentqueue token_crypto)

    add_executable(client_startup_self_test
        ${JAM_TEST_DIR}/client_startup_self_test.cpp
        ${JAM_CLIENT_DIR}/client_config_path.cpp
    )
    jam_add_project_includes(client_startup_self_test)
    target_link_libraries(client_startup_self_test PRIVATE asio concurrentqueue spdlog::spdlog opus juce::juce_core)

    jam_add_self_test(server_rate_limiter_self_test server_rate_limiter_self_test.cpp)
    target_link_libraries(server_rate_limiter_self_test PRIVATE asio)

    jam_add_self_test(room_registry_self_test room_registry_self_test.cpp)
    target_link_libraries(room_registry_self_test PRIVATE token_crypto)

    jam_add_self_test(server_metrics_self_test server_metrics_self_test.cpp)

    jam_add_self_test(periodic_timer_self_test periodic_timer_self_test.cpp)
    target_link_libraries(periodic_timer_self_test PRIVATE asio spdlog::spdlog)

    jam_add_self_test(crash_reporter_self_test crash_reporter_self_test.cpp)
    if(WIN32)
        target_link_libraries(crash_reporter_self_test PRIVATE Dbghelp)
    endif()

    jam_add_self_test(join_reliability_self_test join_reliability_self_test.cpp)

    jam_add_self_test(opus_network_clock_self_test opus_network_clock_self_test.cpp)

    jam_add_self_test(audio_packet_self_test audio_packet_self_test.cpp)

    jam_add_self_test(session_crypto_self_test session_crypto_self_test.cpp)
    target_link_libraries(session_crypto_self_test PRIVATE token_crypto)

    jam_add_self_test(secure_invite_self_test secure_invite_self_test.cpp)
    target_sources(secure_invite_self_test PRIVATE ${JAM_CLIENT_DIR}/secure_invite.cpp)
    target_link_libraries(secure_invite_self_test PRIVATE token_crypto)

    jam_add_self_test(participant_packet_queue_self_test participant_packet_queue_self_test.cpp)
    target_link_libraries(participant_packet_queue_self_test PRIVATE concurrentqueue spdlog::spdlog opus)

    jam_add_self_test(participant_manager_self_test participant_manager_self_test.cpp)
    target_link_libraries(participant_manager_self_test PRIVATE concurrentqueue spdlog::spdlog opus)

    jam_add_self_test(sequence_tracker_self_test sequence_tracker_self_test.cpp)

    jam_add_self_test(udp_port_self_test udp_port_self_test.cpp)

    jam_add_self_test(udp_socket_config_self_test udp_socket_config_self_test.cpp)
    target_link_libraries(udp_socket_config_self_test PRIVATE asio)
    if(WIN32)
        target_link_libraries(udp_socket_config_self_test PRIVATE Qwave)
    endif()

    jam_add_self_test(jitter_policy_self_test jitter_policy_self_test.cpp)

    jam_add_self_test(redundant_receive_policy_self_test redundant_receive_policy_self_test.cpp)
    target_link_libraries(redundant_receive_policy_self_test PRIVATE concurrentqueue spdlog::spdlog opus)
endif()

if(JAM_BUILD_TESTS)
    jam_add_self_test(recording_writer_self_test recording_writer_self_test.cpp)
    target_link_libraries(recording_writer_self_test PRIVATE concurrentqueue)

    jam_add_self_test(audio_analysis_self_test audio_analysis_self_test.cpp)

    jam_add_self_test(audio_backend_policy_self_test audio_backend_policy_self_test.cpp)

    jam_add_self_test(juce_audio_adapter_self_test juce_audio_adapter_self_test.cpp)
endif()

if(JAM_BUILD_TESTS)
    enable_testing()

    function(jam_add_executable_test target_name)
        add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
    endfunction()

    jam_add_executable_test(client_manager_self_test)
    jam_add_executable_test(client_startup_self_test)
    jam_add_executable_test(server_rate_limiter_self_test)
    jam_add_executable_test(room_registry_self_test)
    jam_add_executable_test(server_metrics_self_test)
    jam_add_executable_test(periodic_timer_self_test)
    jam_add_executable_test(crash_reporter_self_test)
    jam_add_executable_test(join_reliability_self_test)
    jam_add_executable_test(opus_network_clock_self_test)
    jam_add_executable_test(audio_packet_self_test)
    jam_add_executable_test(session_crypto_self_test)
    jam_add_executable_test(secure_invite_self_test)
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

include(cmake/packaging.cmake)
