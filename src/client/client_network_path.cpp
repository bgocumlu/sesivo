#include "client_network_path.h"

namespace client_network_path {

namespace {

constexpr uint32_t UDP_REBIND_MIN_OBSERVED_PACKETS = 8;
constexpr double UDP_REBIND_SEVERE_GAP_RATE = 0.25;

}  // namespace

double gap_rate(uint32_t received_packets, uint32_t missing_packets) {
    const uint64_t denominator =
        static_cast<uint64_t>(received_packets) + static_cast<uint64_t>(missing_packets);
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(missing_packets) / static_cast<double>(denominator);
}

double net_gap_rate(uint32_t received_packets, uint32_t sequence_gaps,
                    uint32_t unrecovered_sequence_gaps) {
    const uint64_t denominator =
        static_cast<uint64_t>(received_packets) + static_cast<uint64_t>(sequence_gaps);
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(unrecovered_sequence_gaps) /
           static_cast<double>(denominator);
}

bool should_rebind_after_severe_loss(uint32_t received_packets,
                                     uint32_t missing_packets) {
    const uint64_t observed_packets =
        static_cast<uint64_t>(received_packets) + static_cast<uint64_t>(missing_packets);
    return observed_packets >= UDP_REBIND_MIN_OBSERVED_PACKETS &&
           gap_rate(received_packets, missing_packets) >= UDP_REBIND_SEVERE_GAP_RATE;
}

bool should_rebind_after_ping_feedback(uint32_t received_replies,
                                       uint32_t missing_replies,
                                       double rtt_ms) {
    return rtt_ms >= HIGH_RTT_MS &&
           should_rebind_after_severe_loss(received_replies, missing_replies);
}

bool ping_reply_is_within_watch_window(uint32_t reply_sequence,
                                       uint32_t watch_start_sequence) {
    return reply_sequence >= watch_start_sequence;
}

uint32_t missing_replies_for_timeout(uint32_t sent_sequence,
                                     uint32_t watch_start_sequence,
                                     bool have_reply_sequence,
                                     uint32_t last_reply_sequence) {
    if (sent_sequence < watch_start_sequence) {
        return 0;
    }

    uint32_t first_missing_sequence = watch_start_sequence;
    if (have_reply_sequence && last_reply_sequence >= watch_start_sequence) {
        first_missing_sequence = last_reply_sequence + 1U;
    }
    if (sent_sequence < first_missing_sequence) {
        return 0;
    }
    return sent_sequence - first_missing_sequence + 1U;
}

}  // namespace client_network_path
