#include "client_audio_state.h"

#include <algorithm>
#include <utility>

ClientAudioState::ClientAudioState(std::string audio_api_filter)
    : audio_api_filter_(audio_api_filter.empty() ? "All" : std::move(audio_api_filter)) {
    publish_config(default_config());
}

AudioStream::AudioConfig ClientAudioState::default_config() {
    AudioStream::AudioConfig config{};
    config.sample_rate       = 48000;
    config.bitrate           = AudioStream::AudioConfig::DEFAULT_BITRATE;
    config.complexity        = AudioStream::AudioConfig::DEFAULT_COMPLEXITY;
    config.frames_per_buffer = 120;
    config.input_channel_index = 0;
    config.input_gain        = 1.0F;
    config.output_gain       = 1.0F;
    return config;
}

AudioStream::AudioConfig ClientAudioState::config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void ClientAudioState::publish_config(const AudioStream::AudioConfig& config) {
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
    }
    sample_rate_.store(config.sample_rate, std::memory_order_release);
    frames_per_buffer_.store(config.frames_per_buffer, std::memory_order_release);
}

void ClientAudioState::set_requested_frames_per_buffer(int frames_per_buffer) {
    auto next_config = config();
    next_config.frames_per_buffer = frames_per_buffer;
    publish_config(next_config);
}

void ClientAudioState::set_input_channel_index(int channel_index, int max_channel_count) {
    auto next_config = config();
    const int channel_count = std::max(max_channel_count, 1);
    next_config.input_channel_index = std::clamp(channel_index, 0, channel_count - 1);
    publish_config(next_config);
}

int ClientAudioState::sample_rate() const {
    return sample_rate_.load(std::memory_order_acquire);
}

int ClientAudioState::frames_per_buffer() const {
    return frames_per_buffer_.load(std::memory_order_acquire);
}

int ClientAudioState::input_channel_index() const {
    return config().input_channel_index;
}

void ClientAudioState::set_input_gain(float gain) {
    input_gain_.store(std::clamp(gain, 0.0F, 2.0F), std::memory_order_release);
}

float ClientAudioState::input_gain() const {
    return input_gain_.load(std::memory_order_acquire);
}

AudioStream::DeviceIndex ClientAudioState::selected_input_device() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return selected_input_device_;
}

AudioStream::DeviceIndex ClientAudioState::selected_output_device() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return selected_output_device_;
}

void ClientAudioState::set_selected_input_device(AudioStream::DeviceIndex device) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    selected_input_device_ = device;
}

void ClientAudioState::set_selected_output_device(AudioStream::DeviceIndex device) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    selected_output_device_ = device;
}

std::string ClientAudioState::audio_api_filter() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return audio_api_filter_;
}

void ClientAudioState::set_audio_api_filter(std::string api_filter) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    audio_api_filter_ = api_filter.empty() ? "All" : std::move(api_filter);
}

ClientDeviceInfo ClientAudioState::device_info() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return device_info_;
}

void ClientAudioState::set_input_device_info(const AudioStream::DeviceInfo& info,
                                             int input_channel_index) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    device_info_.input_device_name = info.name;
    device_info_.input_api = info.api_name;
    device_info_.input_channels = info.max_input_channels;
    device_info_.input_channel_index = input_channel_index;
    device_info_.input_sample_rate = info.default_sample_rate;
}

void ClientAudioState::set_output_device_info(const AudioStream::DeviceInfo& info) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    device_info_.output_device_name = info.name;
    device_info_.output_api = info.api_name;
    device_info_.output_channels = info.max_output_channels >= 2 ? 2 : 1;
    device_info_.output_sample_rate = info.default_sample_rate;
}

ClientEncoderInfo ClientAudioState::encoder_info() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return encoder_info_;
}

void ClientAudioState::set_encoder_info(const ClientEncoderInfo& info) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    encoder_info_ = info;
}
