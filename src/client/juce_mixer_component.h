#pragma once

#include "audio_stream.h"
#include "client_app_facade.h"
#include "juce_participant_list_component.h"
#include "juce_status_bar_component.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class JuceMixerComponent final : public juce::Component, private juce::Timer {
public:
    JuceMixerComponent(ClientAppFacade& client,
                       JuceClientStartupOptions startup_options,
                       std::function<void()> leave_callback = {});
    ~JuceMixerComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
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
    struct ConnectionResult {
        juce::String status;
        bool started = false;
    };
    struct RoomAdminResult {
        juce::String status;
        bool ok = false;
        bool closed = false;
        bool media_key_rotated = false;
        uint32_t access_epoch = 0;
        uint8_t access_mode = ROOM_ACCESS_OPEN;
        std::string new_media_secret;
    };
    void timerCallback() override;
    void configure_controls();
    void configure_device_controls();
    void refresh_live_state();
    void refresh_audio_device_controls();
    void request_audio_device_refresh(bool auto_start_audio);
    AudioDeviceRefreshResult load_audio_devices(bool auto_start_audio);
    void poll_audio_device_refresh();
    void apply_audio_device_refresh_result(AudioDeviceRefreshResult result);
    void request_connection_start();
    ConnectionResult start_connection();
    void poll_connection_start();
    void set_device_controls_enabled(bool enabled);
    void populate_api_combo();
    void populate_device_combos();
    void populate_input_channel_combo();
    void populate_buffer_combo();
    void populate_opus_packet_combo();
    void configure_latency_preset_buttons();
    void update_latency_preset_buttons(int preset_id);
    void apply_latency_preset(int preset_id);
    int latency_preset_id_for_current_settings() const;
    void populate_redundancy_combo();
    void auto_match_buffer_to_packet_frames(int packet_frames);
    void layout_network_content();
    void set_advanced_latency_open(bool open, bool persist);
    void apply_audio_settings();
    void start_or_stop_audio();
    void reset_audio_path();
    void commit_metronome_bpm();
    void handle_record_button();
    void request_recording_destination(const juce::File& recording_folder);
    void save_recording_to_destination(const juce::File& recording_folder,
                                       const juce::File& destination_root);
    void show_recording_saved_tick();
    void load_wav_file();
    void load_wav_path(const juce::File& file);
    void refresh_room_admin_controls(const std::vector<ParticipantInfo>& participants);
    void request_room_settings_dialog();
    void request_room_chat_dialog();
    void request_room_access_change(uint8_t access_mode, std::string password);
    void request_participants_dialog();
    void request_room_approve(uint32_t participant_id);
    void request_room_decline(uint32_t participant_id);
    void request_room_kick(uint32_t participant_id);
    void request_room_close();
    void request_copy_invite();
    void start_room_admin_job(uint8_t command, uint32_t target_participant_id,
                              std::string password_hash, bool closes_room,
                              uint8_t access_mode = ROOM_ACCESS_OPEN);
    RoomAdminResult run_room_admin_command(uint8_t command,
                                           uint32_t target_participant_id,
                                           const std::string& password_hash,
                                           bool closes_room,
                                           uint8_t access_mode);
    void poll_room_admin_job();
    void apply_room_admin_result(RoomAdminResult result);
    void apply_selected_api_to_pending_devices(int old_api_index);
    void leave_room();

    juce::String selected_api_name() const;
    int api_index_for_name(const std::string& api_name) const;
    int max_input_channels_for(AudioStream::DeviceIndex device_index) const;
    AudioStream::DeviceIndex selected_input_device() const;
    AudioStream::DeviceIndex selected_output_device() const;
    bool has_room_admin() const;
    std::string password_hash(const std::string& password) const;
    juce::String invite_text() const;
    bool has_pending_audio_changes() const;
    bool pending_stream_restart_needed() const;
    void update_apply_audio_button(bool controls_enabled);
    void set_device_status(const juce::String& text);
    void set_room_admin_status(const juce::String& text);

    ClientAppFacade& client_;
    JuceClientStartupOptions startup_options_;
    std::function<void()> leave_callback_;
    bool updating_from_client_ = false;
    bool device_controls_loaded_ = false;
    bool startup_device_refresh_started_ = false;
    bool device_job_running_ = false;
    bool device_job_finished_ = false;
    bool startup_connection_started_ = false;
    bool connection_job_running_ = false;
    bool connection_job_finished_ = false;
    bool room_admin_job_running_ = false;
    bool room_admin_job_finished_ = false;
    bool advanced_latency_open_ = false;
    int device_load_delay_ticks_ = 2;
    int connection_delay_ticks_ = 1;
    double last_participant_refresh_ms_ = 0.0;
    juce::String connection_status_ = "Connection will start after the window opens...";
    juce::String room_admin_status_ = "Creator controls";
    std::atomic<uint32_t> room_admin_request_id_{1};
    std::mutex device_job_mutex_;
    std::thread device_job_thread_;
    std::optional<AudioDeviceRefreshResult> device_job_result_;
    std::mutex connection_job_mutex_;
    std::thread connection_job_thread_;
    std::optional<ConnectionResult> connection_job_result_;
    std::mutex room_admin_job_mutex_;
    std::thread room_admin_job_thread_;
    std::optional<RoomAdminResult> room_admin_job_result_;
    std::unique_ptr<juce::FileChooser> wav_file_chooser_;
    std::unique_ptr<juce::FileChooser> recording_folder_chooser_;
    juce::File last_wav_file_;
    bool recording_save_pending_ = false;
    double recording_saved_until_ms_ = 0.0;

    std::vector<AudioStream::DeviceInfo> input_devices_;
    std::vector<AudioStream::DeviceInfo> output_devices_;
    std::vector<AudioStream::ApiInfo> available_apis_;
    int selected_api_index_ = -1;
    AudioStream::DeviceIndex pending_input_ = AudioStream::NO_DEVICE;
    int pending_input_channel_ = 0;
    AudioStream::DeviceIndex pending_output_ = AudioStream::NO_DEVICE;
    int pending_buffer_frames_ = AudioStream::AudioConfig::DEFAULT_FRAMES_PER_BUFFER;
    int pending_opus_frames_per_packet_ = 0;
    std::optional<int> pending_network_age_limit_ms_;
    std::optional<int> pending_network_jitter_ms_;
    std::optional<size_t> pending_network_queue_limit_packets_;
    std::optional<bool> pending_network_auto_jitter_;
    std::optional<int> pending_network_redundancy_depth_;

    juce::Label diagnostics_label_;
    juce::Component network_content_;
    juce::Viewport network_viewport_;
    juce::Label device_status_label_;
    juce::Label local_audio_label_;
    juce::Label network_label_;
    juce::Label metronome_label_;
    juce::Label recording_label_;
    juce::Label wav_label_;
    juce::Label room_admin_label_;
    juce::Label room_admin_status_label_;
    juce::Label packet_label_;
    juce::Label jitter_label_;
    juce::Label queue_label_;
    juce::Label age_limit_label_;
    juce::Label preset_label_;
    juce::Label redundancy_label_;
    juce::Label participant_overrides_label_;
    juce::Label wav_position_label_;
    juce::Label wav_gain_label_;

    JuceStatusBarComponent status_bar_;
    JuceParticipantListComponent participants_component_;
    juce::TextButton leave_button_;
    juce::TextButton room_settings_button_;
    juce::TextButton room_participants_button_;
    juce::TextButton room_close_button_;
    juce::TextButton room_copy_invite_button_;
    juce::TextButton room_chat_button_;

    juce::TextButton mic_mute_button_;
    juce::TextButton monitor_toggle_;
    juce::Slider jitter_ms_slider_;
    juce::Slider queue_limit_slider_;
    juce::Slider age_limit_slider_;
    juce::TextButton latency_preset_ultra_button_;
    juce::TextButton latency_preset_low_button_;
    juce::TextButton latency_preset_balanced_button_;
    juce::TextButton latency_preset_stable_button_;
    juce::Label advanced_latency_label_;
    juce::ToggleButton auto_jitter_toggle_;
    juce::ComboBox redundancy_combo_;

    juce::TextEditor bpm_editor_;
    juce::TextButton metronome_start_stop_button_;
    juce::TextButton metronome_tap_button_;
    juce::TextButton record_button_;
    juce::Label recording_saved_label_;

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

};
