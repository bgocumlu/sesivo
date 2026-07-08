#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Audio analysis utilities
namespace audio_analysis {

// Calculate RMS (Root Mean Square) audio level
inline float calculate_rms(const float* samples, size_t count) {
    if (count == 0) {
        return 0.0F;
    }

    float sum_squares = 0.0F;
    for (size_t i = 0; i < count; ++i) {
        sum_squares += samples[i] * samples[i];
    }
    return std::sqrt(sum_squares / static_cast<float>(count));
}

// Find maximum absolute sample value
inline float find_peak(const float* samples, size_t count) {
    float max_sample = 0.0F;
    for (size_t i = 0; i < count; ++i) {
        float abs_sample = std::fabs(samples[i]);
        max_sample       = std::max(abs_sample, max_sample);
    }
    return max_sample;
}

// Voice Activity Detection - simple threshold-based
inline bool detect_voice_activity(float rms_level, float threshold = 0.01F) {
    return rms_level > threshold;
}

// Silence detection - checks if audio is below threshold
inline bool is_silence(const float* samples, size_t count, float threshold = 0.001F) {
    float peak = find_peak(samples, count);
    return peak <= threshold;
}

// Mix audio samples with gain (mono)
inline void mix_with_gain(float* output, const float* input, size_t count, float gain) {
    for (size_t i = 0; i < count; ++i) {
        output[i] += input[i] * gain;
    }
}

// Mix mono input to stereo output with gain
inline void mix_mono_to_stereo(float* output, const float* input, size_t frame_count,
                               size_t out_channels, float gain) {
    for (size_t i = 0; i < frame_count; ++i) {
        float sample = input[i] * gain;
        output[(i * out_channels) + 0] += sample;  // Left
        if (out_channels > 1) {
            output[(i * out_channels) + 1] += sample;  // Right
        }
    }
}

inline void mix_local_monitor(float* output, const float* input, size_t frame_count,
                              size_t out_channels, float gain) {
    if (output == nullptr || input == nullptr || frame_count == 0 || out_channels == 0) {
        return;
    }
    if (out_channels == 1) {
        mix_with_gain(output, input, frame_count, gain);
        return;
    }
    mix_mono_to_stereo(output, input, frame_count, out_channels, gain);
}

inline float mix_normalization_target_gain(int active_source_count) {
    if (active_source_count <= 1) {
        return 1.0F;
    }
    constexpr float HEADROOM = 0.5F;
    return HEADROOM / static_cast<float>(active_source_count);
}

inline float smooth_mix_normalization_gain(float current_gain, float target_gain,
                                           size_t frame_count, double sample_rate,
                                           double smoothing_ms = 80.0) {
    current_gain = std::clamp(current_gain, 0.0F, 1.0F);
    target_gain = std::clamp(target_gain, 0.0F, 1.0F);
    if (frame_count == 0 || sample_rate <= 0.0 || smoothing_ms <= 0.0) {
        return target_gain;
    }

    const double callback_ms = static_cast<double>(frame_count) * 1000.0 / sample_rate;
    const float step = static_cast<float>(std::clamp(callback_ms / smoothing_ms, 0.0, 1.0));
    return current_gain + ((target_gain - current_gain) * step);
}

inline void apply_gain_and_hard_limit(float* samples, size_t count, float gain) {
    if (samples == nullptr || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        samples[i] = std::clamp(samples[i] * gain, -1.0F, 1.0F);
    }
}

}  // namespace audio_analysis
