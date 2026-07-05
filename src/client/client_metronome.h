#pragma once

#include "protocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

struct ClientMetronomeState {
    float    bpm = 120.0F;
    bool     running = false;
    uint32_t beat_number = 0;
    uint64_t sync_sent = 0;
    uint64_t sync_received = 0;
    bool     clock_ready = false;
    double   clock_offset_ms = 0.0;
};

class ClientMetronome {
public:
    ClientMetronomeState state(bool clock_ready, int64_t clock_offset_ns) const;

    int bpm_milli() const;
    bool running() const;
    uint32_t beat_number() const;
    uint32_t next_boundary_beat() const;
    void set_bpm_milli_local(int bpm_milli);
    bool should_send_bpm_milli(int bpm_milli) const;
    std::optional<int> tap_tempo_bpm_milli(std::chrono::steady_clock::time_point now);

    void mark_sync_sent();
    void mark_sync_received();
    void schedule_sync(const MetronomeSyncHdr& sync, int64_t effective_server_time_ns);
    void mix_click(float* output_buffer, unsigned long frame_count, size_t out_channels,
                   int sample_rate, int64_t local_time_ns, int64_t server_clock_offset_ns);

private:
    static int clamp_bpm_milli(int bpm_milli);
    static int64_t ns_delta_to_samples(int64_t ns, size_t sample_rate);
    static int64_t beat_interval_samples(int bpm_milli, size_t sample_rate);

    void prepare_schedule(int64_t local_time_ns, size_t sample_rate,
                          int64_t server_clock_offset_ns);
    void apply_due_schedule(size_t sample_rate);

    std::atomic<int>      bpm_milli_{120000};
    std::atomic<bool>     running_{false};
    std::atomic<uint32_t> beat_number_{0};
    std::atomic<uint64_t> sync_sent_{0};
    std::atomic<uint64_t> sync_received_{0};
    std::atomic<int>      pending_bpm_milli_{120000};
    std::atomic<bool>     pending_running_{false};
    std::atomic<uint32_t> pending_beat_number_{0};
    std::atomic<int64_t>  pending_effective_server_time_ns_{0};
    std::atomic<uint32_t> pending_sequence_{0};
    uint32_t              prepared_sequence_ = 0;
    int64_t               prepared_effective_sample_ = 0;
    uint32_t              applied_sequence_ = 0;
    int64_t               epoch_sample_ = 0;
    int64_t               audio_sample_cursor_ = 0;
    bool                  timeline_ready_ = false;
    std::array<std::chrono::steady_clock::time_point, 8> tap_times_{};
    size_t                                             tap_count_ = 0;
    size_t                                             tap_index_ = 0;
};
