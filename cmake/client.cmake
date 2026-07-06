# Client-specific configuration

include(cmake/common.cmake)

# ============================================================
# Client Dependencies
# ============================================================

FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.10
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(juce)

find_path(ASIO_SDK_INCLUDE_DIR
    NAMES iasiodrv.h
    PATHS
        "$ENV{ASIO_SDK_DIR}"
        "$ENV{ASIOSDK_DIR}"
        "C:/ASIOSDK"
        "C:/ASIO SDK"
    PATH_SUFFIXES common
)
find_path(JACK_INCLUDE_DIR jack/jack.h)

set(JUCE_CLIENT_ENABLE_ASIO 0)
if(WIN32 AND ASIO_SDK_INCLUDE_DIR)
    set(JUCE_CLIENT_ENABLE_ASIO 1)
endif()

set(JUCE_CLIENT_ENABLE_JACK 0)
if(JACK_INCLUDE_DIR)
    set(JUCE_CLIENT_ENABLE_JACK 1)
endif()

message(STATUS "JUCE ASIO support: ${JUCE_CLIENT_ENABLE_ASIO}")
message(STATUS "JUCE JACK support: ${JUCE_CLIENT_ENABLE_JACK}")

set(JAM_CLIENT_PLATFORM_SOURCES)
if(APPLE)
    set(JAM_CLIENT_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/sesivo.icns")
    set_source_files_properties("${JAM_CLIENT_ICON_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND JAM_CLIENT_PLATFORM_SOURCES "${JAM_CLIENT_ICON_ICNS}")
endif()
if(WIN32)
    list(APPEND JAM_CLIENT_PLATFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/sesivo.rc"
    )
endif()

add_executable(client
    ${JAM_CLIENT_DIR}/client.cpp
    ${JAM_CLIENT_DIR}/client_audio_devices.cpp
    ${JAM_CLIENT_DIR}/client_audio_state.cpp
    ${JAM_CLIENT_DIR}/client_config_path.cpp
    ${JAM_CLIENT_DIR}/client_join_session.cpp
    ${JAM_CLIENT_DIR}/client_media_state.cpp
    ${JAM_CLIENT_DIR}/client_metronome.cpp
    ${JAM_CLIENT_DIR}/client_network_path.cpp
    ${JAM_CLIENT_DIR}/client_runtime.cpp
    ${JAM_CLIENT_DIR}/client_startup.cpp
    ${JAM_CLIENT_DIR}/juce_app.cpp
    ${JAM_CLIENT_DIR}/audio_stream.cpp
    ${JAM_CLIENT_DIR}/juce_audio_backend.cpp
    ${JAM_CLIENT_DIR}/juce_participant_list_component.cpp
    ${JAM_CLIENT_DIR}/juce_main_window.cpp
    ${JAM_CLIENT_DIR}/juce_mixer_component.cpp
    ${JAM_CLIENT_DIR}/juce_room_browser_component.cpp
    ${JAM_CLIENT_DIR}/juce_root_component.cpp
    ${JAM_CLIENT_DIR}/juce_status_bar_component.cpp
    ${JAM_CLIENT_DIR}/juce_theme.cpp
    ${JAM_COMMON_DIR}/logging_setup.cpp
    ${JAM_CLIENT_PLATFORM_SOURCES}
)
jam_add_project_includes(client)

set_target_properties(client PROPERTIES
    OUTPUT_NAME sesivo
)

if(APPLE)
    set_target_properties(client PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos_client_info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "sesivo"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.sesivo.client"
        MACOSX_BUNDLE_ICON_FILE "sesivo.icns"
        MACOSX_BUNDLE_INFO_STRING "sesivo"
        MACOSX_BUNDLE_LONG_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_COPYRIGHT "Copyright 2026 sesivo"
    )
endif()

add_executable(ui_style_sandbox
    ${JAM_CLIENT_DIR}/ui_style_sandbox.cpp
)
jam_add_project_includes(ui_style_sandbox)

target_compile_definitions(client PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_ASIO=${JUCE_CLIENT_ENABLE_ASIO}
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_JACK=${JUCE_CLIENT_ENABLE_JACK}
    JUCE_ALSA=1
    JUCE_USE_ANDROID_OBOE=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_compile_definitions(ui_style_sandbox PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

if(ASIO_SDK_INCLUDE_DIR)
    target_include_directories(client PRIVATE ${ASIO_SDK_INCLUDE_DIR})
endif()
if(JACK_INCLUDE_DIR)
    target_include_directories(client PRIVATE ${JACK_INCLUDE_DIR})
endif()

target_link_libraries(client PRIVATE
    asio
    concurrentqueue
    spdlog::spdlog
    opus
    token_crypto
    juce::juce_audio_devices
    juce::juce_audio_basics
    juce::juce_core
    juce::juce_events
    juce::juce_gui_basics
)

target_link_libraries(ui_style_sandbox PRIVATE
    juce::juce_core
    juce::juce_events
    juce::juce_gui_basics
)

if(WIN32)
    target_link_libraries(client PRIVATE Avrt Qwave)
endif()
