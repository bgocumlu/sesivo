#pragma once

#include "audio_stream.h"
#include "client_app_facade.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class JuceMixerComponent final : public juce::Component, private juce::Timer {
public:
    JuceMixerComponent(ClientAppFacade& client,
                       JuceClientStartupAudioOptions startup_audio_options);
    ~JuceMixerComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class ParticipantRowComponent;
    struct AudioDeviceRefreshResult {
        std::vector<AudioStream::DeviceInfo> input_devices;
        std::vector<AudioStream::DeviceInfo> output_devices;
        std::vector<AudioStream::ApiInfo> available_apis;
        int selected_api_index = -1;
        AudioStream::DeviceIndex pending_input = AudioStream::NO_DEVICE;
        int pending_input_channel = 0;
        AudioStream::DeviceIndex pending_output = AudioStream::NO_DEVICE;
        int pending_buffer_frames = AudioStream::AudioConfig::DEFAULT_FRAMES_PER_BUFFER;
        int pending_opus_frames_per_packet = 0;
        juce::String status;
        bool auto_start_attempted = false;
        bool auto_start_succeeded = false;
    };

    void timerCallback() override;
    void configure_controls();
    void configure_device_controls();
    void refresh_live_state();
    void refresh_participants();
    void refresh_audio_device_controls();
    void request_audio_device_refresh(bool auto_start_audio);
    AudioDeviceRefreshResult load_audio_devices(bool auto_start_audio);
    void poll_audio_device_refresh();
    void apply_audio_device_refresh_result(AudioDeviceRefreshResult result);
    void set_device_controls_enabled(bool enabled);
    void populate_api_combo();
    void populate_device_combos();
    void populate_input_channel_combo();
    void populate_buffer_combo();
    void populate_opus_packet_combo();
    void populate_redundancy_combo();
    void apply_audio_settings();
    void start_or_stop_audio();
    void reset_audio_path();
    void commit_metronome_bpm();
    void load_wav_file();
    void apply_selected_api_to_pending_devices(int old_api_index);

    juce::String selected_api_name() const;
    int api_index_for_name(const std::string& api_name) const;
    int max_input_channels_for(AudioStream::DeviceIndex device_index) const;
    AudioStream::DeviceIndex selected_input_device() const;
    AudioStream::DeviceIndex selected_output_device() const;
    bool has_pending_audio_changes() const;
    bool pending_stream_restart_needed() const;
    void set_device_status(const juce::String& text);

    ClientAppFacade& client_;
    JuceClientStartupAudioOptions startup_audio_options_;
    bool updating_from_client_ = false;
    bool device_controls_loaded_ = false;
    bool startup_device_refresh_started_ = false;
    bool device_job_running_ = false;
    bool device_job_finished_ = false;
    int device_load_delay_ticks_ = 2;
    double last_participant_refresh_ms_ = 0.0;
    size_t visible_participant_count_ = 0;
    std::mutex device_job_mutex_;
    std::thread device_job_thread_;
    std::optional<AudioDeviceRefreshResult> device_job_result_;

    std::vector<AudioStream::DeviceInfo> input_devices_;
    std::vector<AudioStream::DeviceInfo> output_devices_;
    std::vector<AudioStream::ApiInfo> available_apis_;

    int selected_api_index_ = -1;
    AudioStream::DeviceIndex pending_input_ = AudioStream::NO_DEVICE;
    int pending_input_channel_ = 0;
    AudioStream::DeviceIndex pending_output_ = AudioStream::NO_DEVICE;
    int pending_buffer_frames_ = AudioStream::AudioConfig::DEFAULT_FRAMES_PER_BUFFER;
    int pending_opus_frames_per_packet_ = 0;

    juce::Label status_label_;
    juce::Label transport_label_;
    juce::Label diagnostics_label_;
    juce::Label device_status_label_;
    juce::Label participant_header_label_;
    juce::Label empty_participants_label_;

    juce::TextButton mic_mute_button_;
    juce::ToggleButton monitor_toggle_;
    juce::Slider input_gain_slider_;
    juce::Slider jitter_ms_slider_;
    juce::Slider queue_limit_slider_;
    juce::Slider age_limit_slider_;
    juce::ToggleButton auto_jitter_toggle_;
    juce::ComboBox redundancy_combo_;

    juce::TextEditor bpm_editor_;
    juce::TextButton metronome_start_stop_button_;
    juce::TextButton metronome_tap_button_;
    juce::TextButton record_button_;

    juce::TextEditor wav_path_editor_;
    juce::TextButton wav_load_button_;
    juce::TextButton wav_play_button_;
    juce::Slider wav_position_slider_;
    juce::Slider wav_gain_slider_;
    juce::ToggleButton wav_mute_toggle_;

    juce::ComboBox api_combo_;
    juce::ComboBox input_combo_;
    juce::ComboBox input_channel_combo_;
    juce::ComboBox output_combo_;
    juce::ComboBox buffer_combo_;
    juce::ComboBox opus_packet_combo_;
    juce::TextButton apply_audio_button_;
    juce::TextButton start_stop_audio_button_;
    juce::TextButton reset_audio_button_;
    juce::TextButton refresh_devices_button_;

    juce::Viewport participants_viewport_;
    juce::Component participants_content_;
    std::vector<std::unique_ptr<ParticipantRowComponent>> participant_rows_;
};
