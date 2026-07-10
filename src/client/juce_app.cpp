#include "juce_app.h"

#include "client_app_facade.h"
#include "client_audio_devices.h"
#include "client_config_path.h"
#include "client_runtime.h"
#include "client_startup.h"
#include "http_json_fetch_job.h"
#include "juce_main_window.h"
#include "logging_setup.h"
#include "opus_defines.h"
#include "performer_join_token.h"
#include "secure_invite.h"
#include "sesivo_version.h"

#include <asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace {

constexpr const char* APP_INFO_URL = "https://sesivo.app/info.json";
constexpr const char* DEFAULT_DOWNLOADS_URL = "https://sesivo.app/downloads.html";
constexpr int UPDATE_CHECK_TIMEOUT_MS = 3000;

bool apply_startup_latency_profile(ClientAppFacade& client,
                                   const ClientStartupOptions& startup_options) {
    const StartupLatencyProfile profile = resolve_startup_latency_profile(startup_options);
    if (!profile.valid) {
        return false;
    }
    if (!profile.enabled) {
        return true;
    }

    if (!startup_options.startup_opus_packet_frames.has_value()) {
        client.set_opus_network_frame_count(profile.opus_packet_frames);
    }
    if (!startup_options.startup_jitter_packets.has_value() &&
        !startup_options.startup_jitter_ms.has_value()) {
        client.set_opus_jitter_buffer_ms(profile.jitter_ms);
    }
    if (!startup_options.startup_queue_limit_packets.has_value()) {
        client.set_opus_queue_limit_packets(profile.queue_limit_packets);
    }
    if (!startup_options.startup_age_limit_ms.has_value()) {
        client.set_jitter_packet_age_limit_ms(profile.age_limit_ms);
    }
    if (!startup_options.startup_auto_jitter &&
        !startup_options.startup_disable_auto_jitter) {
        client.set_opus_auto_jitter_default(profile.auto_jitter);
    }
    if (!startup_options.startup_redundancy_depth_packets.has_value()) {
        client.set_opus_redundancy_depth(profile.redundancy_depth);
    }

    spdlog::info("Startup latency profile: {}", profile.name);
    return true;
}

std::vector<std::string> command_line_strings() {
    std::vector<std::string> args;
    args.emplace_back("sesivo");
    for (const auto& arg: juce::JUCEApplicationBase::getCommandLineParameterArray()) {
        args.push_back(arg.toStdString());
    }
    return args;
}

ClientStartupOptions parse_current_command_line() {
    auto args = command_line_strings();
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg: args) {
        argv.push_back(arg.data());
    }
    return parse_startup_options(static_cast<int>(argv.size()), argv.data());
}

std::string invite_text_from_candidates(const std::vector<std::string>& candidates) {
    std::string first_detected;
    std::string reason;
    for (const auto& text: candidates) {
        if (text.empty() || !contains_secure_invite_link(text)) {
            continue;
        }
        if (first_detected.empty()) {
            first_detected = text;
        }
        if (parse_secure_invite_text(text, reason).has_value()) {
            spdlog::info("Received sesivo:// join launch");
            return text;
        }
    }

    if (!first_detected.empty()) {
        parse_secure_invite_text(first_detected, reason);
        spdlog::warn("Received sesivo:// join launch, but invite did not parse: {}",
                     reason);
    }
    return first_detected;
}

std::string invite_text_from_launch(const juce::String& command_line,
                                    bool include_current_arguments) {
    std::vector<std::string> candidates;
    candidates.push_back(command_line.toStdString());
    if (include_current_arguments) {
        for (const auto& arg: juce::JUCEApplicationBase::getCommandLineParameterArray()) {
            candidates.push_back(arg.toStdString());
        }
    }
    return invite_text_from_candidates(candidates);
}

std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(),
                             [&](char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char c) { return !is_space(c); })
                    .base(),
                value.end());
    return value;
}

std::optional<std::vector<int>> parse_version_numbers(std::string version) {
    version = trim_ascii(std::move(version));
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
        version.erase(version.begin());
    }
    if (version.empty()) {
        return std::nullopt;
    }

    std::vector<int> parts;
    size_t start = 0;
    while (start <= version.size()) {
        const size_t dot = version.find('.', start);
        const size_t end = dot == std::string::npos ? version.size() : dot;
        if (end == start) {
            return std::nullopt;
        }

        int value = 0;
        for (size_t i = start; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(version[i]);
            if (!std::isdigit(c)) {
                return std::nullopt;
            }
            const int digit = version[i] - '0';
            if (value > (std::numeric_limits<int>::max() - digit) / 10) {
                return std::nullopt;
            }
            value = value * 10 + digit;
        }
        parts.push_back(value);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }

    return parts.empty() ? std::nullopt
                         : std::optional<std::vector<int>>(std::move(parts));
}

bool version_is_newer(const std::string& latest, const std::string& current) {
    const auto latest_parts = parse_version_numbers(latest);
    const auto current_parts = parse_version_numbers(current);
    if (!latest_parts.has_value() || !current_parts.has_value()) {
        return false;
    }

    const size_t count = std::max(latest_parts->size(), current_parts->size());
    for (size_t i = 0; i < count; ++i) {
        const int latest_value = i < latest_parts->size() ? (*latest_parts)[i] : 0;
        const int current_value =
            i < current_parts->size() ? (*current_parts)[i] : 0;
        if (latest_value != current_value) {
            return latest_value > current_value;
        }
    }
    return false;
}

std::string json_string_property(const juce::var& root, const char* key) {
    auto* object = root.getDynamicObject();
    if (object == nullptr) {
        return {};
    }
    return trim_ascii(object->getProperty(key).toString().toStdString());
}

#if JUCE_WINDOWS
bool set_registry_string(HKEY parent, const wchar_t* subkey, const wchar_t* name,
                         const std::wstring& value) {
    HKEY key = nullptr;
    const auto created = RegCreateKeyExW(parent, subkey, 0, nullptr, 0, KEY_SET_VALUE,
                                         nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS || key == nullptr) {
        return false;
    }
    const auto close_key = [key]() { RegCloseKey(key); };
    const DWORD byte_count =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const auto written = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        byte_count);
    close_key();
    return written == ERROR_SUCCESS;
}

std::wstring current_executable_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size() - 1) {
            return std::wstring(buffer.data(), written);
        }
        buffer.resize(buffer.size() * 2);
    }
}

void register_windows_url_protocol() {
    const auto exe_path = current_executable_path();
    if (exe_path.empty()) {
        spdlog::warn("Could not resolve executable path for sesivo:// registration");
        return;
    }

    bool ok = true;
    ok = set_registry_string(HKEY_CURRENT_USER, L"Software\\Classes\\sesivo",
                             nullptr, L"URL:sesivo") &&
         ok;
    ok = set_registry_string(HKEY_CURRENT_USER, L"Software\\Classes\\sesivo",
                             L"URL Protocol", L"") &&
         ok;
    ok = set_registry_string(HKEY_CURRENT_USER,
                             L"Software\\Classes\\sesivo\\DefaultIcon",
                             nullptr, L"\"" + exe_path + L"\",0") &&
         ok;
    ok = set_registry_string(
             HKEY_CURRENT_USER,
             L"Software\\Classes\\sesivo\\shell\\open\\command", nullptr,
             L"\"" + exe_path + L"\" \"%1\"") &&
         ok;
    if (!ok) {
        spdlog::warn("Could not register sesivo:// URL protocol for current user");
    }
}
#else
void register_windows_url_protocol() {}
#endif

class SesivoApplication final : public juce::JUCEApplication,
                                private juce::Timer {
public:
    const juce::String getApplicationName() override { return "sesivo"; }
    const juce::String getApplicationVersion() override { return SESIVO_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& command_line) override {
        try {
            startup_options_ = parse_current_command_line();
            const auto log_level = startup_options_.log_file_path.empty()
                                       ? logging::default_level()
                                       : spdlog::level::info;
            logging::init(true, true, !startup_options_.log_file_path.empty(),
                          startup_options_.log_file_path, log_level);
            if (!startup_options_.log_file_path.empty()) {
                spdlog::info("Logging to {}", startup_options_.log_file_path);
            }
            spdlog::info("Runtime: process=client platform={} arch={}",
                         runtime_platform_name(), runtime_arch_name());
            register_windows_url_protocol();

            if (startup_options_.startup_jitter_packets.has_value() &&
                startup_options_.startup_jitter_ms.has_value()) {
                spdlog::error(
                    "Cannot combine packet jitter override (--jitter/--opus-jitter) with "
                    "millisecond jitter override (--jitter-ms/--opus-jitter-ms)");
                setApplicationReturnValue(2);
                quit();
                return;
            }

            if (startup_options_.list_audio_devices) {
                print_audio_backend_inventory();
                quit();
                return;
            }
            if (startup_options_.low_latency_check) {
                setApplicationReturnValue(
                    run_low_latency_backend_check(startup_options_));
                quit();
                return;
            }

            start_runtime(command_line);
        } catch (const std::exception& e) {
            spdlog::error("ERR: {}", e.what());
            logging::flush();
            setApplicationReturnValue(1);
            quit();
        }
    }

    void shutdown() override {
        stopTimer();
        update_check_.join();
        update_dialog_.reset();
        main_window_ = nullptr;
        if (client_runtime_ != nullptr) {
            auto& client_app = client_runtime_->app_facade();
            client_app.stop_audio_stream();
            client_runtime_->stop_connection();
        }
        if (io_context_ != nullptr) {
            io_context_->stop();
        }
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        client_runtime_.reset();
        io_context_.reset();
        logging::flush();
    }

    void anotherInstanceStarted(const juce::String& command_line) override {
        open_invite_if_present(command_line);
        if (main_window_ != nullptr) {
            main_window_->toFront(true);
        }
    }

private:
    void timerCallback() override {
        if (main_window_ == nullptr) {
            stopTimer();
            return;
        }

        if (auto result = update_check_.poll()) {
            stopTimer();
            apply_update_check_result(*result);
        }
    }

    void start_runtime(const juce::String& command_line) {
        io_context_ = std::make_unique<asio::io_context>();
        const auto config_path = client_config_path(startup_options_.config_dir);
        const auto audio_preferences = load_audio_device_preferences(config_path);

        client_runtime_ = std::make_unique<ClientRuntime>(
            *io_context_, startup_options_.performer_join, config_path,
            audio_preferences);
        auto& client_app = client_runtime_->app_facade();

        if (!apply_startup_options(client_app)) {
            return;
        }
        io_thread_ = std::thread([this]() {
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            io_context_->run();
        });

        auto launch_invite = invite_text_from_launch(command_line, true);

        JuceClientStartupOptions gui_startup_options;
        gui_startup_options.audio_preferences = audio_preferences;
        gui_startup_options.config_path = config_path;
        gui_startup_options.required_audio_api = startup_options_.required_audio_api;
        gui_startup_options.startup_input_channel_index =
            startup_options_.startup_input_channel_index;
        gui_startup_options.server_address = startup_options_.server_address;
        gui_startup_options.server_port = startup_options_.server_port;
        gui_startup_options.server_endpoint_explicit =
            startup_options_.server_endpoint_explicit;
        std::string token_reason;
        const auto startup_token = performer_join_token::parse_unverified(
            startup_options_.performer_join.join_token, token_reason);
        if (startup_token.has_value()) {
            gui_startup_options.room_instance_id =
                startup_token->claims.room_instance_id;
            gui_startup_options.access_epoch = startup_token->claims.access_epoch;
        }
        gui_startup_options.media_secret =
            startup_options_.performer_join.media_secret;
        gui_startup_options.auto_connect =
            launch_invite.empty() &&
            !startup_options_.performer_join.room_id.empty() &&
            !startup_options_.performer_join.user_id.empty();

        main_window_ = std::make_unique<JuceMainWindow>(
            "sesivo", client_app, std::move(gui_startup_options),
            []() { juce::JUCEApplicationBase::quit(); });
        schedule_update_check();
        if (!launch_invite.empty()) {
            schedule_open_invite(std::move(launch_invite));
        }
    }

    bool apply_startup_options(ClientAppFacade& client_app) {
        if (startup_options_.requested_frames > 0) {
            client_app.set_requested_frames_per_buffer(
                startup_options_.requested_frames);
            spdlog::info("Startup requested buffer override: {} frames",
                         startup_options_.requested_frames);
        }
        if (!apply_startup_latency_profile(client_app, startup_options_)) {
            setApplicationReturnValue(2);
            quit();
            return false;
        }
        if (startup_options_.startup_opus_packet_frames.has_value()) {
            client_app.set_opus_network_frame_count(
                *startup_options_.startup_opus_packet_frames);
            spdlog::info("Startup Opus packet override: {} frames",
                         *startup_options_.startup_opus_packet_frames);
        }
        if (startup_options_.startup_jitter_ms.has_value()) {
            client_app.set_opus_jitter_buffer_ms(
                std::max(*startup_options_.startup_jitter_ms, 0));
            spdlog::info("Startup Opus jitter override: {} ms",
                         *startup_options_.startup_jitter_ms);
        }
        if (startup_options_.startup_jitter_packets.has_value()) {
            client_runtime_->set_opus_jitter_buffer_packets(
                static_cast<size_t>(
                    std::max(*startup_options_.startup_jitter_packets, 0)));
            spdlog::info("Startup Opus jitter override: {} packets",
                         *startup_options_.startup_jitter_packets);
        }
        if (startup_options_.startup_queue_limit_packets.has_value()) {
            client_app.set_opus_queue_limit_packets(
                static_cast<size_t>(
                    std::max(*startup_options_.startup_queue_limit_packets, 0)));
            spdlog::info("Startup Opus queue limit override: {} packets",
                         *startup_options_.startup_queue_limit_packets);
        }
        if (startup_options_.startup_redundancy_depth_packets.has_value()) {
            client_app.set_opus_redundancy_depth(
                *startup_options_.startup_redundancy_depth_packets);
            const int depth = client_app.get_opus_redundancy_depth_setting();
            spdlog::info("Startup Opus redundancy depth override: {}",
                         depth == OPUS_REDUNDANCY_DEPTH_AUTO
                             ? "auto"
                             : std::to_string(depth));
        }
        if (startup_options_.startup_age_limit_ms.has_value()) {
            client_app.set_jitter_packet_age_limit_ms(
                *startup_options_.startup_age_limit_ms);
            spdlog::info("Startup packet age limit override: {} ms",
                         *startup_options_.startup_age_limit_ms);
        }
        if (startup_options_.startup_disable_auto_jitter) {
            client_app.set_opus_auto_jitter_default(false);
            spdlog::info("Startup Opus auto jitter default disabled");
        } else if (startup_options_.startup_auto_jitter) {
            client_app.set_opus_auto_jitter_default(true);
            spdlog::info("Startup Opus auto jitter default enabled");
        }
        return true;
    }

    void schedule_update_check() {
        update_check_.start(APP_INFO_URL, UPDATE_CHECK_TIMEOUT_MS);
        startTimerHz(10);
    }

    void apply_update_check_result(const HttpJsonFetchResult& result) {
        if (!result.ok) {
            return;
        }

        const auto status = json_string_property(result.json, "status");
        if (!status.empty() && status != "ok") {
            return;
        }

        const auto latest_version =
            json_string_property(result.json, "latest_version");
        if (!version_is_newer(latest_version, SESIVO_VERSION)) {
            return;
        }

        auto downloads_page = json_string_property(result.json, "downloads_page");
        if (downloads_page.empty()) {
            downloads_page = DEFAULT_DOWNLOADS_URL;
        }

        show_update_dialog(latest_version, downloads_page);
    }

    void show_update_dialog(const std::string& latest_version,
                            std::string downloads_page) {
        if (main_window_ == nullptr || update_dialog_ != nullptr) {
            return;
        }

        update_dialog_ = std::make_unique<juce::AlertWindow>(
            "Update Available",
            juce::String("Current version: ") + juce::String(SESIVO_VERSION) +
                "\nLatest version: " + juce::String(latest_version),
            juce::AlertWindow::NoIcon, main_window_.get());
        auto& dialog = *update_dialog_;
        dialog.addButton("Update", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog.addButton("Close", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        dialog.enterModalState(
            true, juce::ModalCallbackFunction::create(
                      [this, downloads_page = std::move(downloads_page)](int result) {
                          if (result == 1) {
                              juce::URL(downloads_page).launchInDefaultBrowser();
                          }
                          if (update_dialog_ != nullptr) {
                              update_dialog_->exitModalState(result);
                              update_dialog_->setVisible(false);
                              update_dialog_.reset();
                          }
                      }),
            false);
    }

    void open_invite_if_present(const juce::String& command_line) {
        auto launch_invite = invite_text_from_launch(command_line, false);
        if (!launch_invite.empty() && main_window_ != nullptr) {
            schedule_open_invite(std::move(launch_invite));
        }
    }

    void schedule_open_invite(std::string invite_text) {
        if (main_window_ == nullptr) {
            return;
        }

        juce::Component::SafePointer<JuceMainWindow> safe_window(main_window_.get());
        juce::Timer::callAfterDelay(
            150, [safe_window, invite_text = std::move(invite_text)]() mutable {
                if (auto* window = safe_window.getComponent()) {
                    window->open_invite(std::move(invite_text));
                }
            });
    }

    ClientStartupOptions startup_options_;
    std::unique_ptr<asio::io_context> io_context_;
    std::unique_ptr<ClientRuntime> client_runtime_;
    std::thread io_thread_;
    std::unique_ptr<JuceMainWindow> main_window_;
    HttpJsonFetchJob update_check_;
    std::unique_ptr<juce::AlertWindow> update_dialog_;
};

juce::JUCEApplicationBase* create_sesivo_application() {
    return new SesivoApplication();
}

}  // namespace

int run_sesivo_juce_client_app(int argc, char** argv) {
    juce::JUCEApplicationBase::createInstance = &create_sesivo_application;
#if JUCE_WINDOWS && !defined(_CONSOLE)
    (void)argc;
    (void)argv;
    return juce::JUCEApplicationBase::main();
#else
    std::vector<const char*> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    return juce::JUCEApplicationBase::main(static_cast<int>(args.size()), args.data());
#endif
}
