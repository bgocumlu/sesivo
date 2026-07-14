#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concurrentqueue.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "delivery_stall_policy.h"
#include "opus_decoder.h"
#include "pcm_delivery_outcomes.h"
#include "protocol.h"  // For AUDIO_BUF_SIZE
#include "sequence_tracker.h"

// Opus packet with metadata (for time-driven decode)
// Uses fixed buffer to avoid allocations in hot path
struct OpusPacket {
    uint16_t                              size = 0;  // Actual data size (<= AUDIO_BUF_SIZE)
    std::array<uint8_t, AUDIO_BUF_SIZE>   data;      // Fixed buffer (no allocations)
    std::chrono::steady_clock::time_point timestamp;
    AudioCodec                            codec = AudioCodec::Opus;
    uint32_t                              sequence = 0;
    bool                                  sequence_valid = false;
    bool                                  loss_concealment = false;
    bool                                  receiver_drop_pending = false;
    bool                                  reset_decoder = false;
    uint32_t                              abandoned_gap_packets = 0;
    uint32_t                              abandoned_receiver_drop_packets = 0;
    uint32_t                              sample_rate = 48000;
    uint16_t                              frame_count = 0;
    uint8_t                               channels = 1;
    bool                                  capture_timestamp_valid = false;
    int64_t                               capture_server_time_ns = 0;

    // Helper to expose the fixed payload buffer.
    const uint8_t* get_data() const {
        return data.data();
    }
    size_t get_size() const {
        return size;
    }
};

enum class ParticipantOpusDequeueStatus {
    Packet,
    Empty,
    WaitingForGap,
};

class ParticipantOpusPacketQueue {
public:
    ParticipantOpusPacketQueue() {
        unsequenced_.reserve(MAX_OPUS_QUEUE_SIZE);
        sequenced_.reserve(MAX_OPUS_QUEUE_SIZE);
    }

    bool enqueue(const OpusPacket& packet) {
        return enqueue_bounded(packet, MAX_OPUS_QUEUE_SIZE);
    }

    bool enqueue_bounded(const OpusPacket& packet, size_t max_packets) {
        if (admission_closed_.load(std::memory_order_seq_cst)) {
            return false;
        }
        active_enqueuers_.fetch_add(1, std::memory_order_seq_cst);
        if (admission_closed_.load(std::memory_order_seq_cst)) {
            active_enqueuers_.fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        const auto finish = [this](bool result) {
            active_enqueuers_.fetch_sub(1, std::memory_order_seq_cst);
            return result;
        };
        const size_t limit = admission_limit(packet, max_packets);
        if (limit == 0) {
            return finish(false);
        }

        size_t count = buffered_count_.load(std::memory_order_acquire);
        while (count < limit) {
            if (buffered_count_.compare_exchange_weak(
                    count, count + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (incoming_.enqueue(packet)) {
                    record_admitted_packet(packet);
                    return finish(true);
                }
                decrement_buffered_count();
                return finish(false);
            }
        }
        return finish(false);
    }

    bool enqueue_bounded_or_reject_overflow(const OpusPacket& packet, size_t max_packets) {
        return enqueue_bounded(packet, max_packets);
    }

    bool record_receiver_drop(const OpusPacket& packet) {
        if (!packet.sequence_valid ||
            receiver_drop_publications_closed_.load(std::memory_order_seq_cst)) {
            return false;
        }
        active_receiver_drop_publishers_.fetch_add(1, std::memory_order_seq_cst);
        if (receiver_drop_publications_closed_.load(std::memory_order_seq_cst) ||
            already_played(packet.sequence)) {
            active_receiver_drop_publishers_.fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        const bool recorded = receiver_dropped_ranges_.record(packet.sequence);
        active_receiver_drop_publishers_.fetch_sub(1, std::memory_order_seq_cst);
        return recorded;
    }

    uint64_t take_internal_receiver_drops() {
        return internal_receiver_drops_.exchange(0, std::memory_order_acq_rel);
    }

    bool try_dequeue(OpusPacket& packet) {
        return dequeue(packet, 0) == ParticipantOpusDequeueStatus::Packet;
    }

    bool try_dequeue(OpusPacket& packet, size_t gap_wait_packets) {
        return dequeue(packet, gap_wait_packets) == ParticipantOpusDequeueStatus::Packet;
    }

    ParticipantOpusDequeueStatus dequeue(OpusPacket& packet,
                                         size_t gap_wait_packets = 0) {
        drain_incoming_for_playout();
        if (!unsequenced_.empty()) {
            packet = unsequenced_.front();
            unsequenced_.erase(unsequenced_.begin());
            decrement_buffered_count();
            return ParticipantOpusDequeueStatus::Packet;
        }

        return dequeue_sequenced_for_playout(packet, gap_wait_packets);
    }

    bool discard_oldest_actual_packet() {
        drain_incoming_for_playout();
        if (!unsequenced_.empty()) {
            unsequenced_.erase(unsequenced_.begin());
            decrement_buffered_count();
            return true;
        }
        if (!sequenced_.empty()) {
            erase_sequenced_at(earliest_index_locked());
            decrement_buffered_count();
            return true;
        }
        return false;
    }

    bool discard_oldest_actual_packet_for_latency_trim(
        bool* receiver_drop_ready = nullptr) {
        drain_incoming_for_playout();
        ReceiverDropPublicationGuard publication_guard(*this);
        if (!publication_guard) {
            return false;
        }
        if (!unsequenced_.empty()) {
            unsequenced_.erase(unsequenced_.begin());
            decrement_buffered_count();
            if (receiver_drop_ready != nullptr) {
                *receiver_drop_ready = true;
            }
            return true;
        }
        if (!sequenced_.empty()) {
            const size_t index = earliest_index_locked();
            const uint32_t sequence = sequenced_[index].sequence;
            bool recorded = !playout_initialized_ || sequence == next_playout_sequence_;
            const bool ready = recorded;
            if (!recorded) {
                recorded = receiver_dropped_ranges_.record(sequence);
            }
            erase_sequenced_at(index);
            decrement_buffered_count();
            if (playout_initialized_ && sequence == next_playout_sequence_) {
                next_playout_sequence_ = sequence + 1;
                reset_gap_wait();
                publish_playout_sequence();
            }
            if (receiver_drop_ready != nullptr) {
                *receiver_drop_ready = ready;
            }
            return true;
        }
        return false;
    }

    size_t discard_all_for_local_mute() {
        drain_incoming_for_playout();
        ReceiverDropPublicationGuard publication_guard(*this);
        if (!publication_guard) {
            return 0;
        }
        const size_t discarded = unsequenced_.size() + sequenced_.size();
        unsequenced_.clear();
        sequenced_.clear();
        for (size_t i = 0; i < discarded; ++i) {
            decrement_buffered_count();
        }
        playout_initialized_ = false;
        next_playout_sequence_ = 0;
        reset_gap_wait();
        gap_loss_run_active_ = false;
        gap_loss_run_packets_ = 0;
        receiver_dropped_ranges_.clear();
        internal_receiver_drops_.store(0, std::memory_order_release);
        playout_initialized_snapshot_.store(false, std::memory_order_release);
        next_playout_sequence_snapshot_.store(0, std::memory_order_release);
        return discarded;
    }

    size_t size_approx() const {
        return buffered_count_.load(std::memory_order_acquire);
    }

    void clear() {
        admission_closed_.store(true, std::memory_order_seq_cst);
        receiver_drop_publications_closed_.store(true, std::memory_order_seq_cst);
        while (active_enqueuers_.load(std::memory_order_seq_cst) != 0 ||
               active_receiver_drop_publishers_.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
        OpusPacket packet{};
        while (incoming_.try_dequeue(packet)) {
        }
        unsequenced_.clear();
        sequenced_.clear();
        buffered_count_.store(0, std::memory_order_release);
        playout_initialized_ = false;
        next_playout_sequence_ = 0;
        reset_gap_wait();
        gap_loss_run_active_ = false;
        gap_loss_run_packets_ = 0;
        receiver_dropped_ranges_.clear();
        internal_receiver_drops_.store(0, std::memory_order_release);
        admission_tracker_ = SequenceArrivalTracker{};
        playout_initialized_snapshot_.store(false, std::memory_order_release);
        next_playout_sequence_snapshot_.store(0, std::memory_order_release);
        receiver_drop_publications_closed_.store(false, std::memory_order_seq_cst);
        admission_closed_.store(false, std::memory_order_seq_cst);
    }

private:
    static bool sequence_before(uint32_t lhs, uint32_t rhs) {
        return sequence_number_before(lhs, rhs);
    }

    bool try_close_receiver_drop_publications() {
        receiver_drop_publications_closed_.store(true, std::memory_order_seq_cst);
        if (active_receiver_drop_publishers_.load(std::memory_order_seq_cst) != 0) {
            receiver_drop_publications_closed_.store(false, std::memory_order_seq_cst);
            return false;
        }
        return true;
    }

    void reopen_receiver_drop_publications() {
        receiver_drop_publications_closed_.store(false, std::memory_order_seq_cst);
    }

    class ReceiverDropPublicationGuard {
    public:
        explicit ReceiverDropPublicationGuard(ParticipantOpusPacketQueue& owner)
            : owner_(owner), acquired_(owner_.try_close_receiver_drop_publications()) {}

        ~ReceiverDropPublicationGuard() {
            if (acquired_) {
                owner_.reopen_receiver_drop_publications();
            }
        }

        explicit operator bool() const {
            return acquired_;
        }

    private:
        ParticipantOpusPacketQueue& owner_;
        bool acquired_ = false;
    };

    void drain_incoming_for_playout() {
        OpusPacket packet{};
        size_t drained = 0;
        while (drained < MAX_OPUS_QUEUE_SIZE + 1 && incoming_.try_dequeue(packet)) {
            ++drained;
            if (!packet.sequence_valid) {
                if (unsequenced_.size() >= MAX_OPUS_QUEUE_SIZE) {
                    internal_receiver_drops_.fetch_add(1, std::memory_order_relaxed);
                    decrement_buffered_count();
                    continue;
                }
                unsequenced_.push_back(packet);
                continue;
            }

            if (playout_initialized_ &&
                sequence_before(packet.sequence, next_playout_sequence_)) {
                decrement_buffered_count();
                continue;
            }

            if (has_sequence(packet.sequence)) {
                decrement_buffered_count();
                continue;
            }

            if (sequenced_.size() >= MAX_OPUS_QUEUE_SIZE) {
                const size_t latest = latest_index_locked();
                if (sequence_before(packet.sequence, sequenced_[latest].sequence)) {
                    record_internal_receiver_drop(sequenced_[latest]);
                    sequenced_[latest] = packet;
                } else {
                    record_internal_receiver_drop(packet);
                }
                decrement_buffered_count();
                continue;
            }

            sequenced_.push_back(packet);
        }
    }

    ParticipantOpusDequeueStatus dequeue_sequenced_for_playout(
        OpusPacket& packet, size_t gap_wait_packets) {
        if (sequenced_.empty()) {
            reset_gap_wait();
            return ParticipantOpusDequeueStatus::Empty;
        }

        ReceiverDropPublicationGuard publication_guard(*this);
        if (!publication_guard) {
            return ParticipantOpusDequeueStatus::WaitingForGap;
        }

        if (!playout_initialized_) {
            next_playout_sequence_ = earliest_sequence_locked();
            playout_initialized_ = true;
            publish_playout_sequence();
        }

        discard_stale_packets_locked();

        if (sequenced_.empty()) {
            reset_gap_wait();
            return ParticipantOpusDequeueStatus::Empty;
        }

        const auto next_index = earliest_index_locked();
        const auto next_sequence = sequenced_[next_index].sequence;
        if (next_sequence != next_playout_sequence_) {
            uint32_t skipped_receiver_drops = 0;
            while (next_playout_sequence_ != next_sequence &&
                   receiver_dropped_ranges_.consume(next_playout_sequence_)) {
                ++next_playout_sequence_;
                ++skipped_receiver_drops;
            }
            if (skipped_receiver_drops > 0) {
                internal_receiver_drops_.fetch_add(
                    skipped_receiver_drops, std::memory_order_relaxed);
                publish_playout_sequence();
                reset_gap_wait();
                if (next_playout_sequence_ == next_sequence) {
                    packet = sequenced_[next_index];
                    packet.reset_decoder = true;
                    next_playout_sequence_ = packet.sequence + 1;
                    publish_playout_sequence();
                    gap_loss_run_active_ = false;
                    gap_loss_run_packets_ = 0;
                    erase_sequenced_at(next_index);
                    decrement_buffered_count();
                    return ParticipantOpusDequeueStatus::Packet;
                }
            }
            if (should_wait_for_gap(gap_wait_packets, sequenced_.size())) {
                return ParticipantOpusDequeueStatus::WaitingForGap;
            }
            if (gap_loss_run_active_ &&
                gap_loss_run_packets_ >= MAX_OPUS_CONSECUTIVE_GAP_PLC_PACKETS) {
                packet = sequenced_[next_index];
                packet.reset_decoder = true;
                const uint32_t abandoned =
                    packet.sequence - next_playout_sequence_;
                packet.abandoned_receiver_drop_packets =
                    receiver_dropped_ranges_.consume_range(
                        next_playout_sequence_, abandoned);
                packet.abandoned_gap_packets =
                    abandoned - packet.abandoned_receiver_drop_packets;
                next_playout_sequence_ = packet.sequence + 1;
                publish_playout_sequence();
                gap_loss_run_active_ = false;
                gap_loss_run_packets_ = 0;
                reset_gap_wait();
                erase_sequenced_at(next_index);
                decrement_buffered_count();
                return ParticipantOpusDequeueStatus::Packet;
            }
            packet = make_loss_concealment_packet(sequenced_[next_index],
                                                  next_playout_sequence_);
            packet.receiver_drop_pending =
                receiver_dropped_ranges_.consume(next_playout_sequence_);
            next_playout_sequence_ = packet.sequence + 1;
            publish_playout_sequence();
            gap_loss_run_active_ = true;
            ++gap_loss_run_packets_;
            reset_gap_wait();
            return ParticipantOpusDequeueStatus::Packet;
        } else {
            reset_gap_wait();
        }

        packet = sequenced_[next_index];
        next_playout_sequence_ = packet.sequence + 1;
        publish_playout_sequence();
        gap_loss_run_active_ = false;
        gap_loss_run_packets_ = 0;
        reset_gap_wait();
        erase_sequenced_at(next_index);
        decrement_buffered_count();
        return ParticipantOpusDequeueStatus::Packet;
    }

    bool has_sequence(uint32_t sequence) const {
        for (const auto& packet: sequenced_) {
            if (packet.sequence == sequence) {
                return true;
            }
        }
        return false;
    }

    static OpusPacket make_loss_concealment_packet(const OpusPacket& reference,
                                                   uint32_t sequence) {
        OpusPacket packet{};
        packet.codec = reference.codec;
        packet.sequence = sequence;
        packet.sequence_valid = true;
        packet.loss_concealment = true;
        packet.sample_rate = reference.sample_rate;
        packet.frame_count = reference.frame_count;
        packet.channels = reference.channels;
        packet.timestamp = std::chrono::steady_clock::now();
        return packet;
    }

    void record_internal_receiver_drop(const OpusPacket& packet) {
        if (!packet.sequence_valid) {
            internal_receiver_drops_.fetch_add(1, std::memory_order_relaxed);
        } else {
            (void)record_receiver_drop(packet);
        }
    }

    bool already_played(uint32_t sequence) const {
        return playout_initialized_snapshot_.load(std::memory_order_acquire) &&
               sequence_before(
                   sequence,
                   next_playout_sequence_snapshot_.load(std::memory_order_acquire));
    }

    size_t admission_limit(const OpusPacket& packet, size_t max_packets) const {
        const size_t configured_limit = std::min(max_packets, MAX_OPUS_QUEUE_SIZE);
        if (configured_limit == 0) {
            return 0;
        }

        if (is_expected_late_packet(packet) || is_startup_gap_recovery_packet(packet)) {
            return configured_limit + 1;
        }
        return configured_limit;
    }

    bool is_expected_late_packet(const OpusPacket& packet) const {
        if (!packet.sequence_valid ||
            !playout_initialized_snapshot_.load(std::memory_order_acquire)) {
            return false;
        }
        return packet.sequence ==
               next_playout_sequence_snapshot_.load(std::memory_order_acquire);
    }

    bool is_startup_gap_recovery_packet(const OpusPacket& packet) const {
        return packet.sequence_valid &&
               !playout_initialized_snapshot_.load(std::memory_order_acquire) &&
               admission_tracker_.has_unresolved_gap(packet.sequence);
    }

    void record_admitted_packet(const OpusPacket& packet) {
        if (packet.sequence_valid) {
            admission_tracker_.record(packet.sequence);
        }
    }

    size_t earliest_index_locked() const {
        size_t earliest = 0;
        for (size_t i = 1; i < sequenced_.size(); ++i) {
            if (sequence_before(sequenced_[i].sequence, sequenced_[earliest].sequence)) {
                earliest = i;
            }
        }
        return earliest;
    }

    size_t latest_index_locked() const {
        size_t latest = 0;
        for (size_t i = 1; i < sequenced_.size(); ++i) {
            if (sequence_before(sequenced_[latest].sequence, sequenced_[i].sequence)) {
                latest = i;
            }
        }
        return latest;
    }

    uint32_t earliest_sequence_locked() const {
        return sequenced_[earliest_index_locked()].sequence;
    }

    void discard_stale_packets_locked() {
        for (size_t i = 0; i < sequenced_.size();) {
            if (sequence_before(sequenced_[i].sequence, next_playout_sequence_)) {
                erase_sequenced_at(i);
                decrement_buffered_count();
            } else {
                ++i;
            }
        }
    }

    void erase_sequenced_at(size_t index) {
        if (index + 1 < sequenced_.size()) {
            sequenced_[index] = sequenced_.back();
        }
        sequenced_.pop_back();
    }

    bool should_wait_for_gap(size_t gap_wait_packets, size_t /*future_packet_count*/) {
        if (gap_wait_packets == 0) {
            return false;
        }

        if (gap_loss_run_active_) {
            return false;
        }

        if (!gap_wait_initialized_ || gap_wait_sequence_ != next_playout_sequence_) {
            gap_wait_initialized_ = true;
            gap_wait_sequence_ = next_playout_sequence_;
            gap_wait_callbacks_ = 0;
        }

        if (gap_wait_callbacks_ >= gap_wait_packets) {
            return false;
        }

        ++gap_wait_callbacks_;
        return true;
    }

    void reset_gap_wait() {
        gap_wait_initialized_ = false;
        gap_wait_callbacks_ = 0;
    }

    void publish_playout_sequence() {
        next_playout_sequence_snapshot_.store(next_playout_sequence_,
                                             std::memory_order_release);
        playout_initialized_snapshot_.store(playout_initialized_,
                                            std::memory_order_release);
    }

    void decrement_buffered_count() {
        buffered_count_.fetch_sub(1, std::memory_order_release);
    }

    moodycamel::ConcurrentQueue<OpusPacket> incoming_;
    std::atomic<size_t> buffered_count_{0};
    std::atomic<bool> admission_closed_{false};
    std::atomic<uint32_t> active_enqueuers_{0};
    std::atomic<bool> receiver_drop_publications_closed_{false};
    std::atomic<uint32_t> active_receiver_drop_publishers_{0};
    std::vector<OpusPacket> unsequenced_;
    std::vector<OpusPacket> sequenced_;
    class ReceiverDroppedRanges {
    public:
        bool record(uint32_t sequence) {
            for (auto& slot: slots_) {
                uint64_t packed = slot.load(std::memory_order_acquire);
                while (packed != 0) {
                    const uint32_t first = static_cast<uint32_t>(packed >> 32U);
                    const uint32_t count = static_cast<uint32_t>(packed);
                    const uint32_t offset = sequence - first;
                    if (offset < count) {
                        return true;
                    }
                    uint64_t replacement = 0;
                    if (offset == count && count != UINT32_MAX) {
                        replacement = pack(first, count + 1);
                    } else if (first - sequence == 1 && count != UINT32_MAX) {
                        replacement = pack(sequence, count + 1);
                    } else {
                        break;
                    }
                    if (slot.compare_exchange_weak(
                            packed, replacement, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        return true;
                    }
                }
            }
            for (auto& slot: slots_) {
                uint64_t empty = 0;
                if (slot.compare_exchange_strong(
                        empty, pack(sequence, 1), std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return true;
                }
            }
            slots_[sequence % slots_.size()].store(
                pack(sequence, 1), std::memory_order_release);
            return true;
        }

        bool consume(uint32_t sequence) {
            for (auto& slot: slots_) {
                uint64_t packed = slot.load(std::memory_order_acquire);
                while (packed != 0) {
                    const uint32_t first = static_cast<uint32_t>(packed >> 32U);
                    const uint32_t count = static_cast<uint32_t>(packed);
                    const uint32_t offset = sequence - first;
                    if (offset >= count) {
                        break;
                    }
                    uint64_t replacement = packed;
                    if (count == 1) {
                        replacement = 0;
                    } else if (offset == 0) {
                        replacement = pack(first + 1, count - 1);
                    } else if (offset == count - 1) {
                        replacement = pack(first, count - 1);
                    } else {
                        // A fixed slot cannot split into two ranges. Dropping the
                        // rest is safe because none of its outcomes were reported;
                        // those sequences will resolve as ordinary gaps instead.
                        replacement = 0;
                    }
                    if (slot.compare_exchange_weak(
                            packed, replacement, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        return true;
                    }
                }
            }
            return false;
        }

        uint32_t consume_range(uint32_t first, uint32_t count) {
            uint32_t consumed = 0;
            for (auto& slot: slots_) {
                uint64_t packed = slot.load(std::memory_order_acquire);
                while (packed != 0) {
                    const uint32_t range_first = static_cast<uint32_t>(packed >> 32U);
                    const uint32_t range_count = static_cast<uint32_t>(packed);
                    const uint32_t start = range_first - first;
                    if (start >= count) {
                        break;
                    }
                    const uint32_t overlap = std::min(range_count, count - start);
                    const uint32_t remaining = range_count - overlap;
                    const uint64_t replacement =
                        remaining == 0
                            ? 0
                            : pack(range_first + overlap, remaining);
                    if (slot.compare_exchange_weak(
                            packed, replacement, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        consumed += overlap;
                        break;
                    }
                }
            }
            return consumed;
        }

        void clear() {
            for (auto& slot: slots_) {
                slot.store(0, std::memory_order_release);
            }
        }

    private:
        static uint64_t pack(uint32_t first, uint32_t count) {
            return (static_cast<uint64_t>(first) << 32U) | count;
        }

        static constexpr size_t MAX_RECEIVER_DROP_RANGES = 64;
        std::array<std::atomic<uint64_t>, MAX_RECEIVER_DROP_RANGES> slots_{};
    } receiver_dropped_ranges_;
    std::atomic<uint64_t> internal_receiver_drops_{0};
    bool playout_initialized_ = false;
    uint32_t next_playout_sequence_ = 0;
    bool gap_wait_initialized_ = false;
    uint32_t gap_wait_sequence_ = 0;
    size_t gap_wait_callbacks_ = 0;
    bool gap_loss_run_active_ = false;
    size_t gap_loss_run_packets_ = 0;
    SequenceArrivalTracker admission_tracker_;
    std::atomic<bool> playout_initialized_snapshot_{false};
    std::atomic<uint32_t> next_playout_sequence_snapshot_{0};
};

// Per-participant audio data and state
struct ParticipantData {
    // Audio processing - store OPUS packets, decode in audio callback
    ParticipantOpusPacketQueue              opus_queue;
    std::unique_ptr<OpusDecoderWrapper>     decoder;
    std::array<float, 960>                  pcm_buffer;  // Preallocated decode buffer
    std::array<float, 1920>                 opus_pcm_buffer{};
    size_t                                  opus_pcm_buffered_frames = 0;
    PcmDeliveryOutcomes                     opus_pcm_outcomes;
    double                                  opus_resample_phase = 0.0;
    uint64_t                                opus_rate_last_queue_limit_drops = 0;
    std::chrono::steady_clock::time_point   opus_rate_correction_deadline{};
    std::atomic<int64_t>                    opus_playout_rate_ratio_micros{1'000'000};
    std::atomic<int>                        opus_rate_correction_callbacks_observed{0};
    std::atomic<size_t>                     opus_pcm_buffered_frames_observed{0};
    std::atomic<uint64_t>                   opus_packets_decoded_in_callback{0};
    std::atomic<uint64_t>                   opus_queue_limit_drops{0};
    std::atomic<uint64_t>                   opus_age_limit_drops{0};
    std::atomic<uint64_t>                   opus_decode_buffer_overflow_drops{0};
    std::atomic<uint64_t>                   opus_target_trim_drops{0};
    std::atomic<uint64_t>                   receiver_delivery_delivered{0};
    std::atomic<uint64_t>                   receiver_delivery_drops{0};
    std::atomic<size_t>                     last_packet_frame_count{0};
    std::atomic<size_t>                     last_callback_frame_count{0};
    // Participant state
    std::string                           profile_id;
    std::string                           display_name;
    Bytes<E2E_PUBLIC_KEY_BYTES>           key_public{};
    bool                                  has_key_public = false;
    std::atomic<bool>                    is_muted{false};
    std::atomic<float>                   gain{1.0F};
    std::atomic<float>                   pan{0.5F};  // 0.0 = full left, 0.5 = center, 1.0 = full right
    std::chrono::steady_clock::time_point last_packet_time;
    delivery_stall_policy::BurstState     delivery_stall_burst;
    std::atomic<bool>                     delivery_stall_flush_requested{false};
    std::atomic<size_t>                   jitter_buffer_floor_packets{MIN_JITTER_BUFFER_PACKETS};
    std::atomic<size_t>                   jitter_buffer_min_packets{MIN_JITTER_BUFFER_PACKETS};
    std::atomic<size_t>                   opus_queue_limit_packets{MAX_OPUS_QUEUE_SIZE};
    std::atomic<bool>                     opus_jitter_manual_override{false};
    std::atomic<int>                      opus_jitter_override_ms{0};
    std::atomic<bool>                     opus_jitter_auto_enabled{false};
    std::atomic<size_t>                   opus_jitter_auto_floor_packets{DEFAULT_OPUS_JITTER_PACKETS};
    std::atomic<int>                      opus_jitter_auto_stable_callbacks{0};
    std::atomic<int>                      opus_jitter_auto_instability_events{0};
    std::atomic<uint64_t>                 opus_jitter_auto_increases{0};
    std::atomic<uint64_t>                 opus_jitter_auto_decreases{0};
    std::atomic<bool>                     buffer_ready{false};
    std::atomic<int>                      opus_consecutive_empty_callbacks{0};
    std::atomic<int>                      underrun_count{0};
    std::atomic<float>                   current_level{0.0F};  // RMS audio level
    std::atomic<float>                   current_peak{0.0F};
    std::atomic<bool>                    is_speaking{false};  // Voice activity detection

    // Adaptive jitter buffer tracking
    std::array<size_t, 8> queue_size_history = {};  // Rolling history for adaptive buffer
    size_t                history_index      = 0;   // Current index in history
    std::atomic<size_t>   plc_count{0};             // PLC invocations (for diagnostics)
    std::atomic<AudioCodec> last_codec{AudioCodec::Opus};
    std::atomic<int64_t>   packet_age_last_ns{0};
    std::atomic<int64_t>   packet_age_max_ns{0};
    std::atomic<int64_t>   packet_age_avg_ns{0};
    std::atomic<int64_t>   capture_to_playout_latency_last_ns{0};
    std::atomic<int64_t>   capture_to_playout_latency_max_ns{0};
    std::atomic<int64_t>   capture_to_playout_latency_avg_ns{0};
    std::atomic<uint64_t>  capture_to_playout_latency_samples{0};
    std::atomic<size_t>    queue_depth_max{0};
    std::atomic<size_t>    queue_depth_avg{0};
    std::atomic<int64_t>   queue_depth_drift_milli{0};
    SequenceArrivalTracker sequence_tracker;
    std::atomic<uint64_t>   sequence_gaps{0};
    std::atomic<uint64_t>   sequence_gap_recoveries{0};
    std::atomic<uint64_t>   sequence_unresolved_gaps{0};
    std::atomic<uint64_t>   sequence_gaps_declared_lost{0};
    std::atomic<uint64_t>   sequence_late_or_reordered{0};
    std::atomic<uint64_t>   jitter_depth_drops{0};
    std::atomic<uint64_t>   jitter_age_drops{0};
    bool                    drift_reference_initialized = false;
    uint32_t                drift_reference_sequence = 0;
    uint32_t                drift_reference_sample_rate = 48000;
    uint16_t                drift_reference_frame_count = 0;
    std::chrono::steady_clock::time_point drift_reference_time{};
    std::atomic<uint64_t>   receiver_drift_observations{0};
    std::atomic<int64_t>    receiver_drift_ppm_last_milli{0};
    std::atomic<int64_t>    receiver_drift_ppm_avg_milli{0};
    std::atomic<int64_t>    receiver_drift_ppm_abs_max_milli{0};
};

// Lightweight view for UI (snapshot of ParticipantData)
struct ParticipantInfo {
    uint32_t id;
    std::string profile_id;
    std::string display_name;
    Bytes<E2E_PUBLIC_KEY_BYTES> key_public;
    bool     has_key_public;
    bool     is_speaking;
    bool     is_muted;
    float    audio_level;
    float    audio_peak;
    float    gain;
    float    pan;  // 0.0 = full left, 0.5 = center, 1.0 = full right
    bool     buffer_ready;
    size_t   queue_size;
    size_t   queue_size_avg;
    size_t   queue_size_max;
    double   queue_drift_packets;
    size_t   jitter_buffer_min_packets;
    size_t   jitter_buffer_floor_packets;
    size_t   opus_queue_limit_packets;
    bool     opus_jitter_manual_override;
    bool     opus_jitter_auto_enabled;
    size_t   opus_jitter_auto_floor_packets;
    uint64_t opus_jitter_auto_increases;
    uint64_t opus_jitter_auto_decreases;
    size_t   opus_pcm_buffered_frames;
    uint64_t opus_packets_decoded_in_callback;
    uint64_t opus_queue_limit_drops;
    uint64_t opus_age_limit_drops;
    uint64_t opus_decode_buffer_overflow_drops;
    uint64_t opus_target_trim_drops;
    double   opus_playout_rate_ratio;
    int      opus_rate_correction_callbacks;
    size_t   last_packet_frame_count;
    size_t   last_callback_frame_count;
    int      underrun_count;
    size_t   plc_count;  // PLC invocations for diagnostics
    double   packet_age_last_ms;
    double   packet_age_avg_ms;
    double   packet_age_max_ms;
    double   capture_to_playout_latency_last_ms;
    double   capture_to_playout_latency_avg_ms;
    double   capture_to_playout_latency_max_ms;
    uint64_t capture_to_playout_latency_samples;
    uint64_t sequence_gaps;
    uint64_t sequence_gap_recoveries;
    uint64_t sequence_unresolved_gaps;
    uint64_t sequence_late_or_reordered;
    uint64_t jitter_depth_drops;
    uint64_t jitter_age_drops;
    double   receiver_drift_ppm_last;
    double   receiver_drift_ppm_avg;
    double   receiver_drift_ppm_abs_max;
};
