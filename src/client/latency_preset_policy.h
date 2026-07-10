#pragma once

#include "opus_network_clock.h"
#include "protocol.h"

#include <array>
#include <cstddef>

inline constexpr int LATENCY_PRESET_CUSTOM_ID = 1;
inline constexpr int LATENCY_PRESET_ULTRA_ID = 2;
inline constexpr int LATENCY_PRESET_LOW_ID = 3;
inline constexpr int LATENCY_PRESET_BALANCED_ID = 4;
inline constexpr int LATENCY_PRESET_STABLE_ID = 5;
inline constexpr int DEFAULT_LATENCY_PRESET_ID = LATENCY_PRESET_LOW_ID;

struct LatencyPreset {
    int id;
    const char* label;
    int packet_frames;
    int jitter_ms;
    int queue_limit_packets;
    int age_limit_ms;
    int redundancy_depth;
    bool auto_jitter;
};

inline constexpr std::array<LatencyPreset, 4> LATENCY_PRESETS{{
    {LATENCY_PRESET_ULTRA_ID, "Ultra", opus_network_clock::LOW_LATENCY_FRAME_COUNT, 10, 24,
     60, 1, false},
    {LATENCY_PRESET_LOW_ID, "Low", opus_network_clock::FAST_FRAME_COUNT, 15,
     static_cast<int>(DEFAULT_OPUS_QUEUE_LIMIT_PACKETS), DEFAULT_JITTER_PACKET_AGE_MS, 2,
     false},
    {LATENCY_PRESET_BALANCED_ID, "Balanced", opus_network_clock::BALANCED_FRAME_COUNT, 20,
     static_cast<int>(DEFAULT_OPUS_QUEUE_LIMIT_PACKETS), DEFAULT_JITTER_PACKET_AGE_MS, 2,
     false},
    {LATENCY_PRESET_STABLE_ID, "Stable", opus_network_clock::STABLE_FRAME_COUNT, 80, 96, 250,
     3, false},
}};

inline const LatencyPreset* latency_preset_for_id(int id) {
    for (const auto& preset: LATENCY_PRESETS) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

inline int latency_preset_id_for_settings(int packet_frames, int jitter_ms,
                                          int queue_limit_packets, int age_limit_ms,
                                          int redundancy_depth, bool auto_jitter) {
    for (const auto& preset: LATENCY_PRESETS) {
        if (preset.packet_frames == packet_frames && preset.jitter_ms == jitter_ms &&
            preset.queue_limit_packets == queue_limit_packets &&
            preset.age_limit_ms == age_limit_ms &&
            preset.redundancy_depth == redundancy_depth &&
            preset.auto_jitter == auto_jitter) {
            return preset.id;
        }
    }
    return LATENCY_PRESET_CUSTOM_ID;
}
