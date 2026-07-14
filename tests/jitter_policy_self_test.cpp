#include "jitter_policy.h"

#include "opus_network_clock.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_jitter_floor_converts_ms_using_the_senders_frame_count() {
    // A 20 ms floor must protect every sender for 20 ms of *that sender's* packets.
    require(jitter_floor_packets_for_audio(
                opus_network_clock::LOW_LATENCY_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 8,
            "20 ms must be 8 packets for a 2.5 ms (120-frame) sender");
    require(jitter_floor_packets_for_audio(
                opus_network_clock::FAST_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 4,
            "20 ms must be 4 packets for a 5 ms (240-frame) sender");
    require(jitter_floor_packets_for_audio(
                opus_network_clock::BALANCED_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 2,
            "20 ms must be 2 packets for a 10 ms (480-frame) sender");
    require(jitter_floor_packets_for_audio(
                opus_network_clock::STABLE_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 1,
            "20 ms must be 1 packet for a 20 ms (960-frame) sender");
}

void test_configured_opus_jitter_is_clamped() {
    require(jitter_floor_packets_for_audio(opus_network_clock::FAST_FRAME_COUNT, 999,
                                           opus_network_clock::SAMPLE_RATE) ==
                MAX_OPUS_JITTER_PACKETS,
            "configured Opus jitter should be clamped to the maximum");
}

void test_auto_start_jitter_uses_larger_startup_cushion() {
    require(DEFAULT_OPUS_AUTO_START_JITTER_PACKETS > DEFAULT_OPUS_JITTER_PACKETS,
            "auto-start cushion should be higher than the steady-state floor");
    require(opus_auto_start_jitter_packets(DEFAULT_OPUS_JITTER_PACKETS) ==
                DEFAULT_OPUS_AUTO_START_JITTER_PACKETS,
            "auto-start jitter should begin at the internet burst cushion");
}

void test_auto_start_jitter_respects_higher_configured_floor() {
    constexpr size_t higher_floor = DEFAULT_OPUS_AUTO_START_JITTER_PACKETS + 2;
    require(opus_auto_start_jitter_packets(higher_floor) == higher_floor,
            "auto-start jitter should not lower an explicitly higher floor");
    require(opus_auto_start_jitter_packets(999) == MAX_OPUS_JITTER_PACKETS,
            "auto-start jitter should still be clamped to the user-facing maximum");
}

void test_jitter_ms_converts_to_packets_for_supported_frame_sizes() {
    require(opus_jitter_packets_for_ms(DEFAULT_OPUS_JITTER_MS,
                                       opus_network_clock::SAMPLE_RATE,
                                       opus_network_clock::LOW_LATENCY_FRAME_COUNT) == 8,
            "20 ms jitter should be 8 packets at 2.5 ms");
    require(opus_jitter_packets_for_ms(DEFAULT_OPUS_JITTER_MS,
                                       opus_network_clock::SAMPLE_RATE,
                                       opus_network_clock::FAST_FRAME_COUNT) == 4,
            "20 ms jitter should be 4 packets at 5 ms");
    require(opus_jitter_packets_for_ms(DEFAULT_OPUS_JITTER_MS,
                                       opus_network_clock::SAMPLE_RATE,
                                       opus_network_clock::BALANCED_FRAME_COUNT) == 2,
            "20 ms jitter should be 2 packets at 10 ms");
    require(opus_jitter_packets_for_ms(DEFAULT_OPUS_JITTER_MS,
                                       opus_network_clock::SAMPLE_RATE,
                                       opus_network_clock::STABLE_FRAME_COUNT) == 1,
            "20 ms jitter should be 1 packet at 20 ms");
}

void test_jitter_age_limit_uses_floor_packet_count() {
    require(opus_jitter_packets_within_ms(85, opus_network_clock::SAMPLE_RATE,
                                          opus_network_clock::BALANCED_FRAME_COUNT) == 8,
            "85 ms age limit should allow only 8 complete 10 ms packets");
    require(opus_jitter_packets_within_ms(85, opus_network_clock::SAMPLE_RATE,
                                          opus_network_clock::STABLE_FRAME_COUNT) == 4,
            "85 ms age limit should allow only 4 complete 20 ms packets");
}

void test_zero_age_limit_disables_age_drops() {
    require(!jitter_packet_age_limit_enabled(0),
            "0 ms age limit should disable age drops");
    require(!jitter_packet_age_exceeds_limit(1, 0),
            "0 ms age limit must not treat any packet as too old");
    require(!jitter_packet_age_exceeds_limit(10 * 1000000LL, -1),
            "negative age limit should be treated as disabled before clamping");
}

void test_positive_age_limit_drops_only_older_packets() {
    require(jitter_packet_age_limit_enabled(60),
            "positive age limit should enable age drops");
    require(!jitter_packet_age_exceeds_limit(60 * 1000000LL, 60),
            "packet exactly at age limit should be accepted");
    require(jitter_packet_age_exceeds_limit((60 * 1000000LL) + 1, 60),
            "packet older than age limit should be dropped");
}

void test_auto_start_jitter_ms_converts_to_packets_for_stable_frames() {
    require(opus_auto_start_jitter_packets_for_audio(
                DEFAULT_OPUS_JITTER_PACKETS, opus_network_clock::SAMPLE_RATE,
                opus_network_clock::BALANCED_FRAME_COUNT) == 4,
            "40 ms auto-start should be 4 packets at 10 ms");
    require(opus_auto_start_jitter_packets_for_audio(
                1, opus_network_clock::SAMPLE_RATE,
                opus_network_clock::STABLE_FRAME_COUNT) == 2,
            "40 ms auto-start should be 2 packets at 20 ms");
}

void test_jitter_packet_count_round_trips_to_effective_ms() {
    require(opus_jitter_ms_for_packets(2, opus_network_clock::SAMPLE_RATE,
                                       opus_network_clock::BALANCED_FRAME_COUNT) == 20,
            "2 balanced packets should be 20 ms");
    require(opus_jitter_ms_for_packets(2, opus_network_clock::SAMPLE_RATE,
                                       opus_network_clock::STABLE_FRAME_COUNT) == 40,
            "2 stable packets should be 40 ms");
}

void test_auto_start_target_does_not_snap_to_floor_before_ready() {
    require(!jitter_target_should_snap_to_floor(
                false, true, false,
                DEFAULT_OPUS_AUTO_START_JITTER_PACKETS, DEFAULT_OPUS_JITTER_PACKETS),
            "auto jitter should preserve the startup cushion before the buffer is ready");
}

void test_non_auto_target_snaps_to_floor_before_ready() {
    require(jitter_target_should_snap_to_floor(
                false, false, false,
                DEFAULT_OPUS_AUTO_START_JITTER_PACKETS, DEFAULT_OPUS_JITTER_PACKETS),
            "non-auto jitter should snap a not-ready target back to the configured floor");
}

void test_target_raises_when_floor_increases() {
    require(jitter_target_should_snap_to_floor(
                false, true, true,
                DEFAULT_OPUS_JITTER_PACKETS, DEFAULT_OPUS_AUTO_START_JITTER_PACKETS),
            "jitter target should rise when the configured floor rises above the target");
}

void test_manual_opus_target_is_owned_by_manual_override() {
    require(!jitter_target_should_snap_to_floor(
                true, false, false,
                DEFAULT_OPUS_JITTER_PACKETS, DEFAULT_OPUS_AUTO_START_JITTER_PACKETS),
            "manual Opus jitter should not be overwritten by packet floor updates");
}

void test_latency_trim_catches_large_bursts_up_to_the_playout_target() {
    require(opus_latency_trim_goal_packets(24, 4, 8) == 4,
            "a startup burst above the high-water mark should catch up to the target");
    require(opus_latency_trim_goal_packets(8, 4, 8) == 8,
            "normal callback burst headroom should be retained at the high-water mark");
    require(opus_latency_trim_goal_packets(6, 4, 8) == 6,
            "a queue inside the burst headroom should not be trimmed");
}

void test_playout_rate_converges_a_full_packet_queue_error() {
    require(opus_playout_ratio_for_queue_depth(2.0, 2.0) == 1.0,
            "playout at the selected queue target should remain neutral");
    require(opus_playout_ratio_for_queue_depth(1.0, 2.0) < 1.0,
            "one packet below target must slow playout and rebuild the preset cushion");
    require(opus_playout_ratio_for_queue_depth(3.0, 2.0) > 1.0,
            "one packet above target must accelerate playout and remove excess latency");
    require(opus_playout_ratio_for_queue_depth(1.9, 2.0) == 1.0,
            "sub-packet measurement noise should remain inside the deadband");
    require(opus_playout_ratio_for_queue_depth(0.0, 4.0) ==
                OPUS_PLAYOUT_MIN_RATIO &&
                opus_playout_ratio_for_queue_depth(8.0, 2.0) ==
                    OPUS_PLAYOUT_MAX_RATIO,
            "large queue errors must remain inside the bounded resampling range");
}

}  // namespace

int main() {
    test_jitter_floor_converts_ms_using_the_senders_frame_count();
    test_configured_opus_jitter_is_clamped();
    test_auto_start_jitter_uses_larger_startup_cushion();
    test_auto_start_jitter_respects_higher_configured_floor();
    test_jitter_ms_converts_to_packets_for_supported_frame_sizes();
    test_jitter_age_limit_uses_floor_packet_count();
    test_zero_age_limit_disables_age_drops();
    test_positive_age_limit_drops_only_older_packets();
    test_auto_start_jitter_ms_converts_to_packets_for_stable_frames();
    test_jitter_packet_count_round_trips_to_effective_ms();
    test_auto_start_target_does_not_snap_to_floor_before_ready();
    test_non_auto_target_snaps_to_floor_before_ready();
    test_target_raises_when_floor_increases();
    test_manual_opus_target_is_owned_by_manual_override();
    test_latency_trim_catches_large_bursts_up_to_the_playout_target();
    test_playout_rate_converges_a_full_packet_queue_error();

    std::cout << "jitter policy self-test passed\n";
    return 0;
}
