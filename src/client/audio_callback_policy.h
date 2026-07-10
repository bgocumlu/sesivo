#pragma once

#include <algorithm>

#include "opus_network_clock.h"

// The audio callback's fixed buffers (wav_buffer, opus_input, PLC output) hold
// at most one 960-frame (20 ms @ 48 kHz) block. A driver can negotiate a larger
// actual buffer than the UI ever offers; frames beyond 960 cannot be processed
// safely and are left as already-zeroed silence.
inline unsigned long audio_callback_process_frame_count(unsigned long device_frame_count) {
    return std::min<unsigned long>(device_frame_count,
                                   opus_network_clock::STABLE_FRAME_COUNT);
}

inline bool audio_callback_frames_clamped(unsigned long device_frame_count) {
    return device_frame_count > opus_network_clock::STABLE_FRAME_COUNT;
}
