#include "delivery_stall_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_flushes_only_after_a_stall_burst_exceeds_the_jitter_target() {
    delivery_stall_policy::BurstState state;
    const auto start = delivery_stall_policy::Clock::time_point{} +
                       std::chrono::seconds(1);

    require(!delivery_stall_policy::record_arrival(
                state, {}, start, false, 3),
            "the first packet must not look like a delivery stall");
    require(!delivery_stall_policy::record_arrival(
                state, start, start + std::chrono::milliseconds(501), true, 3),
            "the first packet after a stall must start the burst without flushing");
    require(!delivery_stall_policy::record_arrival(
                state, start + std::chrono::milliseconds(501),
                start + std::chrono::milliseconds(502), true, 3),
            "a burst no larger than the jitter target must be preserved");
    require(!delivery_stall_policy::record_arrival(
                state, start + std::chrono::milliseconds(502),
                start + std::chrono::milliseconds(503), true, 3),
            "a burst equal to the jitter target must be preserved");
    require(delivery_stall_policy::record_arrival(
                state, start + std::chrono::milliseconds(503),
                start + std::chrono::milliseconds(504), true, 3),
            "a post-stall burst larger than the jitter target must request a flush");
}

void test_normal_arrivals_and_expired_bursts_do_not_flush() {
    delivery_stall_policy::BurstState state;
    const auto start = delivery_stall_policy::Clock::time_point{} +
                       std::chrono::seconds(1);

    require(!delivery_stall_policy::record_arrival(
                state, start, start + std::chrono::milliseconds(10), true, 2),
            "normal packet spacing must not start a flush burst");
    require(!delivery_stall_policy::record_arrival(
                state, start + std::chrono::milliseconds(10),
                start + std::chrono::milliseconds(511), true, 2),
            "a delivery stall must wait for a burst larger than the target");
    require(!delivery_stall_policy::record_arrival(
                state, start + std::chrono::milliseconds(511),
                start + std::chrono::milliseconds(612), true, 2),
            "packets outside the burst window must not trigger a stale flush");
}

}  // namespace

int main() {
    test_flushes_only_after_a_stall_burst_exceeds_the_jitter_target();
    test_normal_arrivals_and_expired_bursts_do_not_flush();
    std::cout << "delivery stall policy self-test passed\n";
    return 0;
}
