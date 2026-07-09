# Server-specific configuration

include(cmake/common.cmake)

# ============================================================
# Server Dependencies
# ============================================================

find_package(Threads REQUIRED)

# ============================================================
# Server Target
# ============================================================

add_executable(server
    ${JAM_SERVER_DIR}/server.cpp
    ${JAM_COMMON_DIR}/logging_setup.cpp
)
jam_add_project_includes(server)
target_include_directories(server PRIVATE "${SESIVO_GENERATED_DIR}")
set_target_properties(server PROPERTIES
    OUTPUT_NAME sesivo-server
)
target_link_libraries(server PRIVATE asio concurrentqueue spdlog::spdlog opus token_crypto Threads::Threads)
if(WIN32)
    target_link_libraries(server PRIVATE Qwave Dbghelp)
endif()
