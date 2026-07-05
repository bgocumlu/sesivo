#include "client_metronome.h"

#include <algorithm>
#include <cmath>

using namespace std::chrono_literals;

ClientMetronomeState ClientMetronome::state(bool clock_ready, int64_t clock_offset_ns) const {
    return ClientMetronomeState{
        static_cast<float>(bpm_milli_.load(std::memory_order_acquire)) / 1000.0F,
        running_.load(std::memory_order_acquire),
        beat_number_.load(std::memory_order_acquire),
        sync_sent_.load(std::memory_order_relaxed),
        sync_received_.load(std::memory_order_relaxed),
        clock_ready,
        static_cast<double>(clock_offset_ns) / 1e6,
    };
}

int ClientMetronome::bpm_milli() const {
    return bpm_milli_.load(std::memory_order_acquire);
}

bool ClientMetronome::running() const {
    return running_.load(std::memory_order_acquire);
}

uint32_t ClientMetronome::beat_number() const {
    return beat_number_.load(std::memory_order_acquire);
}

uint32_t ClientMetronome::next_boundary_beat() const {
    return beat_number() + 1;
}

void ClientMetronome::set_bpm_milli_local(int bpm_milli) {
    bpm_milli_.store(clamp_bpm_milli(bpm_milli), std::memory_order_release);
}

bool ClientMetronome::should_send_bpm_milli(int bpm_milli) const {
    const int clamped = clamp_bpm_milli(bpm_milli);
    const int current_bpm_milli = bpm_milli_.load(std::memory_order_acquire);
    const int pending_bpm_milli =
        pending_bpm_milli_.load(std::memory_order_acquire);
    const uint32_t pending_sequence =
        pending_sequence_.load(std::memory_order_acquire);
    return clamped != current_bpm_milli ||
           (pending_sequence != 0 && pending_bpm_milli != clamped);
}

std::optional<int> ClientMetronome::tap_tempo_bpm_milli(
    std::chrono::steady_clock::time_point now) {
    if (tap_count_ > 0 &&
        now - tap_times_[(tap_index_ + tap_times_.size() - 1) % tap_times_.size()] > 2s) {
        tap_count_ = 0;
        tap_index_ = 0;
    }

    tap_times_[tap_index_] = now;
    tap_index_ = (tap_index_ + 1) % tap_times_.size();
    tap_count_ = std::min(tap_count_ + 1, tap_times_.size());

    if (tap_count_ < 3) {
        return std::nullopt;
    }

    double total_interval_ms = 0.0;
    size_t interval_count = 0;
    for (size_t i = 1; i < tap_count_; ++i) {
        const size_t newer = (tap_index_ + tap_times_.size() - i) % tap_times_.size();
        const size_t older = (tap_index_ + tap_times_.size() - i - 1) % tap_times_.size();
        const auto interval = tap_times_[newer] - tap_times_[older];
        total_interval_ms +=
            std::chrono::duration<double, std::milli>(interval).count();
        ++interval_count;
    }

    if (interval_count == 0 || total_interval_ms <= 0.0) {
        return std::nullopt;
    }

    const double avg_interval_ms = total_interval_ms / static_cast<double>(interval_count);
    return clamp_bpm_milli(static_cast<int>(std::lrint(60000000.0 / avg_interval_ms)));
}

void ClientMetronome::mark_sync_sent() {
    sync_sent_.fetch_add(1, std::memory_order_relaxed);
}

void ClientMetronome::mark_sync_received() {
    sync_received_.fetch_add(1, std::memory_order_relaxed);
}

void ClientMetronome::schedule_sync(const MetronomeSyncHdr& sync,
                                    int64_t effective_server_time_ns) {
    const uint32_t current_sequence =
        pending_sequence_.load(std::memory_order_acquire);
    if (sync.sequence != 0 && sync.sequence <= current_sequence) {
        return;
    }
    pending_bpm_milli_.store(clamp_bpm_milli(static_cast<int>(sync.bpm_milli)),
                             std::memory_order_relaxed);
    pending_running_.store((sync.flags & METRONOME_FLAG_RUNNING) != 0,
                           std::memory_order_relaxed);
    pending_beat_number_.store(sync.beat_number, std::memory_order_relaxed);
    pending_effective_server_time_ns_.store(effective_server_time_ns,
                                            std::memory_order_relaxed);
    pending_sequence_.store(sync.sequence == 0 ? current_sequence + 1 : sync.sequence,
                            std::memory_order_release);
}

void ClientMetronome::mix_click(float* output_buffer, unsigned long frame_count,
                                size_t out_channels, int sample_rate,
                                int64_t local_time_ns, int64_t server_clock_offset_ns) {
    const int clamped_bpm_milli = std::max(1, bpm_milli());
    const size_t clamped_sample_rate =
        static_cast<size_t>(std::max(1, sample_rate));
    const int64_t interval_samples =
        beat_interval_samples(clamped_bpm_milli, clamped_sample_rate);
    const size_t click_samples = std::max<size_t>(1, clamped_sample_rate / 35);
    prepare_schedule(local_time_ns, clamped_sample_rate, server_clock_offset_ns);

    constexpr double PI = 3.14159265358979323846;
    for (unsigned long frame = 0; frame < frame_count; ++frame) {
        apply_due_schedule(clamped_sample_rate);

        if (!running_.load(std::memory_order_acquire) || !timeline_ready_) {
            ++audio_sample_cursor_;
            continue;
        }

        const int64_t elapsed_samples = audio_sample_cursor_ - epoch_sample_;
        if (elapsed_samples < 0) {
            ++audio_sample_cursor_;
            continue;
        }

        const uint32_t current_beat =
            static_cast<uint32_t>(elapsed_samples / interval_samples) + 1;
        const size_t click_sample =
            static_cast<size_t>(elapsed_samples % interval_samples);
        beat_number_.store(current_beat, std::memory_order_release);

        if (click_sample < click_samples) {
            const bool downbeat = ((current_beat - 1) % 4) == 0;
            const double frequency = downbeat ? 1320.0 : 880.0;
            const double t = static_cast<double>(click_sample) /
                             static_cast<double>(clamped_sample_rate);
            const double envelope =
                std::exp(-7.0 * static_cast<double>(click_sample) /
                         static_cast<double>(click_samples));
            const float click =
                static_cast<float>(std::sin(2.0 * PI * frequency * t) * envelope * 0.22);
            for (size_t channel = 0; channel < out_channels; ++channel) {
                const size_t index = (frame * out_channels) + channel;
                output_buffer[index] = std::clamp(output_buffer[index] + click, -1.0F, 1.0F);
            }
        }

        ++audio_sample_cursor_;
    }
}

int ClientMetronome::clamp_bpm_milli(int bpm_milli) {
    return std::clamp(bpm_milli, 30000, 240000);
}

int64_t ClientMetronome::ns_delta_to_samples(int64_t ns, size_t sample_rate) {
    return static_cast<int64_t>((static_cast<long double>(ns) *
                                 static_cast<long double>(sample_rate)) /
                                1'000'000'000.0L);
}

int64_t ClientMetronome::beat_interval_samples(int bpm_milli, size_t sample_rate) {
    return std::max<int64_t>(
        1, static_cast<int64_t>((static_cast<long double>(sample_rate) *
                                 60'000.0L) /
                                static_cast<long double>(std::max(1, bpm_milli))));
}

void ClientMetronome::prepare_schedule(int64_t local_time_ns, size_t sample_rate,
                                       int64_t server_clock_offset_ns) {
    const uint32_t pending_sequence =
        pending_sequence_.load(std::memory_order_acquire);
    if (pending_sequence == 0 || pending_sequence == prepared_sequence_) {
        return;
    }

    const int64_t effective_ns =
        pending_effective_server_time_ns_.load(std::memory_order_relaxed);
    const int64_t local_effective_ns = effective_ns - server_clock_offset_ns;
    const int64_t delta_samples =
        ns_delta_to_samples(local_effective_ns - local_time_ns, sample_rate);
    prepared_effective_sample_ = audio_sample_cursor_ + delta_samples;
    prepared_sequence_ = pending_sequence;
}

void ClientMetronome::apply_due_schedule(size_t sample_rate) {
    if (prepared_sequence_ == 0 ||
        prepared_sequence_ == applied_sequence_ ||
        audio_sample_cursor_ < prepared_effective_sample_) {
        return;
    }

    const int scheduled_bpm_milli =
        std::max(1, pending_bpm_milli_.load(std::memory_order_relaxed));
    const bool scheduled_running = pending_running_.load(std::memory_order_relaxed);
    const uint32_t scheduled_beat = pending_beat_number_.load(std::memory_order_relaxed);
    const int64_t interval_samples = beat_interval_samples(scheduled_bpm_milli, sample_rate);

    bpm_milli_.store(scheduled_bpm_milli, std::memory_order_release);
    running_.store(scheduled_running, std::memory_order_release);
    beat_number_.store(scheduled_beat, std::memory_order_release);
    epoch_sample_ =
        prepared_effective_sample_ - (static_cast<int64_t>(scheduled_beat) * interval_samples);
    timeline_ready_ = true;
    applied_sequence_ = prepared_sequence_;
}
