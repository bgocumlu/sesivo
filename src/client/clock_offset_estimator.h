#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace clock_offset_estimator {

struct Estimate {
    bool accepted = false;
    bool ready = false;
    int64_t offset_ns = 0;
    int64_t uncertainty_ns = 0;
};

class Estimator {
public:
    Estimate observe(int64_t rtt_ns, int64_t offset_ns) {
        if (rtt_ns < 0) {
            return current(false);
        }
        rtt_window_[rtt_window_next_] = rtt_ns;
        rtt_window_next_ = (rtt_window_next_ + 1) % RTT_WINDOW_SAMPLES;
        rtt_window_count_ = std::min(rtt_window_count_ + 1, RTT_WINDOW_SAMPLES);
        minimum_rtt_ns_ = *std::min_element(
            rtt_window_.begin(),
            rtt_window_.begin() + static_cast<std::ptrdiff_t>(rtt_window_count_));

        const int64_t tolerance_ns = 2'000'000;
        if (accepted_samples_ >= 4 &&
            rtt_ns > minimum_rtt_ns_ + minimum_rtt_ns_ / 2 + tolerance_ns) {
            return current(false);
        }

        if (accepted_samples_ == 0) {
            offset_ns_ = offset_ns;
            uncertainty_ns_ = rtt_ns / 2;
        } else {
            offset_ns_ = ((offset_ns_ * 7) + offset_ns) / 8;
            uncertainty_ns_ = ((uncertainty_ns_ * 7) + rtt_ns / 2) / 8;
        }
        ++accepted_samples_;
        return current(true);
    }

    void reset() {
        minimum_rtt_ns_ = std::numeric_limits<int64_t>::max();
        offset_ns_ = 0;
        uncertainty_ns_ = 0;
        accepted_samples_ = 0;
        rtt_window_.fill(std::numeric_limits<int64_t>::max());
        rtt_window_count_ = 0;
        rtt_window_next_ = 0;
    }

private:
    Estimate current(bool accepted) const {
        return {accepted, accepted_samples_ >= 4, offset_ns_, uncertainty_ns_};
    }

    int64_t minimum_rtt_ns_ = std::numeric_limits<int64_t>::max();
    int64_t offset_ns_ = 0;
    int64_t uncertainty_ns_ = 0;
    uint32_t accepted_samples_ = 0;
    static constexpr size_t RTT_WINDOW_SAMPLES = 32;
    std::array<int64_t, RTT_WINDOW_SAMPLES> rtt_window_{};
    size_t rtt_window_count_ = 0;
    size_t rtt_window_next_ = 0;
};

}  // namespace clock_offset_estimator
