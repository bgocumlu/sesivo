#pragma once

#include <chrono>
#include <cstdint>

namespace client_network_path {

inline constexpr uint32_t PING_FEEDBACK_MIN_REPLIES = 8;
inline constexpr uint32_t PING_TIMEOUT_PROMOTE_REPLIES = 4;
inline constexpr double HIGH_RTT_MS = 250.0;
inline constexpr auto UDP_REBIND_COOLDOWN = std::chrono::seconds(3);

double gap_rate(uint32_t received_packets, uint32_t missing_packets);
double net_gap_rate(uint32_t received_packets, uint32_t sequence_gaps,
                    uint32_t unrecovered_sequence_gaps);
bool should_rebind_after_severe_loss(uint32_t received_packets,
                                     uint32_t missing_packets);
bool should_rebind_after_ping_feedback(uint32_t received_replies,
                                       uint32_t missing_replies,
                                       double rtt_ms);
bool ping_reply_is_within_watch_window(uint32_t reply_sequence,
                                       uint32_t watch_start_sequence);
uint32_t missing_replies_for_timeout(uint32_t sent_sequence,
                                     uint32_t watch_start_sequence,
                                     bool have_reply_sequence,
                                     uint32_t last_reply_sequence);

}  // namespace client_network_path
