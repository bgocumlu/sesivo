#pragma once

#include <chrono>

namespace post_drop_rate_recovery {

constexpr auto DURATION = std::chrono::seconds(1);
constexpr auto REFERENCE_CALLBACK_DURATION = std::chrono::microseconds(2500);

inline std::chrono::steady_clock::time_point deadline(
    std::chrono::steady_clock::time_point now) {
    return now + DURATION;
}

inline bool active(std::chrono::steady_clock::time_point now,
                   std::chrono::steady_clock::time_point recovery_deadline,
                   double queued_packets, double target_packets) {
    return now < recovery_deadline && queued_packets >= target_packets * 0.5;
}

inline int remaining_reference_callbacks(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point recovery_deadline) {
    if (now >= recovery_deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        recovery_deadline - now);
    return static_cast<int>(
        (remaining.count() + REFERENCE_CALLBACK_DURATION.count() - 1) /
        REFERENCE_CALLBACK_DURATION.count());
}

}  // namespace post_drop_rate_recovery
