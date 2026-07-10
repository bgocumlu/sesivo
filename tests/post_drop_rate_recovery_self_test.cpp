#include "post_drop_rate_recovery.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_recovery_window_is_one_second_at_every_callback_size() {
    constexpr int sample_rate = 48000;
    const auto start = std::chrono::steady_clock::time_point(std::chrono::seconds(10));
    const auto recovery_deadline = post_drop_rate_recovery::deadline(start);

    const auto callback_120 =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(120.0 / sample_rate));
    require(post_drop_rate_recovery::active(start + callback_120 * 399,
                                            recovery_deadline, 4.0, 4.0),
            "120-frame recovery must remain active before one second");
    require(!post_drop_rate_recovery::active(start + callback_120 * 400,
                                             recovery_deadline, 4.0, 4.0),
            "120-frame recovery must end at one second");

    const auto callback_960 =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(960.0 / sample_rate));
    require(post_drop_rate_recovery::active(start + callback_960 * 49,
                                            recovery_deadline, 4.0, 4.0),
            "960-frame recovery must remain active before one second");
    require(!post_drop_rate_recovery::active(start + callback_960 * 50,
                                             recovery_deadline, 4.0, 4.0),
            "960-frame recovery must end at one second");
}

void test_recovery_keeps_the_existing_queue_threshold() {
    const auto start = std::chrono::steady_clock::time_point(std::chrono::seconds(10));
    const auto recovery_deadline = post_drop_rate_recovery::deadline(start);
    require(!post_drop_rate_recovery::active(start, recovery_deadline, 1.99, 4.0),
            "recovery must not force max speed below half the target");
    require(post_drop_rate_recovery::active(start, recovery_deadline, 2.0, 4.0),
            "recovery must force max speed at half the target");
}

}  // namespace

int main() {
    test_recovery_window_is_one_second_at_every_callback_size();
    test_recovery_keeps_the_existing_queue_threshold();
    std::cout << "post-drop rate recovery self-test passed\n";
    return 0;
}
