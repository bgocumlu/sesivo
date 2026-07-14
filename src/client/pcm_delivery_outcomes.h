#pragma once

#include "protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>

struct OpusPcmCaptureChunk {
    size_t frames = 0;
    int64_t capture_server_time_ns = 0;
    bool capture_timestamp_valid = false;
    bool received_media = false;
};

struct FrameWeightedLatencyAccumulator {
    void observe(int64_t latency_ns, size_t consumed_frames,
                 bool completed_sample) {
        weighted_latency_ns +=
            static_cast<double>(latency_ns) * consumed_frames;
        frames += consumed_frames;
        max_latency_ns = latency_ns > max_latency_ns ? latency_ns : max_latency_ns;
        completed_samples += completed_sample ? 1 : 0;
    }

    bool empty() const {
        return frames == 0;
    }

    int64_t average_ns() const {
        return frames == 0
                   ? 0
                   : static_cast<int64_t>(weighted_latency_ns /
                                          static_cast<double>(frames));
    }

    double weighted_latency_ns = 0.0;
    size_t frames = 0;
    int64_t max_latency_ns = 0;
    uint64_t completed_samples = 0;
};

class PcmDeliveryOutcomes {
public:
    bool append(size_t frames, int64_t capture_server_time_ns,
                bool capture_timestamp_valid, bool received_media) {
        if (frames == 0) {
            return true;
        }
        if (count_ == chunks_.size()) {
            return false;
        }
        const size_t index = (head_ + count_) % chunks_.size();
        chunks_[index] = OpusPcmCaptureChunk{
            frames, capture_server_time_ns, capture_timestamp_valid, received_media};
        ++count_;
        return true;
    }

    template <typename Observer>
    uint64_t consume(size_t frames, Observer&& observer) {
        uint64_t delivered = 0;
        size_t remaining = frames;
        while (remaining > 0 && count_ > 0) {
            auto& chunk = chunks_[head_];
            const size_t consumed = remaining < chunk.frames ? remaining : chunk.frames;
            observer(chunk, consumed);
            chunk.frames -= consumed;
            remaining -= consumed;
            if (chunk.frames == 0) {
                delivered += chunk.received_media ? 1 : 0;
                head_ = (head_ + 1) % chunks_.size();
                --count_;
            }
        }
        return delivered;
    }

    uint64_t discard_all_as_lost() {
        const uint64_t lost = pending_received();
        clear();
        return lost;
    }

    uint64_t discard_overflow_as_lost(bool incoming_received) {
        return discard_all_as_lost() + (incoming_received ? 1 : 0);
    }

    uint64_t pending_received() const {
        uint64_t pending = 0;
        for (size_t offset = 0; offset < count_; ++offset) {
            const size_t index = (head_ + offset) % chunks_.size();
            pending += chunks_[index].received_media ? 1 : 0;
        }
        return pending;
    }

    void clear() {
        head_ = 0;
        count_ = 0;
    }

    size_t size() const {
        return count_;
    }

    static constexpr size_t capacity() {
        return MAX_OPUS_QUEUE_SIZE;
    }

private:
    std::array<OpusPcmCaptureChunk, MAX_OPUS_QUEUE_SIZE> chunks_{};
    size_t head_ = 0;
    size_t count_ = 0;
};
