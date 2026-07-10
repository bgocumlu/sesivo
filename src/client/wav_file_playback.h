#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "audio_callback_context.h"

// Format parsing (can extract to class later)
namespace wav_format {
struct WavHeader {
    // RIFF chunk
    char     riff_id[4];  // "RIFF"
    uint32_t riff_size;
    char     format[4];  // "WAVE"

    // fmt chunk
    char     fmt_id[4];     // "fmt "
    uint32_t fmt_size;      // 16 for PCM
    uint16_t audio_format;  // 1 for PCM
    uint16_t num_channels;  // 1 = mono, 2 = stereo
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;  // 16

    // data chunk
    char     data_id[4];  // "data"
    uint32_t data_size;   // size of PCM data
};

// Validate that the current player supports the file format (16-bit PCM only).
inline bool validate_format(const WavHeader& header) {
    // Check RIFF/WAVE signature
    if (std::memcmp(header.riff_id, "RIFF", 4) != 0 || std::memcmp(header.format, "WAVE", 4) != 0 ||
        std::memcmp(header.fmt_id, "fmt ", 4) != 0 || std::memcmp(header.data_id, "data", 4) != 0) {
        return false;
    }

    // Only support PCM (format 1), 16-bit.
    if (header.audio_format != 1 || header.bits_per_sample != 16) {
        return false;
    }

    // Support mono and stereo
    if (header.num_channels < 1 || header.num_channels > 2) {
        return false;
    }

    return true;
}

// Parse WAV header from file
inline bool parse_header(std::ifstream& file, WavHeader& header) {
    file.seekg(0, std::ios::beg);

    // Read RIFF chunk
    file.read(reinterpret_cast<char*>(&header.riff_id), 4);
    file.read(reinterpret_cast<char*>(&header.riff_size), 4);
    file.read(reinterpret_cast<char*>(&header.format), 4);

    // Find fmt chunk (skip any chunks before it)
    char     chunk_id[4];
    uint32_t chunk_size;
    bool     found_fmt = false;

    while (file.good()) {
        file.read(chunk_id, 4);
        file.read(reinterpret_cast<char*>(&chunk_size), 4);

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            found_fmt        = true;
            header.fmt_id[0] = chunk_id[0];
            header.fmt_id[1] = chunk_id[1];
            header.fmt_id[2] = chunk_id[2];
            header.fmt_id[3] = chunk_id[3];
            header.fmt_size  = chunk_size;
            break;
        }

        // Skip this chunk
        file.seekg(chunk_size, std::ios::cur);
    }

    if (!found_fmt) {
        return false;
    }

    // Read fmt chunk data
    file.read(reinterpret_cast<char*>(&header.audio_format), 2);
    file.read(reinterpret_cast<char*>(&header.num_channels), 2);
    file.read(reinterpret_cast<char*>(&header.sample_rate), 4);
    file.read(reinterpret_cast<char*>(&header.byte_rate), 4);
    file.read(reinterpret_cast<char*>(&header.block_align), 2);
    file.read(reinterpret_cast<char*>(&header.bits_per_sample), 2);

    // Skip any extra fmt data
    if (header.fmt_size > 16) {
        file.seekg(header.fmt_size - 16, std::ios::cur);
    }

    // Find data chunk
    bool found_data = false;
    while (file.good()) {
        file.read(chunk_id, 4);
        file.read(reinterpret_cast<char*>(&chunk_size), 4);

        if (std::memcmp(chunk_id, "data", 4) == 0) {
            found_data        = true;
            header.data_id[0] = chunk_id[0];
            header.data_id[1] = chunk_id[1];
            header.data_id[2] = chunk_id[2];
            header.data_id[3] = chunk_id[3];
            header.data_size  = chunk_size;
            break;
        }

        // Skip this chunk
        file.seekg(chunk_size, std::ios::cur);
    }

    return found_data;
}
}  // namespace wav_format

// Format decoding (can extract to class later)
namespace pcm_decode {
// Convert 16-bit PCM to float [-1.0, 1.0]
inline void pcm16_to_float(const int16_t* input, float* output, int samples) {
    constexpr float scale = 1.0F / 32768.0F;
    for (int i = 0; i < samples; ++i) {
        output[i] = static_cast<float>(input[i]) * scale;
    }
}

// Convert stereo 16-bit PCM to mono float (average channels)
inline void pcm16_stereo_to_mono_float(const int16_t* input, float* output, int frames) {
    constexpr float scale = 1.0F / 32768.0F;
    for (int i = 0; i < frames; ++i) {
        int16_t left  = input[i * 2];
        int16_t right = input[(i * 2) + 1];
        output[i]     = (static_cast<float>(left) + static_cast<float>(right)) * 0.5F * scale;
    }
}
}  // namespace pcm_decode

// Sample rate conversion (can extract to class later)
namespace audio_resample {
struct LinearResult {
    double source_frames_consumed = 0.0;
    int output_frames_from_input = 0;
};

// Simple linear interpolation resampling with fractional position
// input: source audio (already float)
// output: destination buffer (must be pre-allocated)
// ratio: source_rate / target_rate
// input_frames: number of frames in input
// output_frames: number of frames to generate in output
// src_pos_start: starting fractional source position (input/output - updated on return)
inline LinearResult linear(const float* input, float* output, float ratio, int input_frames,
                           int output_frames, double& src_pos_start) {
    if (ratio == 1.0F) {
        // No resampling needed
        std::copy(input, input + output_frames, output);
        src_pos_start += static_cast<double>(output_frames);
        return {static_cast<double>(output_frames), output_frames};
    }

    const double step = static_cast<double>(ratio);
    double       src_pos = src_pos_start;
    int          output_frames_from_input = 0;

    for (int i = 0; i < output_frames; ++i) {
        int    src_idx = static_cast<int>(src_pos);
        double frac    = src_pos - static_cast<double>(src_idx);

        if (src_idx + 1 < input_frames) {
            // Linear interpolation between two samples
            output[i] =
                static_cast<float>(input[src_idx] * (1.0 - frac) + (input[src_idx + 1] * frac));
            output_frames_from_input = i + 1;
        } else if (src_idx < input_frames) {
            // Last sample (no interpolation)
            output[i] = input[src_idx];
            output_frames_from_input = i + 1;
        } else {
            // Past end of input - output silence
            output[i] = 0.0F;
        }

        src_pos += step;
    }

    // Update starting position for next call
    double frames_consumed = src_pos - src_pos_start;
    src_pos_start          = src_pos;
    return {frames_consumed, output_frames_from_input};
}
}  // namespace audio_resample

// Main WAV playback class
class WavFilePlayback {
    struct LoadedWav {
        std::vector<float> pcm_data;
        int sample_rate = 0;
        int channels = 0;
        int bits_per_sample = 0;
    };

public:
    WavFilePlayback() = default;

    ~WavFilePlayback() = default;

    // Prevent copying (contains atomic state)
    WavFilePlayback(const WavFilePlayback&)            = delete;
    WavFilePlayback& operator=(const WavFilePlayback&) = delete;

    // Allow moving (atomics initialized in body since they can't be copy-constructed)
    WavFilePlayback(WavFilePlayback&& other) noexcept
        : playing_(other.playing_.load(std::memory_order_acquire)),
          read_position_(other.read_position_.load()),
          resample_ratio_(other.resample_ratio_.load()),
          resample_position_frac_(other.resample_position_frac_.load()) {
        std::lock_guard<std::mutex> lock(other.retired_wavs_mutex_);
        loaded_wav_.store(other.loaded_wav_.exchange(nullptr, std::memory_order_acq_rel),
                          std::memory_order_release);
        retired_wavs_ = std::move(other.retired_wavs_);
        other.playing_.store(false);
        other.read_position_.store(0);
        other.resample_ratio_.store(1.0F);
        other.resample_position_frac_.store(0.0);
    }

    WavFilePlayback& operator=(WavFilePlayback&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(retired_wavs_mutex_, other.retired_wavs_mutex_);
            auto incoming = other.loaded_wav_.exchange(nullptr, std::memory_order_acq_rel);
            auto retired = loaded_wav_.exchange(std::move(incoming), std::memory_order_acq_rel);
            if (retired) {
                retired_wavs_.push_back(std::move(retired));
            }
            retired_wavs_.insert(retired_wavs_.end(),
                                 std::make_move_iterator(other.retired_wavs_.begin()),
                                 std::make_move_iterator(other.retired_wavs_.end()));
            other.retired_wavs_.clear();
            playing_.store(other.playing_.load(std::memory_order_acquire));
            read_position_.store(other.read_position_.load());
            resample_ratio_.store(other.resample_ratio_.load());
            resample_position_frac_.store(other.resample_position_frac_.load());

            other.playing_.store(false);
            other.read_position_.store(0);
            other.resample_ratio_         = 1.0F;
            other.resample_position_frac_ = 0.0F;
        }
        return *this;
    }

    // Load WAV file from path
    bool load_file(const std::string& path) {
        unload();

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            spdlog::error("Failed to open WAV file: {}", path);
            return false;
        }

        wav_format::WavHeader header;
        if (!wav_format::parse_header(file, header)) {
            spdlog::error("Failed to parse WAV header: {}", path);
            return false;
        }

        if (!wav_format::validate_format(header)) {
            spdlog::error(
                "Unsupported WAV format: format={}, channels={}, bits={} (only 16-bit PCM "
                "mono/stereo "
                "supported)",
                header.audio_format, header.num_channels, header.bits_per_sample);
            return false;
        }

        // Read PCM data
        const int total_samples = header.data_size / (header.bits_per_sample / 8);
        const int total_frames  = total_samples / header.num_channels;

        std::vector<int16_t> pcm16_data(total_samples);
        file.read(reinterpret_cast<char*>(pcm16_data.data()), header.data_size);

        if (file.gcount() != static_cast<std::streamsize>(header.data_size)) {
            spdlog::error("Failed to read all PCM data: read {} bytes, expected {}", file.gcount(),
                       header.data_size);
            return false;
        }

        // Convert to float, always output as mono.
        auto new_wav = make_loaded_wav();
        new_wav->sample_rate = static_cast<int>(header.sample_rate);
        new_wav->channels = static_cast<int>(header.num_channels);
        new_wav->bits_per_sample = static_cast<int>(header.bits_per_sample);
        new_wav->pcm_data.resize(total_frames);
        if (header.num_channels == 1) {
            pcm_decode::pcm16_to_float(pcm16_data.data(), new_wav->pcm_data.data(), total_frames);
        } else {
            // Stereo to mono conversion
            pcm_decode::pcm16_stereo_to_mono_float(pcm16_data.data(), new_wav->pcm_data.data(),
                                                   total_frames);
        }

        // Reset playback state
        playing_.store(false);
        read_position_.store(0);
        resample_ratio_.store(1.0F);
        resample_position_frac_.store(0.0);

        publish_loaded_wav(std::move(new_wav));

        spdlog::info("Loaded WAV file: {} ({}Hz, {}ch, {}bits, {} frames)", path,
                     header.sample_rate, header.num_channels, header.bits_per_sample,
                     total_frames);

        return true;
    }

    void unload() {
        playing_.store(false);
        read_position_.store(0);
        resample_ratio_.store(1.0F);
        resample_position_frac_.store(0.0);
        publish_loaded_wav(nullptr);
    }

    size_t reap_retired_wavs() {
        std::vector<std::shared_ptr<const LoadedWav>> to_destroy;
        {
            std::lock_guard<std::mutex> lock(retired_wavs_mutex_);
            for (auto it = retired_wavs_.begin(); it != retired_wavs_.end();) {
                if (it->use_count() == 1) {
                    to_destroy.push_back(std::move(*it));
                    it = retired_wavs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return to_destroy.size();
    }

    size_t retired_wav_count() const {
        std::lock_guard<std::mutex> lock(retired_wavs_mutex_);
        return retired_wavs_.size();
    }

    bool is_loaded() const {
        auto wav = loaded_wav();
        return wav != nullptr && !wav->pcm_data.empty();
    }

    void play() {
        if (is_loaded()) {
            playing_.store(true);
        }
    }

    void pause() {
        playing_.store(false);
    }

    void seek(int64_t frame_position) {
        // Only allow seeking when paused (boundary discipline)
        if (!playing_.load(std::memory_order_acquire)) {
            auto wav = loaded_wav();
            if (wav) {
                const int64_t max_pos = static_cast<int64_t>(wav->pcm_data.size());
                const int64_t clamped =
                    std::max(static_cast<int64_t>(0), std::min(frame_position, max_pos));
                read_position_.store(clamped, std::memory_order_release);
                resample_position_frac_.store(0.0,
                                              std::memory_order_release);  // Reset resampling state
            }
        }
    }

    bool is_playing() const {
        return playing_.load(std::memory_order_acquire);
    }

    int64_t get_position() const {
        return read_position_.load(std::memory_order_acquire);
    }

    int64_t get_total_frames() const {
        auto wav = loaded_wav();
        return wav ? static_cast<int64_t>(wav->pcm_data.size()) : 0;
    }

    // Read audio data (thread-safe, called from audio callback)
    // Returns number of frames read (0 = EOF, < frames_requested = partial)
    int read(float* output, int frames_requested, int target_sample_rate) {
        auto wav = loaded_wav();
        if (!wav || !playing_.load(std::memory_order_acquire)) {
            std::fill(output, output + frames_requested, 0.0F);
            return 0;
        }
        const auto& pcm = wav->pcm_data;

        // Calculate resampling ratio (source_rate / target_rate)
        const float ratio =
            static_cast<float>(wav->sample_rate) / static_cast<float>(target_sample_rate);

        // Check if ratio changed and reset state if needed
        float current_ratio = resample_ratio_.load(std::memory_order_acquire);
        if (ratio != current_ratio) {
            resample_ratio_.store(ratio, std::memory_order_release);
            resample_position_frac_.store(0.0, std::memory_order_release);
        }

        const int64_t current_pos = read_position_.load(std::memory_order_acquire);
        const int64_t max_frames  = static_cast<int64_t>(pcm.size());

        if (current_pos >= max_frames) {
            // EOF
            std::fill(output, output + frames_requested, 0.0F);
            playing_.store(false, std::memory_order_release);
            resample_position_frac_.store(0.0, std::memory_order_release);
            return 0;
        }

        if (ratio == 1.0F) {
            // No resampling needed - direct copy
            const int64_t available = max_frames - current_pos;
            const int     frames_to_copy =
                static_cast<int>(std::min(static_cast<int64_t>(frames_requested), available));

            std::copy(pcm.begin() + current_pos, pcm.begin() + current_pos + frames_to_copy,
                      output);

            if (frames_to_copy < frames_requested) {
                std::fill(output + frames_to_copy, output + frames_requested, 0.0F);
                playing_.store(false, std::memory_order_release);
            }

            read_position_.fetch_add(frames_to_copy, std::memory_order_acq_rel);
            return frames_to_copy;
        } else {
            // Resampling needed - use fractional position for continuity
            // Get fractional position (relative to read_position)
            double src_pos_frac = resample_position_frac_.load(std::memory_order_acquire);

            // Calculate how many source frames we need (with some margin for interpolation)
            const double src_frames_needed =
                src_pos_frac +
                (static_cast<double>(frames_requested) * static_cast<double>(ratio)) + 2.0;
            const int64_t available = max_frames - current_pos;
            const int     source_frames = static_cast<int>(
                std::min(static_cast<int64_t>(std::ceil(src_frames_needed)), available));

            if (source_frames == 0) {
                // EOF
                std::fill(output, output + frames_requested, 0.0F);
                playing_.store(false, std::memory_order_release);
                resample_position_frac_.store(0.0, std::memory_order_release);
                return 0;
            }

            // Get pointer to source data
            const float* source_ptr = pcm.data() + current_pos;

            // Perform resampling with fractional position continuity
            double src_pos_start   = src_pos_frac;
            auto result = audio_resample::linear(
                source_ptr, output, ratio, source_frames, frames_requested, src_pos_start);

            // Update read position (integer part of consumed frames)
            int64_t frames_advanced =
                std::min<int64_t>(available, static_cast<int64_t>(src_pos_start));
            const int64_t new_pos = current_pos + frames_advanced;
            read_position_.store(new_pos, std::memory_order_release);

            // Store fractional remainder for next call
            double fractional_remainder =
                src_pos_start - static_cast<double>(frames_advanced);
            resample_position_frac_.store(fractional_remainder, std::memory_order_release);

            // Check if we've reached EOF
            if (new_pos >= max_frames || result.output_frames_from_input < frames_requested) {
                read_position_.store(max_frames, std::memory_order_release);
                playing_.store(false, std::memory_order_release);
                resample_position_frac_.store(0.0, std::memory_order_release);
                return result.output_frames_from_input;
            }

            return frames_requested;
        }
    }

    // Metadata getters
    int get_sample_rate() const {
        auto wav = loaded_wav();
        return wav ? wav->sample_rate : 0;
    }

    int get_channels() const {
        auto wav = loaded_wav();
        return wav ? wav->channels : 0;
    }

    int get_bits_per_sample() const {
        auto wav = loaded_wav();
        return wav ? wav->bits_per_sample : 0;
    }

private:
    static std::shared_ptr<LoadedWav> make_loaded_wav() {
        return std::shared_ptr<LoadedWav>(new LoadedWav(), [](LoadedWav* wav) {
            AudioCallbackContext::assert_not_reclamation();
            delete wav;
        });
    }

    void publish_loaded_wav(std::shared_ptr<const LoadedWav> published) {
        reap_retired_wavs();
        auto retired = loaded_wav_.exchange(std::move(published), std::memory_order_acq_rel);
        if (retired) {
            std::lock_guard<std::mutex> lock(retired_wavs_mutex_);
            retired_wavs_.push_back(std::move(retired));
        }
    }

    std::shared_ptr<const LoadedWav> loaded_wav() const {
        return loaded_wav_.load(std::memory_order_acquire);
    }

    // Atomically published so the audio thread can keep an old file alive while UI loads/unloads.
    std::atomic<std::shared_ptr<const LoadedWav>> loaded_wav_;
    mutable std::mutex retired_wavs_mutex_;
    std::vector<std::shared_ptr<const LoadedWav>> retired_wavs_;

    // Playback state
    std::atomic<bool>    playing_{false};
    std::atomic<int64_t> read_position_{0};

    // Linear resampling state.
    std::atomic<float>  resample_ratio_{1.0F};
    std::atomic<double> resample_position_frac_{0.0};  // Fractional source position for continuity
};
