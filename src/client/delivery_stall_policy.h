#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace delivery_stall_policy {

using Clock = std::chrono::steady_clock;

constexpr auto STALL_THRESHOLD = std::chrono::milliseconds(500);
constexpr auto BURST_WINDOW = std::chrono::milliseconds(100);

struct BurstState {
    Clock::time_point deadline{};
    size_t arrivals = 0;
};

inline bool record_arrival(BurstState& state, Clock::time_point previous_arrival,
                           Clock::time_point arrival, bool has_previous_arrival,
                           size_t jitter_target_packets) {
    if (has_previous_arrival && arrival - previous_arrival > STALL_THRESHOLD) {
        state.deadline = arrival + BURST_WINDOW;
        state.arrivals = 1;
    } else if (state.arrivals > 0 && arrival <= state.deadline) {
        ++state.arrivals;
    } else {
        state = {};
    }

    return state.arrivals > std::max<size_t>(1, jitter_target_packets);
}

}  // namespace delivery_stall_policy
