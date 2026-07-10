#include "client_startup.h"

#include "latency_preset_policy.h"

#include "client_audio_devices.h"
#include "udp_port.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <spdlog/spdlog.h>
#include <system_error>

namespace {

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

ClientStartupOptions parse_startup_options(int argc, char** argv) {
    ClientStartupOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            options.server_address = argv[++i];
            options.server_endpoint_explicit = true;
        } else if (arg == "--port" && i + 1 < argc) {
            options.server_port = parse_udp_port(argv[++i], "--port");
            options.server_endpoint_explicit = true;
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
        } else if ((arg == "--media-secret" || arg == "--e2e-media-secret") &&
                   i + 1 < argc) {
            options.performer_join.media_secret = argv[++i];
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

    const bool ultra_profile = startup_options.startup_latency_profile == "ultra";
    const bool low_profile = startup_options.startup_latency_profile == "low";
    const bool stable_profile = startup_options.startup_latency_profile == "stable" ||
                                startup_options.startup_latency_profile == "safe";
    const bool adaptive_profile =
        startup_options.startup_latency_profile == "adaptive" ||
        startup_options.startup_latency_profile == "balanced" ||
        startup_options.startup_latency_profile == "current" ||
        startup_options.startup_latency_profile == "default";
    if (!ultra_profile && !low_profile && !stable_profile && !adaptive_profile) {
        spdlog::error("Unknown latency profile '{}'", startup_options.startup_latency_profile);
        profile.valid = false;
        return profile;
    }

    int preset_id = LATENCY_PRESET_BALANCED_ID;
    if (ultra_profile) {
        preset_id = LATENCY_PRESET_ULTRA_ID;
    } else if (low_profile) {
        preset_id = LATENCY_PRESET_LOW_ID;
    } else if (stable_profile) {
        preset_id = LATENCY_PRESET_STABLE_ID;
    }
    const auto& preset = *latency_preset_for_id(preset_id);
    profile.enabled = true;
    profile.name = preset.label;
    profile.jitter_ms = preset.jitter_ms;
    profile.queue_limit_packets = preset.queue_limit_packets;
    profile.age_limit_ms = preset.age_limit_ms;
    profile.auto_jitter = preset.auto_jitter;
    profile.opus_packet_frames = preset.packet_frames;
    profile.redundancy_depth = preset.redundancy_depth;
    return profile;
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
