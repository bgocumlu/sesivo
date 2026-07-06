#pragma once

#include "audio_stream.h"
#include "client_audio_state.h"
#include "client_media_state.h"
#include "client_metronome.h"
#include "participant_info.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ClientCallbackTimingInfo {
    double last_ms = 0.0;
    double max_ms = 0.0;
    double avg_ms = 0.0;
    double deadline_ms = 0.0;
    uint64_t callback_count = 0;
    uint64_t over_deadline_count = 0;
};

struct ClientPathDiagnostics {
    double rtt_last_ms = 0.0;
    double rtt_min_ms = 0.0;
    double rtt_avg_ms = 0.0;
    double rtt_max_ms = 0.0;
    uint32_t ping_received = 0;
    uint32_t ping_missing = 0;
    uint32_t ping_consecutive_missing = 0;
    double ping_gap_percent = 0.0;
    uint32_t audio_ingress_received = 0;
    uint32_t audio_ingress_gaps = 0;
    double audio_ingress_gap_percent = 0.0;
    double opus_send_queue_avg_ms = 0.0;
    double opus_send_queue_max_ms = 0.0;
    double opus_send_queue_p99_ms = 0.0;
    double total_estimate_ms = 0.0;
    double total_input_ms = 0.0;
    double total_opus_ms = 0.0;
    double total_network_ms = 0.0;
    double total_jitter_ms = 0.0;
    double total_output_ms = 0.0;
    double e2e_latency_avg_max_ms = 0.0;
    double e2e_latency_peak_ms = 0.0;
    uint64_t e2e_latency_samples = 0;
    double tx_pace_avg_ms = 0.0;
    double tx_pace_max_ms = 0.0;
    size_t rx_queue_current = 0;
    size_t rx_queue_avg_max = 0;
    size_t rx_queue_peak = 0;
    int underruns = 0;
    size_t plc_frames = 0;
    uint32_t udp_rebind_count = 0;
};

class ClientAppFacade {
public:
    using DeviceInfo = ClientDeviceInfo;
    using EncoderInfo = ClientEncoderInfo;
    using CallbackTimingInfo = ClientCallbackTimingInfo;
    using PathDiagnostics = ClientPathDiagnostics;
    using MetronomeState = ClientMetronomeState;
    using RecordingState = ClientRecordingState;
    using WavState = ClientWavState;

    virtual ~ClientAppFacade() = default;

    virtual bool start_audio_stream(AudioStream::DeviceIndex input_device,
                                    AudioStream::DeviceIndex output_device,
                                    const AudioStream::AudioConfig& config) = 0;
    virtual void stop_audio_stream() = 0;
    virtual bool is_audio_stream_active() const = 0;
    virtual bool swap_audio_devices(AudioStream::DeviceIndex input_device,
                                    AudioStream::DeviceIndex output_device) = 0;
    virtual void reset_audio_path() = 0;

    virtual void start_connection(const std::string& server_address, uint16_t server_port) = 0;
    virtual void stop_connection() = 0;
    virtual void join_room(const std::string& server_address, uint16_t server_port,
                           const std::string& room_id, const std::string& profile_id,
                           const std::string& display_name,
                           const std::string& join_token) = 0;
    virtual bool is_join_confirmed() const = 0;
    virtual bool consume_room_removed_by_server() = 0;
    virtual std::string get_server_address() const = 0;
    virtual unsigned short get_server_port() const = 0;
    virtual std::string get_room_id() const = 0;
    virtual std::vector<ParticipantInfo> get_participant_info() const = 0;
    virtual float get_own_audio_level() const = 0;
    virtual double get_rtt_ms() const = 0;
    virtual uint64_t get_total_bytes_rx() const = 0;
    virtual uint64_t get_total_bytes_tx() const = 0;

    virtual void set_mic_muted(bool muted) = 0;
    virtual bool get_mic_muted() const = 0;
    virtual void set_self_monitor_enabled(bool enabled) = 0;
    virtual bool get_self_monitor_enabled() const = 0;
    virtual void set_input_gain(float gain) = 0;
    virtual float get_input_gain() const = 0;

    virtual DeviceInfo get_device_info() const = 0;
    virtual AudioStream::LatencyInfo get_latency_info() const = 0;
    virtual AudioStream::AudioConfig get_audio_config() const = 0;
    virtual CallbackTimingInfo get_callback_timing_info() const = 0;
    virtual PathDiagnostics get_path_diagnostics() const = 0;

    virtual int get_input_channel_index() const = 0;
    virtual void set_input_channel_index(int channel_index) = 0;
    virtual void set_requested_frames_per_buffer(int frames_per_buffer) = 0;
    virtual AudioStream::DeviceIndex get_selected_input_device() const = 0;
    virtual AudioStream::DeviceIndex get_selected_output_device() const = 0;
    virtual std::string get_audio_api_filter() const = 0;
    virtual void set_audio_api_filter(std::string api_filter) = 0;
    virtual bool save_audio_device_preferences() const = 0;
    virtual bool set_input_device(AudioStream::DeviceIndex device_index) = 0;
    virtual bool set_output_device(AudioStream::DeviceIndex device_index) = 0;

    virtual uint16_t get_opus_network_frame_count() const = 0;
    virtual void set_opus_network_frame_count(int frame_count) = 0;
    virtual double get_opus_network_packet_ms() const = 0;
    virtual int get_opus_jitter_buffer_ms() const = 0;
    virtual size_t get_opus_jitter_buffer_packets() const = 0;
    virtual void set_opus_jitter_buffer_ms(int target_ms) = 0;
    virtual size_t get_opus_auto_start_jitter_packets() const = 0;
    virtual int get_opus_auto_start_jitter_ms() const = 0;
    virtual size_t get_opus_queue_limit_packets() const = 0;
    virtual void set_opus_queue_limit_packets(size_t packets) = 0;
    virtual int get_jitter_packet_age_limit_ms() const = 0;
    virtual void set_jitter_packet_age_limit_ms(int age_ms) = 0;
    virtual bool get_opus_auto_jitter_default() const = 0;
    virtual void set_opus_auto_jitter_default(bool enabled) = 0;
    virtual int get_opus_redundancy_depth_setting() const = 0;
    virtual int get_effective_opus_redundancy_depth() const = 0;
    virtual void set_opus_redundancy_depth(int depth) = 0;

    virtual void set_participant_muted(uint32_t id, bool muted) = 0;
    virtual void set_participant_gain(uint32_t id, float gain) = 0;
    virtual void set_participant_pan(uint32_t id, float pan) = 0;
    virtual void set_participant_opus_jitter_buffer_ms(uint32_t id, int target_ms) = 0;
    virtual void reset_participant_opus_jitter_buffer_packets(uint32_t id) = 0;
    virtual void set_participant_opus_auto_jitter(uint32_t id, bool enabled) = 0;

    virtual MetronomeState get_metronome_state() const = 0;
    virtual void commit_metronome_bpm(float bpm) = 0;
    virtual void start_metronome() = 0;
    virtual void stop_metronome() = 0;
    virtual void tap_metronome_tempo() = 0;

    virtual RecordingState get_recording_state() const = 0;
    virtual bool start_recording() = 0;
    virtual void stop_recording() = 0;

    virtual bool load_wav_file(const std::string& path) = 0;
    virtual void wav_play() = 0;
    virtual void wav_pause() = 0;
    virtual void wav_seek(int64_t frame_position) = 0;
    virtual void set_wav_gain(float gain) = 0;
    virtual void set_wav_muted_local(bool muted) = 0;
    virtual WavState get_wav_state() const = 0;
};
