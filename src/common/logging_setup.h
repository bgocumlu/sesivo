#pragma once

#include <cstddef>
#include <string>

#include <spdlog/common.h>

namespace logging {

constexpr size_t THREAD_POOL_SIZE = 8192;
constexpr int FLUSH_INTERVAL_MS = 3000;
constexpr size_t DEFAULT_ROTATING_LOG_MAX_BYTES = 10ULL * 1024ULL * 1024ULL;
constexpr size_t DEFAULT_ROTATING_LOG_MAX_FILES = 5;

void init(bool use_stdout = true, bool use_stderr = true, bool use_file = false,
          const std::string& file_path = "logs/app.log",
          spdlog::level::level_enum level = spdlog::level::debug,
          size_t max_file_bytes = DEFAULT_ROTATING_LOG_MAX_BYTES,
          size_t max_files = DEFAULT_ROTATING_LOG_MAX_FILES);

void flush();

}  // namespace logging
