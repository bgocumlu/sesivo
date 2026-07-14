#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace congestion_policy {

inline constexpr int MIN_BITRATE = 32'000;
inline constexpr int DEFAULT_MAX_BITRATE = 96'000;
inline constexpr uint32_t PACKET_DURATION_IMPAIRED_INTERVALS = 2;
inline constexpr uint32_t PACKET_DURATION_HEALTHY_INTERVALS = 3;

enum class FeedbackPath : std::size_t {
    downstream,
    sender_ingress,
    ping,
    count,
};

struct PacketDurationState {
    int configured_frames = 480;
    int current_frames = 480;
    std::array<uint32_t, static_cast<std::size_t>(FeedbackPath::count)>
        impaired_intervals{};
    std::array<uint32_t, static_cast<std::size_t>(FeedbackPath::count)>
        healthy_intervals{};
    std::array<bool, static_cast<std::size_t>(FeedbackPath::count)>
        requests_larger_packets{};
};

inline void configure_packet_duration(PacketDurationState& state,
                                      int configured_frames) {
    state = {};
    state.configured_frames = configured_frames;
    state.current_frames = configured_frames;
}

inline int update_packet_duration(PacketDurationState& state,
                                  FeedbackPath path,
                                  bool impaired,
                                  int impaired_frames) {
    const auto index = static_cast<std::size_t>(path);
    if (impaired) {
        state.healthy_intervals[index] = 0;
        if (state.impaired_intervals[index] < UINT32_MAX) {
            ++state.impaired_intervals[index];
        }
        if (state.impaired_intervals[index] >=
            PACKET_DURATION_IMPAIRED_INTERVALS) {
            state.requests_larger_packets[index] = true;
        }
    } else {
        state.impaired_intervals[index] = 0;
        if (state.requests_larger_packets[index]) {
            if (state.healthy_intervals[index] < UINT32_MAX) {
                ++state.healthy_intervals[index];
            }
            if (state.healthy_intervals[index] >=
                PACKET_DURATION_HEALTHY_INTERVALS) {
                state.requests_larger_packets[index] = false;
                state.healthy_intervals[index] = 0;
            }
        } else {
            state.healthy_intervals[index] = 0;
        }
    }

    const bool larger_packets_requested = std::any_of(
        state.requests_larger_packets.begin(),
        state.requests_larger_packets.end(), [](bool requested) {
            return requested;
        });
    state.current_frames = larger_packets_requested
                               ? std::max(state.configured_frames,
                                          impaired_frames)
                               : state.configured_frames;
    return state.current_frames;
}

struct State {
    int target_bitrate = DEFAULT_MAX_BITRATE;
    int maximum_bitrate = DEFAULT_MAX_BITRATE;
    // Negative means adaptive control has not overridden the configured policy.
    int redundancy_depth = -1;
    uint32_t healthy_intervals = 0;
};

inline State update(State state, double loss_ratio, double rtt_ms) {
    loss_ratio = std::clamp(loss_ratio, 0.0, 1.0);
    state.maximum_bitrate = std::max(state.maximum_bitrate, MIN_BITRATE);
    state.target_bitrate = std::clamp(state.target_bitrate, MIN_BITRATE,
                                      state.maximum_bitrate);

    const bool severe = loss_ratio >= 0.05 || rtt_ms >= 250.0;
    const bool impaired = loss_ratio >= 0.01 || rtt_ms >= 150.0;
    if (severe) {
        state.target_bitrate = std::max(
            MIN_BITRATE, state.target_bitrate - std::max(8'000, state.target_bitrate / 4));
        state.redundancy_depth = 0;
        state.healthy_intervals = 0;
        return state;
    }
    if (impaired) {
        state.target_bitrate = std::max(MIN_BITRATE, state.target_bitrate - 8'000);
        // One duplicate is allowed only while the combined encoded offer remains
        // within the configured uncongested bitrate budget.
        state.redundancy_depth =
            loss_ratio < 0.03 && state.target_bitrate * 2 <= state.maximum_bitrate ? 1 : 0;
        state.healthy_intervals = 0;
        return state;
    }

    if (++state.healthy_intervals >= 3) {
        state.target_bitrate =
            std::min(state.maximum_bitrate, state.target_bitrate + 8'000);
        state.redundancy_depth = -1;
        state.healthy_intervals = 0;
    }
    return state;
}

}  // namespace congestion_policy
