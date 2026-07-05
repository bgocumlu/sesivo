#pragma once

#include "recording_writer.h"
#include "wav_file_playback.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct ClientRecordingState {
    bool        active = false;
    std::string folder;
    size_t      queued_blocks = 0;
    uint64_t    dropped_blocks = 0;
};

struct ClientWavState {
    bool    is_loaded = false;
    bool    is_playing = false;
    int64_t position = 0;
    int64_t total_frames = 0;
    int     sample_rate = 0;
    int     channels = 0;
    float   gain = 1.0F;
    bool    muted_local = false;
};

class ClientMediaState {
public:
    ClientRecordingState recording_state() const;
    bool start_recording(uint32_t sample_rate);
    void stop_recording();
    void set_participant_metadata(uint32_t participant_id, const std::string& profile_id,
                                  const std::string& display_name);
    void record_mono_block(RecordingWriter::TrackKind kind, uint32_t participant_id,
                           uint32_t sample_rate, const float* samples, size_t frame_count);
    void record_master_mix(uint32_t sample_rate, const float* output_buffer,
                           unsigned long frame_count, size_t out_channels);

    bool load_wav_file(const std::string& path);
    void wav_play();
    void wav_pause();
    void wav_seek(int64_t frame_position);
    void set_wav_gain(float gain);
    float wav_gain() const;
    void set_wav_muted_local(bool muted);
    bool wav_muted_local() const;
    ClientWavState wav_state() const;
    bool wav_loaded_and_playing() const;
    int read_wav(float* output, int frames_requested, int target_sample_rate);

private:
    RecordingWriter recording_writer_;
    WavFilePlayback wav_playback_;
    std::atomic<float> wav_gain_{1.0F};
    std::atomic<bool>  wav_muted_local_{false};
};
