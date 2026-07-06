#pragma once

#include "client_audio_devices.h"

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
    std::string room_admin_token;
    bool auto_connect = true;
    bool auto_start_audio = true;
};
