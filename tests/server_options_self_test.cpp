#include "server_options.h"

#include <cstdlib>
#include <exception>
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

void test_no_join_secret_generates_ephemeral_secret() {
    const auto options = parse_args({"sesivo-server"});
    require(!options.join_secret.empty(),
            "missing join secret should generate a server secret");
    require(is_lowercase_hex_secret(options.join_secret),
            "generated join secret should be 32 random bytes encoded as hex");
    require(options.join_secret_ephemeral,
            "generated join secret should be marked ephemeral");
    require(options.port == 9999, "default port should remain unchanged");
    require(options.server_id == "local-dev",
            "default server id should remain unchanged");
}

void test_configured_join_secret_is_preserved() {
    const auto options =
        parse_args({"sesivo-server", "--join-secret", "configured-secret"});
    require(options.join_secret == "configured-secret",
            "configured join secret should be used");
    require(!options.join_secret_ephemeral,
            "configured join secret should not be marked ephemeral");
}

void test_join_secret_requires_non_empty_value() {
    require_parse_throws({"sesivo-server", "--join-secret"},
                         "missing join secret value should throw");
    require_parse_throws({"sesivo-server", "--join-secret", ""},
                         "empty join secret value should throw");
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
        "--allow-insecure-dev-joins",
    });
    require(options.port == 12000, "port should parse");
    require(options.server_id == "test-server", "server id should parse");
    require(options.log_max_bytes == 4096, "log max bytes should parse");
    require(options.log_max_files == 2, "log max files should parse");
    require(!options.crash_reports_enabled, "crash reports flag should parse");
    require(options.allow_insecure_dev_joins, "dev join flag should parse");
    require(!options.join_secret.empty(),
            "other options should still receive an ephemeral join secret");
}

}  // namespace

int main() {
    test_no_join_secret_generates_ephemeral_secret();
    test_configured_join_secret_is_preserved();
    test_join_secret_requires_non_empty_value();
    test_other_options_still_parse();
    std::cout << "server options self-test passed\n";
    return 0;
}
