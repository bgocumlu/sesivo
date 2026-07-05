#pragma once

#include "client_audio_devices.h"

#include <optional>
#include <string>

struct JuceClientStartupAudioOptions {
    AudioDevicePreferences audio_preferences;
    std::string required_audio_api;
    std::optional<int> startup_input_channel_index;
    bool auto_start_audio = true;
};
