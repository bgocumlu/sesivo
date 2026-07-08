#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <sodium.h>

#include "logging_setup.h"
#include "performer_join_token.h"
#include "udp_port.h"

constexpr size_t EPHEMERAL_JOIN_SECRET_BYTES = 32;

struct ServerOptions {
    uint16_t    port = 9999;
    bool        allow_insecure_dev_joins = false;
    std::string server_id = "local-dev";
    std::string join_secret;
    bool        join_secret_ephemeral = false;
    std::string log_file_path;
    size_t      log_max_bytes = logging::DEFAULT_ROTATING_LOG_MAX_BYTES;
    size_t      log_max_files = logging::DEFAULT_ROTATING_LOG_MAX_FILES;
    std::string metrics_jsonl_path;
    bool        crash_reports_enabled = true;
    std::string crash_report_dir = "crash_reports/server";
};

inline std::string generate_ephemeral_join_secret() {
    if (!performer_join_token::ensure_sodium_initialized()) {
        throw std::runtime_error("crypto unavailable");
    }

    std::array<unsigned char, EPHEMERAL_JOIN_SECRET_BYTES> secret{};
    std::array<char, EPHEMERAL_JOIN_SECRET_BYTES * 2 + 1>  encoded{};
    randombytes_buf(secret.data(), secret.size());
    sodium_bin2hex(encoded.data(), encoded.size(), secret.data(), secret.size());
    sodium_memzero(secret.data(), secret.size());
    return std::string(encoded.data());
}

inline void ensure_join_secret(ServerOptions& options) {
    if (!options.join_secret.empty()) {
        return;
    }
    options.join_secret = generate_ephemeral_join_secret();
    options.join_secret_ephemeral = true;
}

inline size_t parse_positive_size_arg(const std::string& value,
                                      const char* option_name) {
    try {
        size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0 ||
            parsed > static_cast<unsigned long long>(
                         std::numeric_limits<size_t>::max())) {
            throw std::invalid_argument("out of range");
        }
        return static_cast<size_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("Invalid ") + option_name + ": " +
                                    value);
    }
}

inline const char* require_option_value(int argc, char** argv, int& index,
                                        const std::string& option_name) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(option_name + " requires a value");
    }
    return argv[++index];
}

inline ServerOptions parse_server_options(int argc, char** argv) {
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port") {
            options.port = parse_udp_port(require_option_value(argc, argv, i, arg),
                                          "--port");
        } else if (arg == "--server-id") {
            options.server_id = require_option_value(argc, argv, i, arg);
        } else if (arg == "--join-secret") {
            options.join_secret = require_option_value(argc, argv, i, arg);
            if (options.join_secret.empty()) {
                throw std::invalid_argument("--join-secret must not be empty");
            }
            options.join_secret_ephemeral = false;
        } else if (arg == "--log-file") {
            options.log_file_path = require_option_value(argc, argv, i, arg);
        } else if (arg == "--log-max-bytes") {
            options.log_max_bytes = parse_positive_size_arg(
                require_option_value(argc, argv, i, arg), "--log-max-bytes");
        } else if (arg == "--log-max-files") {
            options.log_max_files = parse_positive_size_arg(
                require_option_value(argc, argv, i, arg), "--log-max-files");
        } else if (arg == "--metrics-jsonl") {
            options.metrics_jsonl_path = require_option_value(argc, argv, i, arg);
        } else if (arg == "--crash-report-dir") {
            options.crash_report_dir = require_option_value(argc, argv, i, arg);
        } else if (arg == "--disable-crash-reports") {
            options.crash_reports_enabled = false;
        } else if (arg == "--allow-insecure-dev-joins") {
            options.allow_insecure_dev_joins = true;
        }
    }
    ensure_join_secret(options);
    return options;
}
