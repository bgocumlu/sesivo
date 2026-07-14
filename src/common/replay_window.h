#pragma once

#include <cstdint>

class ReplayWindow {
public:
    static constexpr uint32_t WINDOW_BITS = 64;

    bool accept(uint32_t sequence) {
        if (sequence == 0) {
            return false;
        }
        if (!initialized_) {
            initialized_ = true;
            highest_sequence_ = sequence;
            seen_ = 1;
            return true;
        }

        if (sequence_after(sequence, highest_sequence_)) {
            const uint32_t distance = sequence - highest_sequence_;
            seen_ = distance >= WINDOW_BITS ? 1 : (seen_ << distance) | 1;
            highest_sequence_ = sequence;
            return true;
        }

        const uint32_t distance = highest_sequence_ - sequence;
        if (distance >= WINDOW_BITS) {
            return false;
        }
        const uint64_t bit = uint64_t{1} << distance;
        if ((seen_ & bit) != 0) {
            return false;
        }
        seen_ |= bit;
        return true;
    }

private:
    static bool sequence_after(uint32_t lhs, uint32_t rhs) {
        return lhs != rhs && static_cast<int32_t>(lhs - rhs) > 0;
    }

    bool initialized_ = false;
    uint32_t highest_sequence_ = 0;
    uint64_t seen_ = 0;
};
