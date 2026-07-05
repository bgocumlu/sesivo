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

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.5-docking
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
find_package(OpenGL REQUIRED)

FetchContent_MakeAvailable(juce imgui glfw)

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

# ============================================================
# Client Wrappers
# ============================================================

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC 
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

# ============================================================
# Client Target
# ============================================================

add_executable(client
    ${JAM_CLIENT_DIR}/client.cpp
    ${JAM_CLIENT_DIR}/client_audio_devices.cpp
    ${JAM_CLIENT_DIR}/client_audio_state.cpp
    ${JAM_CLIENT_DIR}/client_join_session.cpp
    ${JAM_CLIENT_DIR}/client_media_state.cpp
    ${JAM_CLIENT_DIR}/client_metronome.cpp
    ${JAM_CLIENT_DIR}/client_startup.cpp
    ${JAM_CLIENT_DIR}/gui.cpp
    ${JAM_CLIENT_DIR}/imgui_client_ui.cpp
    ${JAM_CLIENT_DIR}/audio_stream.cpp
    ${JAM_CLIENT_DIR}/juce_audio_backend.cpp
    ${JAM_COMMON_DIR}/logging_setup.cpp
)
jam_add_project_includes(client)

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
    imgui_lib
    juce::juce_audio_devices
    juce::juce_audio_basics
    juce::juce_core
    juce::juce_events
)

if(WIN32)
    target_link_libraries(client PRIVATE Avrt Qwave)
endif()
