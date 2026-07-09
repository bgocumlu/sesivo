#include "client_audio_devices.h"

#include "client_config_store.h"

#include <algorithm>
#include <cctype>
#include <exception>

#include <juce_core/juce_core.h>
#include <spdlog/spdlog.h>

namespace {

constexpr const char* ROOM_SERVERS_SEEDED_KEY = "roomServersSeeded";
constexpr const char* LAST_SELECTED_SERVER_KEY = "lastSelectedServer";

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

std::string string_property(juce::DynamicObject& object, const char* key) {
    return object.getProperty(key).toString().toStdString();
}

}  // namespace

AudioDevicePreferences load_audio_device_preferences(const std::filesystem::path& path) {
    AudioDevicePreferences preferences;
    auto root = read_client_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        return preferences;
    }

    const auto audio_value = root_obj->getProperty("audio");
    auto* audio = audio_value.getDynamicObject();
    if (audio == nullptr) {
        spdlog::info("No saved audio device preferences");
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
    spdlog::info("Loaded audio device preferences: api={} input='{}' output='{}'",
                 preferences.audio_api, preferences.input_device,
                 preferences.output_device);
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

    const auto* input_info_ptr = AudioStream::get_device_info(input_device);
    if (input_info_ptr == nullptr || input_info_ptr->max_input_channels <= 0) {
        return false;
    }
    const auto input_info = *input_info_ptr;

    const auto* output_info_ptr = AudioStream::get_device_info(output_device);
    if (output_info_ptr == nullptr || output_info_ptr->max_output_channels <= 0) {
        return false;
    }
    const auto output_info = *output_info_ptr;

    const auto api = audio_api.empty() ? std::string("All") : audio_api;
    spdlog::info("Queueing audio device preference save: api={} input='{}' output='{}'",
                 api, input_info.name, output_info.name);
    return enqueue_client_config_write(
        path,
        [api, input_info, output_info, input_channel_index](juce::DynamicObject& object) {
            auto* audio = new juce::DynamicObject();
            audio->setProperty("api", juce::String(api));
            audio->setProperty("inputDevice", juce::String(input_info.name));
            audio->setProperty("inputApi", juce::String(input_info.api_name));
            audio->setProperty("inputChannel", input_channel_index);
            audio->setProperty("outputDevice", juce::String(output_info.name));
            audio->setProperty("outputApi", juce::String(output_info.api_name));
            object.setProperty("audio", juce::var(audio));
        },
        "Saved audio device preferences");
}

std::vector<SavedRoomServer> load_saved_room_servers(const std::filesystem::path& path) {
    std::vector<SavedRoomServer> result;
    auto root = read_client_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        return result;
    }

    const auto servers_value = root_obj->getProperty("servers");
    const auto* servers = servers_value.getArray();
    if (servers == nullptr) {
        spdlog::info("No saved room browser servers");
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
    spdlog::info("Loaded {} room browser server(s)", result.size());
    return result;
}

bool save_saved_room_servers(const std::filesystem::path& path,
                             const std::vector<SavedRoomServer>& servers,
                             std::optional<bool> room_servers_seeded) {
    std::vector<SavedRoomServer> clean_servers;
    clean_servers.reserve(servers.size());
    for (const auto& server: servers) {
        const auto address = trim_copy(server.address);
        if (address.empty() || server.port == 0) {
            continue;
        }
        clean_servers.push_back(
            SavedRoomServer{trim_copy(server.name), address, server.port});
    }

    spdlog::info("Queueing room browser server save: {} server(s), seeded={}",
                 clean_servers.size(),
                 room_servers_seeded.has_value()
                     ? (*room_servers_seeded ? "true" : "false")
                     : "unchanged");
    return enqueue_client_config_write(
        path,
        [servers = std::move(clean_servers),
         room_servers_seeded](juce::DynamicObject& object) {
            juce::Array<juce::var> server_values;
            for (const auto& server: servers) {
                auto* server_object = new juce::DynamicObject();
                server_object->setProperty("name", juce::String(server.name));
                server_object->setProperty("address", juce::String(server.address));
                server_object->setProperty("port", static_cast<int>(server.port));
                server_values.add(juce::var(server_object));
            }
            object.setProperty("servers", juce::var(server_values));
            if (room_servers_seeded.has_value()) {
                object.setProperty(ROOM_SERVERS_SEEDED_KEY, *room_servers_seeded);
            }
        },
        "Saved room browser servers");
}

bool load_room_servers_seeded(const std::filesystem::path& path) {
    auto root = read_client_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        spdlog::info("Room server seed flag not found");
        return false;
    }
    const bool seeded =
        static_cast<bool>(root_obj->getProperty(ROOM_SERVERS_SEEDED_KEY));
    spdlog::info("Loaded room server seed flag: {}", seeded);
    return seeded;
}

std::optional<SavedRoomServerEndpoint> load_last_selected_room_server(
    const std::filesystem::path& path) {
    auto root = read_client_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        spdlog::info("Last selected room server not found");
        return std::nullopt;
    }

    const auto selected_value = root_obj->getProperty(LAST_SELECTED_SERVER_KEY);
    auto* selected = selected_value.getDynamicObject();
    if (selected == nullptr) {
        spdlog::info("Last selected room server not found");
        return std::nullopt;
    }

    SavedRoomServerEndpoint server;
    server.address = trim_copy(string_property(*selected, "address"));
    const int port = static_cast<int>(selected->getProperty("port"));
    if (server.address.empty() || port <= 0 || port > 65535) {
        spdlog::warn("Last selected room server is invalid");
        return std::nullopt;
    }
    server.port = static_cast<uint16_t>(port);
    spdlog::info("Loaded last selected room server: {}:{}", server.address,
                 server.port);
    return server;
}

bool save_last_selected_room_server(const std::filesystem::path& path,
                                    const SavedRoomServerEndpoint& server) {
    const auto address = trim_copy(server.address);
    if (path.empty() || address.empty() || server.port == 0) {
        return false;
    }

    spdlog::info("Queueing last selected room server save: {}:{}", address,
                 server.port);
    return enqueue_client_config_write(
        path,
        [address, port = server.port](juce::DynamicObject& object) {
            auto* selected = new juce::DynamicObject();
            selected->setProperty("address", juce::String(address));
            selected->setProperty("port", static_cast<int>(port));
            object.setProperty(LAST_SELECTED_SERVER_KEY, juce::var(selected));
        },
        "Saved last selected room server");
}

std::string load_client_display_name(const std::filesystem::path& path) {
    auto root = read_client_config_root(path);
    const auto* root_obj = root.getDynamicObject();
    if (root_obj == nullptr) {
        return {};
    }

    const auto profile_value = root_obj->getProperty("profile");
    auto* profile = profile_value.getDynamicObject();
    if (profile == nullptr) {
        spdlog::info("No saved client display name");
        return {};
    }
    const auto display_name = trim_copy(string_property(*profile, "displayName"));
    spdlog::info("Loaded client display name: {}", display_name.empty() ? "<empty>" : "set");
    return display_name;
}

bool save_client_display_name(const std::filesystem::path& path,
                              const std::string& display_name) {
    const auto name = trim_copy(display_name);
    if (path.empty() || name.empty()) {
        return false;
    }

    spdlog::info("Queueing client display name save");
    return enqueue_client_config_write(
        path,
        [name](juce::DynamicObject& object) {
            const auto profile_value = object.getProperty("profile");
            auto* profile = profile_value.getDynamicObject();
            if (profile == nullptr) {
                profile = new juce::DynamicObject();
                object.setProperty("profile", juce::var(profile));
            }
            profile->setProperty("displayName", juce::String(name));
        },
        "Saved client display name");
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
