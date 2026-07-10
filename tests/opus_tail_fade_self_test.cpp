#include "opus_tail_fade.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    constexpr unsigned long output_frames = 8;
    require(opus_tail_fade_gain(0, output_frames) == 1.0F,
            "the fade must start at full gain");

    float previous_gain = opus_tail_fade_gain(0, output_frames);
    for (unsigned long frame = 1; frame <= output_frames; ++frame) {
        const float gain = opus_tail_fade_gain(frame, output_frames);
        require(gain < previous_gain, "gain must strictly decrease through the fade");
        previous_gain = gain;
    }

    require(opus_tail_fade_gain(output_frames, output_frames) == 0.0F,
            "the fade must reach zero by the final output frame");
    require(opus_tail_fade_gain(output_frames + 1, output_frames) == 0.0F,
            "gain must stay at zero after the fade ends");
    require(opus_tail_fade_gain(0, 0) == 0.0F,
            "a zero-frame output must have zero gain");
    std::cout << "Opus tail fade self-test passed\n";
    return 0;
}
