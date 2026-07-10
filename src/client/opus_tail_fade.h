#pragma once

#include <algorithm>

// When decoded PCM runs out mid-callback, the tail must never hold one sample
// flat (a DC plateau that clicks/buzzes). Instead the held sample fades
// linearly to zero over the remaining output frames of this callback.
// frames_into_tail is 1 for the first synthesized frame.
inline float opus_tail_fade_gain(unsigned long frames_into_tail,
                                 unsigned long output_frames) {
    if (output_frames == 0 || frames_into_tail >= output_frames) {
        return 0.0F;
    }
    return 1.0F - (static_cast<float>(frames_into_tail) /
                   static_cast<float>(output_frames));
}
