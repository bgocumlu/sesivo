#pragma once

#include "audio_stream.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct AudioDevicePreferences {
    bool loaded = false;
    std::string audio_api = "All";
    std::string input_device;
    std::string input_api;
    std::optional<int> input_channel_index;
    std::string output_device;
    std::string output_api;
};

struct SavedRoomServer {
    std::string name;
    std::string address;
    uint16_t port = 0;
};

AudioDevicePreferences load_audio_device_preferences(const std::filesystem::path& path);
bool save_audio_device_preferences(const std::filesystem::path& path,
                                   const std::string& audio_api,
                                   AudioStream::DeviceIndex input_device,
                                   AudioStream::DeviceIndex output_device,
                                   int input_channel_index);
std::vector<SavedRoomServer> load_saved_room_servers(const std::filesystem::path& path);
bool save_saved_room_servers(const std::filesystem::path& path,
                             const std::vector<SavedRoomServer>& servers);

AudioStream::DeviceIndex find_preferred_audio_device(
    const std::vector<AudioStream::DeviceInfo>& devices,
    const std::string& preferred_device,
    const std::string& preferred_device_api,
    const std::string& preferred_filter_api);

void print_audio_backend_inventory();
AudioStream::DeviceIndex find_device_for_api(const std::string& api_name, bool input);
bool required_api_has_duplex_devices(const std::string& api_name);
