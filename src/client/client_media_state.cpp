#include "client_media_state.h"

#include <algorithm>
#include <array>

ClientRecordingState ClientMediaState::recording_state() const {
    return ClientRecordingState{
        recording_writer_.is_active(),
        recording_writer_.folder(),
        recording_writer_.queued_blocks(),
        recording_writer_.dropped_blocks(),
    };
}

bool ClientMediaState::start_recording(uint32_t sample_rate) {
    return recording_writer_.start(sample_rate);
}

void ClientMediaState::stop_recording() {
    recording_writer_.stop();
}

void ClientMediaState::set_participant_metadata(uint32_t participant_id,
                                                const std::string& profile_id,
                                                const std::string& display_name) {
    recording_writer_.set_participant_metadata(participant_id, profile_id, display_name);
}

void ClientMediaState::record_mono_block(RecordingWriter::TrackKind kind,
                                         uint32_t participant_id,
                                         uint32_t sample_rate,
                                         const float* samples,
                                         size_t frame_count) {
    recording_writer_.enqueue(kind, participant_id, sample_rate, samples, frame_count);
}

void ClientMediaState::record_master_mix(uint32_t sample_rate,
                                         const float* output_buffer,
                                         unsigned long frame_count,
                                         size_t out_channels) {
    if (!recording_writer_.is_active() || output_buffer == nullptr ||
        frame_count > RecordingWriter::MAX_FRAMES_PER_BLOCK) {
        return;
    }

    if (out_channels == 1) {
        record_mono_block(RecordingWriter::TrackKind::Master, 0, sample_rate,
                          output_buffer, frame_count);
        return;
    }

    std::array<float, RecordingWriter::MAX_FRAMES_PER_BLOCK> mono{};
    for (unsigned long frame = 0; frame < frame_count; ++frame) {
        float sum = 0.0F;
        for (size_t channel = 0; channel < out_channels; ++channel) {
            sum += output_buffer[(frame * out_channels) + channel];
        }
        mono[frame] = sum / static_cast<float>(out_channels);
    }
    record_mono_block(RecordingWriter::TrackKind::Master, 0, sample_rate,
                      mono.data(), frame_count);
}

bool ClientMediaState::load_wav_file(const std::string& path) {
    return wav_playback_.load_file(path);
}

void ClientMediaState::wav_play() {
    wav_playback_.play();
}

void ClientMediaState::wav_pause() {
    wav_playback_.pause();
}

void ClientMediaState::wav_seek(int64_t frame_position) {
    wav_playback_.seek(frame_position);
}

void ClientMediaState::set_wav_gain(float gain) {
    wav_gain_.store(std::clamp(gain, 0.0F, 2.0F), std::memory_order_release);
}

float ClientMediaState::wav_gain() const {
    return wav_gain_.load(std::memory_order_acquire);
}

void ClientMediaState::set_wav_muted_local(bool muted) {
    wav_muted_local_.store(muted, std::memory_order_release);
}

bool ClientMediaState::wav_muted_local() const {
    return wav_muted_local_.load(std::memory_order_acquire);
}

ClientWavState ClientMediaState::wav_state() const {
    ClientWavState state{};
    state.is_loaded    = wav_playback_.is_loaded();
    state.is_playing   = wav_playback_.is_playing();
    state.position     = wav_playback_.get_position();
    state.total_frames = wav_playback_.get_total_frames();
    state.sample_rate  = wav_playback_.get_sample_rate();
    state.channels     = wav_playback_.get_channels();
    state.gain         = wav_gain();
    state.muted_local  = wav_muted_local();
    return state;
}

bool ClientMediaState::wav_loaded_and_playing() const {
    return wav_playback_.is_loaded() && wav_playback_.is_playing();
}

int ClientMediaState::read_wav(float* output, int frames_requested, int target_sample_rate) {
    return wav_playback_.read(output, frames_requested, target_sample_rate);
}

size_t ClientMediaState::reap_retired_wavs() {
    return wav_playback_.reap_retired_wavs();
}
