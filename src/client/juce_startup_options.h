#pragma once

#include "client_audio_devices.h"
#include "protocol.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct JuceClientStartupOptions {
    AudioDevicePreferences audio_preferences;
    std::filesystem::path config_path;
    std::string required_audio_api;
    std::optional<int> startup_input_channel_index;
    std::string server_address;
    uint16_t server_port = 0;
    bool server_endpoint_explicit = false;
    std::string room_name;
    std::string room_admin_token;
    std::string room_instance_id;
    uint32_t access_epoch = 0;
    std::string media_secret;
    uint8_t access_mode = ROOM_ACCESS_OPEN;
    bool auto_connect = true;
    bool auto_start_audio = true;
};
