# Common dependencies and setup for both server and client

if(_JAM_COMMON_INCLUDED)
    return()
endif()
set(_JAM_COMMON_INCLUDED TRUE)

# ============================================================
# Platform / Asio config
# ============================================================

add_library(asio_config INTERFACE)

if (WIN32)
    target_compile_definitions(asio_config INTERFACE
        _WIN32_WINNT=0x0A00
        ASIO_STANDALONE
    )
endif()

# ============================================================
# Dependencies
# ============================================================

FetchContent_Declare(
    asio_src
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-36-0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.16.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_Declare(
    libsodium_cmake
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG        9b2848dfc1b917a9410f0de9d81059b26cbfaa8d
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_SUBMODULES_RECURSE TRUE
)

set(_JAM_PRE_FETCH_BUILD_TESTING_DEFINED FALSE)
if(DEFINED BUILD_TESTING)
    set(_JAM_PRE_FETCH_BUILD_TESTING_DEFINED TRUE)
    set(_JAM_PRE_FETCH_BUILD_TESTING "${BUILD_TESTING}")
endif()
set(BUILD_TESTING OFF)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "Build Opus with the static MSVC runtime" FORCE)
set(SODIUM_DISABLE_TESTS ON CACHE BOOL "Disable libsodium tests" FORCE)
set(SODIUM_MINIMAL ON CACHE BOOL "Build only the libsodium high-level API set" FORCE)
FetchContent_MakeAvailable(asio_src opus spdlog libsodium_cmake)
if(DEFINED opus_BINARY_DIR AND NOT OPUS_BUILD_TESTING)
    # Opus leaves old CTest metadata behind when an existing build tree is
    # reconfigured with tests disabled. Remove it so `ctest` only runs jam tests.
    file(REMOVE "${opus_BINARY_DIR}/CTestTestfile.cmake")
endif()
if(_JAM_PRE_FETCH_BUILD_TESTING_DEFINED)
    set(BUILD_TESTING "${_JAM_PRE_FETCH_BUILD_TESTING}")
else()
    unset(BUILD_TESTING)
endif()
unset(_JAM_PRE_FETCH_BUILD_TESTING)
unset(_JAM_PRE_FETCH_BUILD_TESTING_DEFINED)

target_compile_definitions(spdlog PUBLIC SPDLOG_USE_STD_FORMAT)

# ============================================================
# Common Wrappers
# ============================================================

add_library(asio INTERFACE)
target_include_directories(asio INTERFACE
    ${asio_src_SOURCE_DIR}/asio/include
)
target_link_libraries(asio INTERFACE asio_config)

add_library(concurrentqueue INTERFACE)
target_include_directories(concurrentqueue INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/concurrentqueue
)

add_library(token_crypto INTERFACE)
target_link_libraries(token_crypto INTERFACE sodium)
