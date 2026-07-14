#include "congestion_policy.h"
#include "delivery_report_feedback.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    delivery_report_feedback::LifetimeCounter lifetime_counter;
    lifetime_counter.observe(5);
    lifetime_counter.end_media_lifetime();
    lifetime_counter.observe(3);
    require(lifetime_counter.total == 8,
            "media timeout must preserve cumulative report counters across participant recreation");

    require(delivery_report_feedback::claimed_outcomes_plausible(0, 100, 100) &&
                !delivery_report_feedback::claimed_outcomes_plausible(10, 91, 100) &&
                !delivery_report_feedback::claimed_outcomes_plausible(
                    UINT64_MAX, 1, UINT64_MAX),
            "receiver claims across epochs must consume generated media opportunities once");
    const auto two_reporter_rate = delivery_report_feedback::robust_loss_rate({
        {100, 0}, {0, 100},
    });
    require(two_reporter_rate.has_value() && *two_reporter_rate == 0.0,
            "one hostile receiver must not override one healthy receiver");
    const auto three_reporter_rate = delivery_report_feedback::robust_loss_rate({
        {100, 0}, {90, 10}, {0, 100},
    });
    require(three_reporter_rate.has_value() &&
                std::abs(*three_reporter_rate - 0.1) < 0.000001,
            "qualified receivers should have equal lower-median votes");
    const auto unequal_volume_rate = delivery_report_feedback::robust_loss_rate({
        {63, 0}, {0, 64},
    });
    require(unequal_volume_rate.has_value() && *unequal_volume_rate == 0.0,
            "one high-volume hostile receiver must not outweigh one healthy peer");
    const auto sparse_outlier_rate = delivery_report_feedback::robust_loss_rate({
        {100, 0}, {0, 1},
    });
    require(sparse_outlier_rate.has_value() && *sparse_outlier_rate == 0.0 &&
                !delivery_report_feedback::robust_loss_rate({{0, 1}}).has_value(),
            "sparse receiver samples must not influence shared congestion control");

    delivery_report_feedback::State delivery;
    auto delivery_delta =
        delivery_report_feedback::observe(delivery, 1, 10, 100, 3);
    require(delivery_delta.status ==
                delivery_report_feedback::Delta::Status::Rebaseline,
            "first delivery report should establish a non-applying warm-up baseline");
    delivery_delta =
        delivery_report_feedback::observe(delivery, 1, 11, 110, 4);
    require(delivery_delta.status ==
                delivery_report_feedback::Delta::Status::Applied &&
            delivery_delta.delivered == 10 && delivery_delta.lost == 1,
            "resolved outcome deltas should share one report interval");
    delivery_delta =
        delivery_report_feedback::observe(delivery, 1, 10, 105, 4);
    require(delivery_delta.status ==
                delivery_report_feedback::Delta::Status::Stale,
            "reordered older delivery report must not regress cumulative state");
    delivery_delta =
        delivery_report_feedback::observe(delivery, 1, 12, 120, 8);
    require(delivery_delta.status ==
                delivery_report_feedback::Delta::Status::Applied &&
            delivery_delta.delivered == 10 && delivery_delta.lost == 4,
            "network and receiver losses should share one resolved-loss union");

    delivery_report_feedback::State oversized;
    auto oversized_delta = delivery_report_feedback::observe(
        oversized, 7, 20, 10'000, 14);
    require(oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Rebaseline,
            "oversized authenticated interval should advance a non-applying baseline");
    delivery_report_feedback::State mixed_oversized;
    auto mixed_oversized_delta = delivery_report_feedback::observe(
        mixed_oversized, 9, 1, 0, 0);
    require(mixed_oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Rebaseline,
            "first empty report should establish a zero baseline");
    mixed_oversized_delta = delivery_report_feedback::observe(
        mixed_oversized, 9, 2, 700, 400);
    require(mixed_oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Rebaseline,
            "combined delivered and lost outcomes must share one interval cap");
    oversized_delta = delivery_report_feedback::observe(
        oversized, 7, 21, 10'010, 15);
    require(oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Applied &&
            oversized_delta.delivered == 10 && oversized_delta.lost == 1,
            "feedback should resume after an oversized interval rebaseline");
    oversized_delta = delivery_report_feedback::observe(
        oversized, 8, 22, 3, 1);
    require(oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Rebaseline,
            "new report epoch must not reapply its first cumulative totals");
    oversized_delta = delivery_report_feedback::observe(
        oversized, 8, 23, 5, 2);
    require(oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Applied &&
            oversized_delta.delivered == 2 && oversized_delta.lost == 1,
            "feedback should resume after a new-epoch baseline");
    oversized_delta = delivery_report_feedback::observe(
        oversized, 7, 24, 10'020, 16);
    require(oversized_delta.status ==
                delivery_report_feedback::Delta::Status::Stale,
            "delayed older epoch must not roll the current baseline backward");

    congestion_policy::State healthy_default;
    healthy_default = congestion_policy::update(healthy_default, 0.0, 40.0);
    require(healthy_default.redundancy_depth == -1,
            "healthy feedback must not override the configured redundancy policy");

    congestion_policy::State state;
    state = congestion_policy::update(state, 0.08, 80.0);
    require(state.target_bitrate < state.maximum_bitrate,
            "severe loss must lower bitrate");
    require(state.redundancy_depth == 0,
            "severe loss must not amplify congestion with redundancy");

    state.target_bitrate = 48'000;
    state = congestion_policy::update(state, 0.02, 80.0);
    require(state.target_bitrate * (state.redundancy_depth + 1) <= state.maximum_bitrate,
            "redundancy must remain inside the bitrate budget");

    const int impaired_bitrate = state.target_bitrate;
    for (int i = 0; i < 6; ++i) {
        state = congestion_policy::update(state, 0.0, 40.0);
    }
    require(state.target_bitrate > impaired_bitrate,
            "sustained healthy feedback must cautiously recover bitrate");
    require(state.target_bitrate <= state.maximum_bitrate,
            "recovery must respect configured maximum bitrate");
    require(state.redundancy_depth == -1,
            "sustained healthy feedback must restore the configured redundancy policy");

    congestion_policy::PacketDurationState packet_duration;
    congestion_policy::configure_packet_duration(packet_duration, 120);
    require(congestion_policy::update_packet_duration(
                packet_duration, congestion_policy::FeedbackPath::downstream,
                true, 240) == 120,
            "one impaired interval must not override an explicit Ultra packet size");
    require(congestion_policy::update_packet_duration(
                packet_duration, congestion_policy::FeedbackPath::downstream,
                true, 240) == 240,
            "sustained downstream impairment should increase Ultra packets to 5 ms");
    for (uint32_t index = 0;
         index < congestion_policy::PACKET_DURATION_HEALTHY_INTERVALS - 1;
         ++index) {
        require(congestion_policy::update_packet_duration(
                    packet_duration, congestion_policy::FeedbackPath::downstream,
                    false, 240) == 240,
                "packet duration recovery must require sustained healthy feedback");
    }
    require(congestion_policy::update_packet_duration(
                packet_duration, congestion_policy::FeedbackPath::downstream,
                false, 240) == 120,
            "sustained healthy feedback must restore the selected Ultra packet size");

    congestion_policy::configure_packet_duration(packet_duration, 120);
    congestion_policy::update_packet_duration(
        packet_duration, congestion_policy::FeedbackPath::downstream, true, 240);
    congestion_policy::update_packet_duration(
        packet_duration, congestion_policy::FeedbackPath::downstream, true, 240);
    for (uint32_t index = 0;
         index < congestion_policy::PACKET_DURATION_HEALTHY_INTERVALS;
         ++index) {
        require(congestion_policy::update_packet_duration(
                    packet_duration, congestion_policy::FeedbackPath::ping,
                    false, 240) == 240,
                "healthy feedback from another path must not erase downstream impairment");
    }

    congestion_policy::configure_packet_duration(packet_duration, 480);
    require(congestion_policy::update_packet_duration(
                packet_duration, congestion_policy::FeedbackPath::ping,
                true, 240) == 480,
            "congestion control must never reduce the configured packet duration");

    std::cout << "congestion policy self-test passed\n";
}
