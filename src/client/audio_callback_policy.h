#pragma once

#include "opus_network_clock.h"

// Runtime scratch buffers are sized for one maximum Opus frame. Device callbacks
// may be larger, so dispatch the complete callback as contiguous bounded chunks.
// The caller owns the buffers and advances channel-interleaved pointers by the
// reported offset; this helper performs no allocation or synchronization.
template <typename Callback>
inline void for_each_audio_callback_chunk(unsigned long device_frame_count,
                                          Callback&& callback) {
    constexpr auto max_chunk_frames =
        static_cast<unsigned long>(opus_network_clock::STABLE_FRAME_COUNT);
    unsigned long offset = 0;
    while (offset < device_frame_count) {
        const auto remaining = device_frame_count - offset;
        const auto chunk_frames = remaining < max_chunk_frames ? remaining : max_chunk_frames;
        callback(offset, chunk_frames);
        offset += chunk_frames;
    }
}
