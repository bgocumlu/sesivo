#include "audio_callback_policy.h"

#include "opus_network_clock.h"

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
    require(audio_callback_process_frame_count(120) == 120,
            "supported callback sizes must pass through unchanged");
    require(audio_callback_process_frame_count(960) == 960,
            "960 frames is the maximum supported size and must not be clamped");
    require(audio_callback_process_frame_count(961) == 960,
            "961 frames must clamp to 960");
    require(audio_callback_process_frame_count(1024) == 960,
            "1024-frame driver buffers must clamp to 960");
    require(audio_callback_process_frame_count(2048) == 960,
            "2048-frame driver buffers must clamp to 960");
    require(!audio_callback_frames_clamped(960), "960 is not clamped");
    require(audio_callback_frames_clamped(1024), "1024 is clamped");
    std::cout << "audio callback policy self-test passed\n";
    return 0;
}
