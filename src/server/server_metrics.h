#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace server_metrics {

struct TrafficCounters {
    uint64_t interval = 0;
    uint64_t total = 0;
    uint64_t sequence_gaps_interval = 0;
    uint64_t sequence_gaps_total = 0;
    uint64_t sequence_gap_recoveries_interval = 0;
    uint64_t sequence_gap_recoveries_total = 0;
    uint64_t sequence_unresolved_gaps = 0;
    uint64_t sequence_late_or_reordered_interval = 0;
    uint64_t sequence_late_or_reordered_total = 0;
};

struct DropCounters {
    uint64_t unknown_audio_interval = 0;
    uint64_t unknown_audio_total = 0;
    uint64_t invalid_audio_interval = 0;
    uint64_t invalid_audio_total = 0;
    uint64_t rate_limited_audio_interval = 0;
    uint64_t rate_limited_audio_total = 0;
    uint64_t sfu_send_cap_interval = 0;
    uint64_t sfu_send_cap_total = 0;
    uint64_t sfu_pool_interval = 0;
    uint64_t sfu_pool_total = 0;
    uint64_t expired_media_interval = 0;
    uint64_t expired_media_total = 0;
};

struct IngressMetric {
    uint32_t sender_id = 0;
    std::string endpoint;
    TrafficCounters audio;
    uint16_t last_frame_count = 0;
};

struct LatencyHistogram {
    std::array<uint64_t, 9> counts{};
    uint64_t samples = 0;
    uint64_t maximum_us = 0;
};

struct PingMetric {
    uint32_t client_id = 0;
    std::string endpoint;
    uint64_t reply_queued_interval = 0;
    uint64_t reply_queued_total = 0;
    TrafficCounters ping;
};

struct Snapshot {
    std::string schema = "jam_server_metrics_v2";
    std::string server_id;
    int64_t timestamp_unix_ms = 0;
    int64_t uptime_ms = 0;
    uint64_t connected_clients = 0;
    uint64_t unknown_endpoint_count = 0;
    uint64_t token_nonce_count = 0;
    uint64_t metrics_export_drops_total = 0;
    uint64_t metrics_export_failures_total = 0;
    DropCounters drops;
    LatencyHistogram receive_handler_us;
    LatencyHistogram relay_dwell_us;
    std::vector<IngressMetric> ingress;
    std::vector<PingMetric> pings;
};

inline void append_latency_histogram(std::ostringstream& out,
                                     const LatencyHistogram& histogram) {
    out << "{\"upper_bounds_us\":[50,100,250,500,1000,2500,5000,10000,null],"
        << "\"counts\":[";
    for (size_t i = 0; i < histogram.counts.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << histogram.counts[i];
    }
    out << "],\"samples\":" << histogram.samples
        << ",\"maximum_us\":" << histogram.maximum_us << '}';
}

inline void append_json_string(std::ostringstream& out, std::string_view value) {
    static constexpr char HEX[] = "0123456789abcdef";
    out << '"';
    for (unsigned char ch: value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << HEX[(ch >> 4) & 0x0F] << HEX[ch & 0x0F];
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
}

inline void append_counter_set(std::ostringstream& out, const TrafficCounters& counters,
                               std::string_view interval_name,
                               std::string_view total_name) {
    out << '{';
    append_json_string(out, interval_name);
    out << ':' << counters.interval << ',';
    append_json_string(out, total_name);
    out << ':' << counters.total
        << ",\"sequence_gaps_interval\":" << counters.sequence_gaps_interval
        << ",\"sequence_gaps_total\":" << counters.sequence_gaps_total
        << ",\"sequence_gap_recoveries_interval\":"
        << counters.sequence_gap_recoveries_interval
        << ",\"sequence_gap_recoveries_total\":" << counters.sequence_gap_recoveries_total
        << ",\"sequence_unresolved_gaps\":" << counters.sequence_unresolved_gaps
        << ",\"sequence_late_or_reordered_interval\":"
        << counters.sequence_late_or_reordered_interval
        << ",\"sequence_late_or_reordered_total\":"
        << counters.sequence_late_or_reordered_total << '}';
}

inline std::string to_json_line(const Snapshot& snapshot) {
    std::ostringstream out;
    out << '{';
    out << "\"schema\":";
    append_json_string(out, snapshot.schema);
    out << ",\"server_id\":";
    append_json_string(out, snapshot.server_id);
    out << ",\"timestamp_unix_ms\":" << snapshot.timestamp_unix_ms
        << ",\"uptime_ms\":" << snapshot.uptime_ms
        << ",\"connected_clients\":" << snapshot.connected_clients
        << ",\"unknown_endpoint_count\":" << snapshot.unknown_endpoint_count
        << ",\"token_nonce_count\":" << snapshot.token_nonce_count
        << ",\"metrics_export_drops_total\":" << snapshot.metrics_export_drops_total
        << ",\"metrics_export_failures_total\":"
        << snapshot.metrics_export_failures_total;

    out << ",\"drops\":{\"unknown_audio_interval\":"
        << snapshot.drops.unknown_audio_interval
        << ",\"unknown_audio_total\":" << snapshot.drops.unknown_audio_total
        << ",\"invalid_audio_interval\":" << snapshot.drops.invalid_audio_interval
        << ",\"invalid_audio_total\":" << snapshot.drops.invalid_audio_total
        << ",\"rate_limited_audio_interval\":"
        << snapshot.drops.rate_limited_audio_interval
        << ",\"rate_limited_audio_total\":"
        << snapshot.drops.rate_limited_audio_total
        << ",\"sfu_send_cap_interval\":" << snapshot.drops.sfu_send_cap_interval
        << ",\"sfu_send_cap_total\":" << snapshot.drops.sfu_send_cap_total
        << ",\"sfu_pool_interval\":" << snapshot.drops.sfu_pool_interval
        << ",\"sfu_pool_total\":" << snapshot.drops.sfu_pool_total
        << ",\"expired_media_interval\":" << snapshot.drops.expired_media_interval
        << ",\"expired_media_total\":" << snapshot.drops.expired_media_total << '}';

    out << ",\"receive_handler_us\":";
    append_latency_histogram(out, snapshot.receive_handler_us);
    out << ",\"relay_dwell_us\":";
    append_latency_histogram(out, snapshot.relay_dwell_us);

    out << ",\"ingress\":[";
    for (size_t i = 0; i < snapshot.ingress.size(); ++i) {
        const auto& metric = snapshot.ingress[i];
        if (i > 0) {
            out << ',';
        }
        out << "{\"sender_id\":" << metric.sender_id << ",\"endpoint\":";
        append_json_string(out, metric.endpoint);
        out << ",\"last_frame_count\":" << metric.last_frame_count << ",\"audio\":";
        append_counter_set(out, metric.audio, "received_interval", "received_total");
        out << '}';
    }
    out << ']';

    out << ",\"pings\":[";
    for (size_t i = 0; i < snapshot.pings.size(); ++i) {
        const auto& metric = snapshot.pings[i];
        if (i > 0) {
            out << ',';
        }
        out << "{\"client_id\":" << metric.client_id << ",\"endpoint\":";
        append_json_string(out, metric.endpoint);
        out << ",\"reply_queued_interval\":" << metric.reply_queued_interval
            << ",\"reply_queued_total\":" << metric.reply_queued_total
            << ",\"ping\":";
        append_counter_set(out, metric.ping, "received_interval", "received_total");
        out << '}';
    }
    out << "]}";
    out << '\n';
    return out.str();
}

class JsonlExporter {
public:
    JsonlExporter() = default;
    explicit JsonlExporter(std::filesystem::path path) : path_(std::move(path)) {}

    bool enabled() const {
        return !path_.empty();
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    bool write(const Snapshot& snapshot, std::string* error = nullptr) const {
        if (!enabled()) {
            return true;
        }

        try {
            const auto parent = path_.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }

            std::ofstream out(path_, std::ios::app | std::ios::binary);
            if (!out) {
                if (error != nullptr) {
                    *error = "open failed";
                }
                return false;
            }
            out << to_json_line(snapshot);
            out.flush();
            if (!out) {
                if (error != nullptr) {
                    *error = "write failed";
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            if (error != nullptr) {
                *error = e.what();
            }
            return false;
        }
    }

private:
    std::filesystem::path path_;
};

class AsyncJsonlExporter {
public:
    using Writer = std::function<bool(const Snapshot&, std::string*)>;
    static constexpr std::size_t MAX_QUEUED_SNAPSHOTS = 2;

    AsyncJsonlExporter() = default;

    explicit AsyncJsonlExporter(std::filesystem::path path)
        : path_(std::move(path)) {
        if (!path_.empty()) {
            const JsonlExporter exporter(path_);
            writer_ = [exporter](const Snapshot& snapshot, std::string* error) {
                return exporter.write(snapshot, error);
            };
            start_worker();
        }
    }

    explicit AsyncJsonlExporter(Writer writer)
        : writer_(std::move(writer)) {
        if (writer_) {
            start_worker();
        }
    }

    AsyncJsonlExporter(const AsyncJsonlExporter&) = delete;
    AsyncJsonlExporter& operator=(const AsyncJsonlExporter&) = delete;

    ~AsyncJsonlExporter() {
        stop();
    }

    bool enabled() const {
        return static_cast<bool>(writer_);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    // The media/event-loop producer never waits for the exporter thread. If the
    // queue lock is briefly contended or both slots are occupied, discard stale
    // telemetry and preserve media progress.
    bool enqueue(Snapshot snapshot) {
        if (!enabled()) {
            return true;
        }
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            dropped_snapshots_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        bool dropped = false;
        if (queue_.size() >= MAX_QUEUED_SNAPSHOTS) {
            queue_.pop_front();
            dropped_snapshots_.fetch_add(1, std::memory_order_relaxed);
            dropped = true;
        }
        queue_.push_back(std::move(snapshot));
        lock.unlock();
        work_ready_.notify_one();
        return !dropped;
    }

    uint64_t dropped_snapshots() const {
        return dropped_snapshots_.load(std::memory_order_relaxed);
    }

    uint64_t failed_writes() const {
        return failed_writes_.load(std::memory_order_relaxed);
    }

    bool wait_for_idle(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return idle_.wait_for(lock, timeout,
                              [this]() { return queue_.empty() && !write_active_; });
    }

private:
    void start_worker() {
        worker_ = std::thread([this]() { run(); });
    }

    void stop() {
        if (!worker_.joinable()) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_ready_.notify_one();
        worker_.join();
    }

    void run() {
        for (;;) {
            Snapshot snapshot;
            {
                std::unique_lock lock(mutex_);
                work_ready_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                snapshot = std::move(queue_.front());
                queue_.pop_front();
                write_active_ = true;
            }

            std::string error;
            if (!writer_(snapshot, &error)) {
                failed_writes_.fetch_add(1, std::memory_order_relaxed);
            }

            {
                std::lock_guard lock(mutex_);
                write_active_ = false;
            }
            idle_.notify_all();
        }
    }

    std::filesystem::path path_;
    Writer writer_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable idle_;
    std::deque<Snapshot> queue_;
    std::thread worker_;
    bool stopping_ = false;
    bool write_active_ = false;
    std::atomic<uint64_t> dropped_snapshots_{0};
    std::atomic<uint64_t> failed_writes_{0};
};

}  // namespace server_metrics
