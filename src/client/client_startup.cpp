#include "client_startup.h"

#include "udp_port.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <exception>
#include <fstream>
#include <spdlog/spdlog.h>
#include <system_error>

namespace {

std::string trim_copy(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return std::string(first, last);
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int normalize_opus_redundancy_depth(int depth) {
    if (depth == OPUS_REDUNDANCY_DEPTH_AUTO) {
        return depth;
    }
    return std::clamp(depth, 0, MAX_OPUS_REDUNDANCY_DEPTH_PACKETS);
}

int parse_opus_redundancy_depth_option(const std::string& value) {
    const std::string normalized = lowercase_copy(value);
    if (normalized == "auto") {
        return OPUS_REDUNDANCY_DEPTH_AUTO;
    }
    if (normalized == "off" || normalized == "none" || normalized == "disabled") {
        return 0;
    }
    return normalize_opus_redundancy_depth(std::stoi(value));
}

int silence_audio_callback(const void*, void* output, unsigned long frame_count,
                           void* user_data) {
    auto* stream = static_cast<AudioStream*>(user_data);
    if (output == nullptr || stream == nullptr) {
        return 0;
    }

    const size_t channels = static_cast<size_t>(stream->get_output_channel_count());
    std::memset(output, 0, frame_count * channels * sizeof(float));
    return 0;
}

}  // namespace

const char* runtime_platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

const char* runtime_arch_name() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::filesystem::path client_config_path(const char* executable_path,
                                         const std::string& config_dir) {
    if (!config_dir.empty()) {
        return std::filesystem::path(config_dir) / "jam_client.ini";
    }

    std::error_code ec;
    std::filesystem::path exe =
        executable_path != nullptr && executable_path[0] != '\0'
            ? std::filesystem::absolute(std::filesystem::path(executable_path), ec)
            : std::filesystem::current_path(ec);
    if (ec) {
        exe = std::filesystem::current_path();
    }
    const std::filesystem::path folder =
        exe.has_parent_path() ? exe.parent_path() : std::filesystem::current_path();
    return folder / "jam_client.ini";
}

AudioDevicePreferences load_audio_device_preferences(const std::filesystem::path& path) {
    AudioDevicePreferences preferences;
    std::ifstream input(path);
    if (!input) {
        return preferences;
    }

    preferences.loaded = true;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() > 4096 || line.find('\0') != std::string::npos) {
            continue;
        }
        line = trim_copy(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = trim_copy(line.substr(0, equals));
        const std::string value = trim_copy(line.substr(equals + 1));
        if (key == "audio_api") {
            preferences.audio_api = value.empty() ? "All" : value;
        } else if (key == "input_device") {
            preferences.input_device = value;
        } else if (key == "input_api") {
            preferences.input_api = value;
        } else if (key == "input_channel") {
            try {
                preferences.input_channel_index = std::max(0, std::stoi(value));
            } catch (const std::exception&) {
                preferences.input_channel_index.reset();
            }
        } else if (key == "output_device") {
            preferences.output_device = value;
        } else if (key == "output_api") {
            preferences.output_api = value;
        }
    }
    return preferences;
}

ClientStartupOptions parse_startup_options(int argc, char** argv) {
    ClientStartupOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            options.server_address = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            options.server_port = parse_udp_port(argv[++i], "--port");
        } else if (arg == "--app-version" && i + 1 < argc) {
            options.app_version = argv[++i];
        } else if (arg == "--config-dir" && i + 1 < argc) {
            options.config_dir = argv[++i];
        } else if (arg == "--room" && i + 1 < argc) {
            options.performer_join.room_id = argv[++i];
        } else if (arg == "--room-handle" && i + 1 < argc) {
            options.performer_join.room_handle = argv[++i];
        } else if (arg == "--user-id" && i + 1 < argc) {
            options.performer_join.user_id = argv[++i];
        } else if (arg == "--display-name" && i + 1 < argc) {
            options.performer_join.display_name = argv[++i];
        } else if (arg == "--join-token" && i + 1 < argc) {
            options.performer_join.join_token = argv[++i];
        } else if ((arg == "--frames" || arg == "--buffer-frames") && i + 1 < argc) {
            options.requested_frames = std::stoi(argv[++i]);
        } else if (arg == "--input-channel" && i + 1 < argc) {
            options.startup_input_channel_index = std::max(0, std::stoi(argv[++i]) - 1);
        } else if (arg == "--input-channel-index" && i + 1 < argc) {
            options.startup_input_channel_index = std::max(0, std::stoi(argv[++i]));
        } else if ((arg == "--latency-profile" || arg == "--opus-latency-profile") &&
                   i + 1 < argc) {
            options.startup_latency_profile = lowercase_copy(argv[++i]);
        } else if ((arg == "--opus-packet-frames" || arg == "--opus-frames" ||
                    arg == "--packet-frames") &&
                   i + 1 < argc) {
            options.startup_opus_packet_frames = std::stoi(argv[++i]);
        } else if ((arg == "--jitter" || arg == "--opus-jitter") && i + 1 < argc) {
            options.startup_jitter_packets = std::stoi(argv[++i]);
        } else if ((arg == "--jitter-ms" || arg == "--opus-jitter-ms") && i + 1 < argc) {
            options.startup_jitter_ms = std::stoi(argv[++i]);
        } else if ((arg == "--queue-limit" || arg == "--opus-queue-limit") &&
                   i + 1 < argc) {
            options.startup_queue_limit_packets = std::stoi(argv[++i]);
        } else if ((arg == "--age-limit-ms" || arg == "--jitter-age-limit-ms") &&
                   i + 1 < argc) {
            options.startup_age_limit_ms = std::stoi(argv[++i]);
        } else if ((arg == "--redundancy-depth" || arg == "--opus-redundancy-depth") &&
                   i + 1 < argc) {
            options.startup_redundancy_depth_packets =
                parse_opus_redundancy_depth_option(argv[++i]);
        } else if (arg == "--auto-jitter") {
            options.startup_auto_jitter = true;
        } else if (arg == "--no-auto-jitter") {
            options.startup_disable_auto_jitter = true;
        } else if (arg == "--list-audio-devices" || arg == "--audio-devices") {
            options.list_audio_devices = true;
        } else if (arg == "--low-latency-check" || arg == "--backend-check") {
            options.low_latency_check = true;
        } else if ((arg == "--require-api" || arg == "--api") && i + 1 < argc) {
            options.required_audio_api = argv[++i];
        } else if (arg == "--log-file" && i + 1 < argc) {
            options.log_file_path = argv[++i];
        }
    }
    return options;
}

StartupLatencyProfile resolve_startup_latency_profile(
    const ClientStartupOptions& startup_options) {
    StartupLatencyProfile profile;
    if (startup_options.startup_latency_profile.empty()) {
        return profile;
    }

    const bool low_profile = startup_options.startup_latency_profile == "low";
    const bool stable_profile = startup_options.startup_latency_profile == "stable" ||
                                startup_options.startup_latency_profile == "safe";
    const bool adaptive_profile =
        startup_options.startup_latency_profile == "adaptive" ||
        startup_options.startup_latency_profile == "balanced" ||
        startup_options.startup_latency_profile == "current" ||
        startup_options.startup_latency_profile == "default";
    if (!low_profile && !stable_profile && !adaptive_profile) {
        spdlog::error("Unknown latency profile '{}'", startup_options.startup_latency_profile);
        profile.valid = false;
        return profile;
    }

    profile.enabled = true;
    profile.name = low_profile ? "low" : stable_profile ? "stable" : "adaptive";
    profile.jitter_ms = low_profile ? 10
                                    : stable_profile ? 80 : DEFAULT_OPUS_JITTER_MS;
    profile.queue_limit_packets =
        low_profile ? 24
                    : stable_profile ? 96
                                     : static_cast<int>(DEFAULT_OPUS_QUEUE_LIMIT_PACKETS);
    profile.age_limit_ms =
        low_profile ? 60 : stable_profile ? 250 : DEFAULT_JITTER_PACKET_AGE_MS;
    profile.auto_jitter = false;
    profile.opus_packet_frames =
        low_profile      ? opus_network_clock::LOW_LATENCY_FRAME_COUNT
        : stable_profile ? opus_network_clock::STABLE_FRAME_COUNT
                         : opus_network_clock::DEFAULT_FRAME_COUNT;
    return profile;
}

void print_audio_backend_inventory() {
    spdlog::info("Available audio APIs:");
    for (const auto& api: AudioStream::get_apis()) {
        spdlog::info("API {}: {} | default input {} | default output {}", api.index,
                     api.name, api.default_input_device, api.default_output_device);
    }

    AudioStream::print_all_devices();
}

AudioStream::DeviceIndex find_device_for_api(const std::string& api_name, bool input) {
    const auto devices = input ? AudioStream::get_input_devices()
                               : AudioStream::get_output_devices();
    const auto it =
        std::find_if(devices.begin(), devices.end(), [&](const AudioStream::DeviceInfo& device) {
            return device.api_name == api_name;
        });
    return it != devices.end() ? it->index : AudioStream::NO_DEVICE;
}

bool required_api_has_duplex_devices(const std::string& api_name) {
    return find_device_for_api(api_name, true) != AudioStream::NO_DEVICE &&
           find_device_for_api(api_name, false) != AudioStream::NO_DEVICE;
}

int run_audio_backend_open_check(const ClientStartupOptions& startup_options) {
    AudioStream::DeviceIndex input_dev = AudioStream::get_default_input_device();
    AudioStream::DeviceIndex output_dev = AudioStream::get_default_output_device();
    if (!startup_options.required_audio_api.empty()) {
        input_dev = find_device_for_api(startup_options.required_audio_api, true);
        output_dev = find_device_for_api(startup_options.required_audio_api, false);
    }

    if (input_dev == AudioStream::NO_DEVICE || output_dev == AudioStream::NO_DEVICE) {
        spdlog::error("Audio backend check has no valid input/output device");
        print_audio_backend_inventory();
        return 2;
    }

    AudioStream stream;
    AudioStream::AudioConfig config;
    config.frames_per_buffer =
        startup_options.requested_frames > 0 ? startup_options.requested_frames : 120;
    if (startup_options.startup_input_channel_index.has_value()) {
        config.input_channel_index = *startup_options.startup_input_channel_index;
    }

    if (!stream.start_audio_stream(input_dev, output_dev, config, silence_audio_callback,
                                   &stream)) {
        spdlog::error("Audio backend check failed: {}", AudioStream::get_last_error());
        return 3;
    }

    stream.print_latency_info();
    stream.stop_audio_stream();
    spdlog::info("Audio backend check succeeded");
    return 0;
}

int run_low_latency_backend_check(const ClientStartupOptions& startup_options) {
    const std::string api_name =
        startup_options.required_audio_api.empty() ? "ASIO" : startup_options.required_audio_api;
    const int frames = startup_options.requested_frames > 0 ? startup_options.requested_frames : 96;

    spdlog::info("Low-latency backend check: API={} frames={}", api_name, frames);
    if (!required_api_has_duplex_devices(api_name)) {
        spdlog::error("Low-latency backend '{}' is not ready: missing input or output device",
                      api_name);
        print_audio_backend_inventory();
        return 2;
    }

    ClientStartupOptions check_options = startup_options;
    check_options.required_audio_api = api_name;
    check_options.requested_frames = frames;
    const int open_result = run_audio_backend_open_check(check_options);
    if (open_result != 0) {
        return open_result;
    }

    spdlog::info("Low-latency backend '{}' is ready for validation", api_name);
    return 0;
}
