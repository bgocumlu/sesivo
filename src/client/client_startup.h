#pragma once

#include "opus_network_clock.h"
#include "protocol.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct PerformerJoinOptions {
    std::string room_id;
    std::string room_handle;
    std::string user_id;
    std::string display_name;
    std::string join_token;
};

struct ClientStartupOptions {
    std::string server_address = "127.0.0.1";
    uint16_t server_port = 9999;
    std::string app_version;
    std::string config_dir;
    int requested_frames = 0;
    std::optional<int> startup_input_channel_index;
    std::string startup_latency_profile = "adaptive";
    std::optional<int> startup_opus_packet_frames;
    std::optional<int> startup_jitter_packets;
    std::optional<int> startup_jitter_ms;
    std::optional<int> startup_queue_limit_packets;
    std::optional<int> startup_age_limit_ms;
    std::optional<int> startup_redundancy_depth_packets;
    bool startup_auto_jitter = false;
    bool startup_disable_auto_jitter = false;
    bool list_audio_devices = false;
    bool low_latency_check = false;
    std::string required_audio_api;
    std::string log_file_path;
    PerformerJoinOptions performer_join;
};

struct StartupLatencyProfile {
    bool valid = true;
    bool enabled = false;
    std::string name;
    int jitter_ms = DEFAULT_OPUS_JITTER_MS;
    int queue_limit_packets = static_cast<int>(DEFAULT_OPUS_QUEUE_LIMIT_PACKETS);
    int age_limit_ms = DEFAULT_JITTER_PACKET_AGE_MS;
    bool auto_jitter = false;
    int opus_packet_frames = opus_network_clock::DEFAULT_FRAME_COUNT;
};

const char* runtime_platform_name();
const char* runtime_arch_name();

std::filesystem::path client_config_path(const char* executable_path,
                                         const std::string& config_dir);

ClientStartupOptions parse_startup_options(int argc, char** argv);
StartupLatencyProfile resolve_startup_latency_profile(
    const ClientStartupOptions& startup_options);

int run_audio_backend_open_check(const ClientStartupOptions& startup_options);
int run_low_latency_backend_check(const ClientStartupOptions& startup_options);
