#include "client_audio_devices.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <system_error>

#include <spdlog/spdlog.h>

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

bool matches_audio_api(const AudioStream::DeviceInfo& device, const std::string& api_name) {
    return api_name.empty() || api_name == "All" || device.api_name == api_name;
}

}  // namespace

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

bool save_audio_device_preferences(const std::filesystem::path& path,
                                   const std::string& audio_api,
                                   AudioStream::DeviceIndex input_device,
                                   AudioStream::DeviceIndex output_device,
                                   int input_channel_index) {
    if (path.empty()) {
        return false;
    }

    const auto* input_info = AudioStream::get_device_info(input_device);
    if (input_info == nullptr) {
        return false;
    }

    const auto* output_info = AudioStream::get_device_info(output_device);
    if (output_info == nullptr) {
        return false;
    }

    std::error_code create_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), create_error);
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        spdlog::warn("Could not write audio device preferences: {}", path.string());
        return false;
    }

    output << "audio_api=" << (audio_api.empty() ? "All" : audio_api) << '\n'
           << "input_device=" << input_info->name << '\n'
           << "input_api=" << input_info->api_name << '\n'
           << "input_channel=" << input_channel_index << '\n'
           << "output_device=" << output_info->name << '\n'
           << "output_api=" << output_info->api_name << '\n';
    spdlog::info("Saved audio device preferences: {}", path.string());
    return true;
}

AudioStream::DeviceIndex find_preferred_audio_device(
    const std::vector<AudioStream::DeviceInfo>& devices,
    const std::string& preferred_device,
    const std::string& preferred_device_api,
    const std::string& preferred_filter_api) {
    if (preferred_device.empty()) {
        return AudioStream::NO_DEVICE;
    }

    auto matches_name = [&](const AudioStream::DeviceInfo& device) {
        return device.name == preferred_device;
    };

    if (!preferred_device_api.empty() && preferred_device_api != "All") {
        const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
            return matches_name(device) && matches_audio_api(device, preferred_device_api);
        });
        if (it != devices.end()) {
            return it->index;
        }
    }

    auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return matches_name(device) && matches_audio_api(device, preferred_filter_api);
    });
    if (it != devices.end()) {
        return it->index;
    }

    it = std::find_if(devices.begin(), devices.end(), matches_name);
    return it != devices.end() ? it->index : AudioStream::NO_DEVICE;
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
