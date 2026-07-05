#include "logging_setup.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace logging {
namespace {

std::once_flag thread_pool_once;

size_t effective_or_default(size_t value, size_t fallback) {
    return value == 0 ? fallback : value;
}

}  // namespace

spdlog::level::level_enum default_level() {
#ifdef NDEBUG
    return spdlog::level::warn;
#else
    return spdlog::level::info;
#endif
}

void init(bool use_stdout, bool use_stderr, bool use_file, const std::string& file_path,
          spdlog::level::level_enum level, size_t max_file_bytes, size_t max_files) {
    std::vector<spdlog::sink_ptr> sinks;

    if (use_stdout) {
        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        sink->set_pattern("[%T] [%^%l%$] %v");
        sink->set_level(spdlog::level::debug);
        sinks.push_back(std::move(sink));
    }

    if (use_stderr) {
        auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        sink->set_pattern("[%T] [%^%l%$] %v");
        sink->set_level(spdlog::level::warn);
        sinks.push_back(std::move(sink));
    }

    if (use_file) {
        try {
            std::filesystem::path path(file_path);
            if (!path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path());
            }

            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file_path,
                effective_or_default(max_file_bytes, DEFAULT_ROTATING_LOG_MAX_BYTES),
                effective_or_default(max_files, DEFAULT_ROTATING_LOG_MAX_FILES), false);
            sink->set_pattern("[%Y-%m-%d %T.%e] [%^%l%$] %v");
            sinks.push_back(std::move(sink));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "logging: failed to create log file (%s): %s\n",
                         file_path.c_str(), e.what());
        }
    }

    if (sinks.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
    }

    std::call_once(thread_pool_once, []() {
        spdlog::init_thread_pool(THREAD_POOL_SIZE, 1);
    });

    auto logger = std::make_shared<spdlog::async_logger>(
        "core", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(std::move(logger));
    spdlog::flush_every(std::chrono::milliseconds(FLUSH_INTERVAL_MS));
}

void flush() {
    if (auto logger = spdlog::default_logger()) {
        logger->flush();
    }
}

}  // namespace logging
