#include <array>
#include <cmath>
#include <iostream>

#include "audio_analysis.h"

namespace {

bool nearly_equal(float a, float b) {
    return std::fabs(a - b) < 0.00001F;
}

bool expect_equal(float actual, float expected, const char* label) {
    if (!nearly_equal(actual, expected)) {
        std::cerr << label << ": expected " << expected << ", got " << actual << "\n";
        return false;
    }
    return true;
}

bool monitor_mixes_mono_input_to_stereo_output() {
    const std::array<float, 2> input{0.25F, -0.5F};
    std::array<float, 4> output{0.1F, 0.2F, 0.3F, 0.4F};

    audio_analysis::mix_local_monitor(output.data(), input.data(), input.size(), 2, 0.5F);

    return expect_equal(output[0], 0.225F, "left frame 0") &&
           expect_equal(output[1], 0.325F, "right frame 0") &&
           expect_equal(output[2], 0.05F, "left frame 1") &&
           expect_equal(output[3], 0.15F, "right frame 1");
}

bool monitor_mixes_mono_input_to_mono_output() {
    const std::array<float, 2> input{0.25F, -0.5F};
    std::array<float, 2> output{0.1F, 0.3F};

    audio_analysis::mix_local_monitor(output.data(), input.data(), input.size(), 1, 2.0F);

    return expect_equal(output[0], 0.6F, "mono frame 0") &&
           expect_equal(output[1], -0.7F, "mono frame 1");
}

bool normalization_target_keeps_single_source_at_unity() {
    return expect_equal(audio_analysis::mix_normalization_target_gain(0), 1.0F,
                        "zero source gain") &&
           expect_equal(audio_analysis::mix_normalization_target_gain(1), 1.0F,
                        "single source gain");
}

bool normalization_target_adds_headroom_for_multiple_sources() {
    return expect_equal(audio_analysis::mix_normalization_target_gain(2), 0.25F,
                        "two source gain") &&
           expect_equal(audio_analysis::mix_normalization_target_gain(4), 0.125F,
                        "four source gain");
}

bool normalization_gain_smooths_toward_target() {
    const float first = audio_analysis::smooth_mix_normalization_gain(
        1.0F, 0.25F, 120, 48000.0, 80.0);
    if (!(first < 1.0F && first > 0.25F)) {
        std::cerr << "smoothed gain should move toward target without stepping, got "
                  << first << "\n";
        return false;
    }

    const float immediate = audio_analysis::smooth_mix_normalization_gain(
        1.0F, 0.25F, 120, 48000.0, 0.0);
    return expect_equal(immediate, 0.25F, "zero smoothing gain");
}

bool gain_limiter_applies_gain_and_clamps() {
    std::array<float, 3> samples{2.0F, -2.0F, 0.5F};
    audio_analysis::apply_gain_and_hard_limit(samples.data(), samples.size(), 0.75F);
    return expect_equal(samples[0], 1.0F, "limited positive sample") &&
           expect_equal(samples[1], -1.0F, "limited negative sample") &&
           expect_equal(samples[2], 0.375F, "gained sample");
}

}  // namespace

int main() {
    if (!monitor_mixes_mono_input_to_stereo_output()) {
        return 1;
    }
    if (!monitor_mixes_mono_input_to_mono_output()) {
        return 1;
    }
    if (!normalization_target_keeps_single_source_at_unity()) {
        return 1;
    }
    if (!normalization_target_adds_headroom_for_multiple_sources()) {
        return 1;
    }
    if (!normalization_gain_smooths_toward_target()) {
        return 1;
    }
    if (!gain_limiter_applies_gain_and_clamps()) {
        return 1;
    }
    return 0;
}
