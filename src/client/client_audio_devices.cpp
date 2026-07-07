#include "client_audio_devices.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <system_error>

#include <juce_core/juce_core.h>
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

juce::var empty_config_object() {
    return juce::var(new juce::DynamicObject());
}

juce::var read_config_root(const std::filesystem::path& path) {
    if (path.empty()) {
        return empty_config_object();
    }

    const juce::File file{path.string()};
    if (!file.existsAsFile()) {
        return empty_config_object();
    }

    auto root = juce::JSON::parse(file);
    return root.isObject() ? root : empty_config_object();
}

juce::DynamicObject* root_object(juce::var& root) {
    if (!root.isObject()) {
        root = empty_config_object();
    }
    return root.getDynamicObject();
}

std::string string_property(juce::DynamicObject& object, const char* key) {
    return object.getProperty(key).toString().toStdString();
}

bool write_config_root(const std::filesystem::path& path, const juce::var& root) {
    if (path.empty()) {
        return false;
    }

    std::error_code create_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), create_error);
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        spdlog::warn("Could not write client config: {}", path.string());
        return false;
    }

    output << juce::JSON::toString(root, false).toStdString() << '\n';
    return true;
}

}  // namespace

AudioDevicePreferences load_audio_device_preferences(const std::filesystem::path& path) {
    AudioDevicePreferences preferences;
    auto root = read_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        return preferences;
    }

    const auto audio_value = root_obj->getProperty("audio");
    auto* audio = audio_value.getDynamicObject();
    if (audio == nullptr) {
        return preferences;
    }
    preferences.loaded = true;
    preferences.audio_api = string_property(*audio, "api");
    if (preferences.audio_api.empty()) {
        preferences.audio_api = "All";
    }
    preferences.input_device = string_property(*audio, "inputDevice");
    preferences.input_api = string_property(*audio, "inputApi");
    const auto input_channel = audio->getProperty("inputChannel");
    if (!input_channel.isVoid()) {
        preferences.input_channel_index = std::max(0, static_cast<int>(input_channel));
    }
    preferences.output_device = string_property(*audio, "outputDevice");
    preferences.output_api = string_property(*audio, "outputApi");
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
    if (input_info == nullptr || input_info->max_input_channels <= 0) {
        return false;
    }

    const auto* output_info = AudioStream::get_device_info(output_device);
    if (output_info == nullptr || output_info->max_output_channels <= 0) {
        return false;
    }

    auto root = read_config_root(path);
    auto* object = root_object(root);

    auto* audio = new juce::DynamicObject();
    audio->setProperty("api", juce::String(audio_api.empty() ? "All" : audio_api));
    audio->setProperty("inputDevice", juce::String(input_info->name));
    audio->setProperty("inputApi", juce::String(input_info->api_name));
    audio->setProperty("inputChannel", input_channel_index);
    audio->setProperty("outputDevice", juce::String(output_info->name));
    audio->setProperty("outputApi", juce::String(output_info->api_name));
    object->setProperty("audio", juce::var(audio));

    const bool saved = write_config_root(path, root);
    if (saved) {
        spdlog::info("Saved audio device preferences: {}", path.string());
    }
    return saved;
}

std::vector<SavedRoomServer> load_saved_room_servers(const std::filesystem::path& path) {
    std::vector<SavedRoomServer> result;
    auto root = read_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        return result;
    }

    const auto servers_value = root_obj->getProperty("servers");
    const auto* servers = servers_value.getArray();
    if (servers == nullptr) {
        return result;
    }

    for (const auto& server_value: *servers) {
        auto* server_object = server_value.getDynamicObject();
        if (server_object == nullptr) {
            continue;
        }
        SavedRoomServer server;
        server.name = trim_copy(string_property(*server_object, "name"));
        server.address = trim_copy(string_property(*server_object, "address"));
        const int port = static_cast<int>(server_object->getProperty("port"));
        if (server.address.empty() || port <= 0 || port > 65535) {
            continue;
        }
        server.port = static_cast<uint16_t>(port);
        result.push_back(std::move(server));
    }
    return result;
}

bool save_saved_room_servers(const std::filesystem::path& path,
                             const std::vector<SavedRoomServer>& servers) {
    auto root = read_config_root(path);
    auto* object = root_object(root);

    juce::Array<juce::var> server_values;
    for (const auto& server: servers) {
        const auto address = trim_copy(server.address);
        if (address.empty() || server.port == 0) {
            continue;
        }
        auto* server_object = new juce::DynamicObject();
        server_object->setProperty("name", juce::String(trim_copy(server.name)));
        server_object->setProperty("address", juce::String(address));
        server_object->setProperty("port", static_cast<int>(server.port));
        server_values.add(juce::var(server_object));
    }
    object->setProperty("servers", juce::var(server_values));

    const bool saved = write_config_root(path, root);
    if (saved) {
        spdlog::info("Saved room browser servers: {}", path.string());
    }
    return saved;
}

std::string load_client_display_name(const std::filesystem::path& path) {
    auto root = read_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        return {};
    }

    const auto profile_value = root_obj->getProperty("profile");
    auto* profile = profile_value.getDynamicObject();
    if (profile == nullptr) {
        return {};
    }
    return trim_copy(string_property(*profile, "displayName"));
}

bool save_client_display_name(const std::filesystem::path& path,
                              const std::string& display_name) {
    const auto name = trim_copy(display_name);
    if (path.empty() || name.empty()) {
        return false;
    }

    auto root = read_config_root(path);
    auto* object = root_object(root);

    const auto profile_value = object->getProperty("profile");
    auto* profile = profile_value.getDynamicObject();
    if (profile == nullptr) {
        profile = new juce::DynamicObject();
        object->setProperty("profile", juce::var(profile));
    }
    profile->setProperty("displayName", juce::String(name));

    const bool saved = write_config_root(path, root);
    if (saved) {
        spdlog::info("Saved client display name: {}", path.string());
    }
    return saved;
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
