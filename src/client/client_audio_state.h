#pragma once

#include "audio_stream.h"

#include <atomic>
#include <mutex>
#include <string>

struct ClientDeviceInfo {
    std::string input_device_name;
    std::string input_api;
    int         input_channels = 0;
    int         input_channel_index = 0;
    double      input_sample_rate = 0.0;
    std::string output_device_name;
    std::string output_api;
    int         output_channels = 0;
    double      output_sample_rate = 0.0;
};

struct ClientEncoderInfo {
    int channels = 0;
    int sample_rate = 0;
    int bitrate = 0;
    int actual_bitrate = 0;
    int complexity = 0;
};

class ClientAudioState {
public:
    explicit ClientAudioState(std::string audio_api_filter = "All");

    AudioStream::AudioConfig config() const;
    void publish_config(const AudioStream::AudioConfig& config);
    void set_requested_frames_per_buffer(int frames_per_buffer);
    void set_input_channel_index(int channel_index, int max_channel_count);

    int sample_rate() const;
    int frames_per_buffer() const;
    int input_channel_index() const;

    void set_input_gain(float gain);
    float input_gain() const;

    AudioStream::DeviceIndex selected_input_device() const;
    AudioStream::DeviceIndex selected_output_device() const;
    void set_selected_input_device(AudioStream::DeviceIndex device);
    void set_selected_output_device(AudioStream::DeviceIndex device);

    std::string audio_api_filter() const;
    void set_audio_api_filter(std::string api_filter);

    ClientDeviceInfo device_info() const;
    void set_input_device_info(const AudioStream::DeviceInfo& info, int input_channel_index);
    void set_output_device_info(const AudioStream::DeviceInfo& info);

    ClientEncoderInfo encoder_info() const;
    void set_encoder_info(const ClientEncoderInfo& info);

private:
    static AudioStream::AudioConfig default_config();

    mutable std::mutex config_mutex_;
    AudioStream::AudioConfig config_{};
    std::atomic<int> sample_rate_{AudioStream::AudioConfig::DEFAULT_SAMPLE_RATE};
    std::atomic<int> frames_per_buffer_{AudioStream::AudioConfig::DEFAULT_FRAMES_PER_BUFFER};
    std::atomic<float> input_gain_{1.0F};

    mutable std::mutex metadata_mutex_;
    AudioStream::DeviceIndex selected_input_device_{AudioStream::NO_DEVICE};
    AudioStream::DeviceIndex selected_output_device_{AudioStream::NO_DEVICE};
    std::string audio_api_filter_ = "All";
    ClientDeviceInfo device_info_{};
    ClientEncoderInfo encoder_info_{};
};
