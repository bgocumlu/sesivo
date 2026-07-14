#include "server_options.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ServerOptions parse_args(std::initializer_list<const char*> values) {
    std::vector<std::string> storage;
    storage.reserve(values.size());
    for (const char* value: values) {
        storage.emplace_back(value);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value: storage) {
        argv.push_back(value.data());
    }

    return parse_server_options(static_cast<int>(argv.size()), argv.data());
}

void require_parse_throws(std::initializer_list<const char*> values,
                          const char* message) {
    try {
        (void)parse_args(values);
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

bool is_lowercase_hex_secret(const std::string& value) {
    if (value.size() != EPHEMERAL_JOIN_SECRET_BYTES * 2) {
        return false;
    }
    for (char ch: value) {
        const bool decimal = ch >= '0' && ch <= '9';
        const bool hex = ch >= 'a' && ch <= 'f';
        if (!decimal && !hex) {
            return false;
        }
    }
    return true;
}

void test_missing_join_secret_generates_ephemeral_key() {
    const auto first = parse_args({"sesivo-server"});
    const auto second = parse_args({"sesivo-server"});
    require(is_lowercase_hex_secret(first.join_secret),
            "default join signing key should contain 256 random bits encoded as hex");
    require(first.join_secret != second.join_secret,
            "separate server starts should generate independent signing keys");
    require(first.join_secret_ephemeral,
            "automatically generated signing key should be marked ephemeral");
}

void test_configured_join_secret_is_preserved() {
    const auto options =
        parse_args({"sesivo-server", "--join-secret", "configured-secret"});
    require(options.join_secret == "configured-secret",
            "configured join secret should be used");
    require(!options.join_secret_ephemeral,
            "explicit join secret should not be marked ephemeral");
}

void test_join_secret_requires_non_empty_value() {
    require_parse_throws({"sesivo-server", "--join-secret"},
                         "missing join secret value should throw");
    require_parse_throws({"sesivo-server", "--join-secret", ""},
                         "empty join secret value should throw");
}

void test_join_secret_file_is_loaded() {
    const auto path = std::filesystem::temp_directory_path() /
                      "sesivo-server-options-secret.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "protected-secret\r\n";
    }
    const std::string path_text = path.string();
    const auto options = parse_args(
        {"sesivo-server", "--join-secret-file", path_text.c_str()});
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    require(options.join_secret == "protected-secret",
            "secret file should be loaded without its line ending");
    require(!options.join_secret_ephemeral,
            "file-backed join secret should not be marked ephemeral");
}

void test_join_secret_sources_are_mutually_exclusive() {
    const auto path = std::filesystem::temp_directory_path() /
                      "sesivo-server-options-secret-exclusive.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "protected-secret";
    }
    const std::string path_text = path.string();
    require_parse_throws(
        {"sesivo-server", "--join-secret", "argument-secret",
         "--join-secret-file", path_text.c_str()},
        "server must reject multiple join secret sources");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_other_options_still_parse() {
    const auto options = parse_args({
        "sesivo-server",
        "--port",
        "12000",
        "--server-id",
        "test-server",
        "--log-max-bytes",
        "4096",
        "--log-max-files",
        "2",
        "--disable-crash-reports",
        "--join-secret",
        "managed-secret",
    });
    require(options.port == 12000, "port should parse");
    require(options.server_id == "test-server", "server id should parse");
    require(options.log_max_bytes == 4096, "log max bytes should parse");
    require(options.log_max_files == 2, "log max files should parse");
    require(!options.crash_reports_enabled, "crash reports flag should parse");
    require(options.join_secret == "managed-secret", "managed join secret should parse");
}

void test_server_id_must_fit_wire_format() {
    const std::string too_long(MAX_SERVER_ID_BYTES + 1, 's');
    require_parse_throws(
        {"sesivo-server", "--server-id", "", "--join-secret", "secret"},
        "empty server id must reject");
    require_parse_throws(
        {"sesivo-server", "--server-id", too_long.c_str(),
         "--join-secret", "secret"},
        "server id longer than its wire field must reject");
}

void test_unknown_options_are_rejected() {
    require_parse_throws({"sesivo-server", "--join-secret", "secret", "--typo"},
                         "unknown options must fail closed");
    require_parse_throws({"sesivo-server", "--join-secret", "secret",
                          "--allow-insecure-dev-joins"},
                         "removed insecure join mode must be rejected");
}

}  // namespace

int main() {
    test_missing_join_secret_generates_ephemeral_key();
    test_configured_join_secret_is_preserved();
    test_join_secret_requires_non_empty_value();
    test_join_secret_file_is_loaded();
    test_join_secret_sources_are_mutually_exclusive();
    test_other_options_still_parse();
    test_server_id_must_fit_wire_format();
    test_unknown_options_are_rejected();
    std::cout << "server options self-test passed\n";
    return 0;
}
