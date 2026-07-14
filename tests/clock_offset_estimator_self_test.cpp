#include "clock_offset_estimator.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    clock_offset_estimator::Estimator estimator;
    clock_offset_estimator::Estimate estimate;
    for (int i = 0; i < 4; ++i) {
        estimate = estimator.observe(20'000'000 + i * 100'000, 5'000'000 + i * 10'000);
        require(estimate.accepted, "low-delay clock samples should be accepted");
    }
    require(estimate.ready, "clock estimate requires a bounded warm-up");
    const auto before_outlier = estimate.offset_ns;
    estimate = estimator.observe(200'000'000, 80'000'000);
    require(!estimate.accepted, "high-RTT offset outlier must be rejected");
    require(std::llabs(estimate.offset_ns - before_outlier) < 100'000,
            "rejected outlier must not bias clock offset");
    require(estimate.uncertainty_ns > 0, "clock uncertainty must be retained");

    estimator.reset();
    for (int i = 0; i < 4; ++i) {
        estimate = estimator.observe(20'000'000, 5'000'000);
    }
    const int64_t stable_offset_ns = estimate.offset_ns;
    for (int i = 0; i < 8; ++i) {
        estimate = estimator.observe(80'000'000 + i * 100'000,
                                     50'000'000 + i * 10'000);
        require(!estimate.accepted,
                "short asymmetric-delay burst should be rejected");
        require(estimate.offset_ns == stable_offset_ns,
                "short delay burst must not poison the clock offset");
    }
    for (int i = 8; i < 31; ++i) {
        estimate = estimator.observe(80'000'000 + i * 100'000,
                                     15'000'000 + i * 10'000);
        require(!estimate.accepted,
                "old low-delay samples should remain authoritative in the window");
    }
    estimate = estimator.observe(83'100'000, 15'310'000);
    require(estimate.accepted,
            "clock estimator must rebase after the low-delay window ages out");
    require(estimate.ready, "rebased clock estimate should remain ready");

    estimator.reset();
    require(!estimator.observe(-1, 0).accepted, "negative RTT must be rejected");
    std::cout << "clock offset estimator self-test passed\n";
}
