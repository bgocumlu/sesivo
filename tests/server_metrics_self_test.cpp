#include "server_metrics.h"

#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

int main() {
    server_metrics::Snapshot snapshot;
    snapshot.schema                 = "jam_server_metrics_v2";
    snapshot.server_id              = "server\"ops";
    snapshot.timestamp_unix_ms      = 123456789;
    snapshot.uptime_ms              = 2500;
    snapshot.connected_clients      = 2;
    snapshot.unknown_endpoint_count = 1;
    snapshot.token_nonce_count      = 1;
    snapshot.metrics_export_drops_total = 2;
    snapshot.metrics_export_failures_total = 3;
    snapshot.drops.unknown_audio_interval = 3;
    snapshot.drops.unknown_audio_total    = 5;
    snapshot.drops.invalid_audio_interval = 7;
    snapshot.drops.invalid_audio_total    = 11;
    snapshot.drops.rate_limited_audio_interval = 13;
    snapshot.drops.rate_limited_audio_total    = 17;
    snapshot.drops.sfu_send_cap_interval       = 19;
    snapshot.drops.sfu_send_cap_total          = 23;
    snapshot.drops.sfu_pool_interval           = 29;
    snapshot.drops.sfu_pool_total              = 31;
    snapshot.drops.expired_media_interval      = 37;
    snapshot.drops.expired_media_total         = 41;
    snapshot.ingress.push_back(
        {1, "127.0.0.1:40000", {10, 20, 2, 4, 1, 3, 1, 0, 4}, 120});
    snapshot.receive_handler_us = {{1, 2, 3, 4, 5, 6, 7, 8, 9}, 45, 11'000};
    snapshot.relay_dwell_us = {{9, 8, 7, 6, 5, 4, 3, 2, 1}, 45, 9'000};
    snapshot.pings.push_back(
        {2, "127.0.0.1:40001", 4, 8, {6, 12, 1, 1, 0, 1, 0, 0, 2}});

    const auto json = server_metrics::to_json_line(snapshot);
    require(json.ends_with('\n'), "metrics JSONL line must end in newline");
    require(json.find("\"schema\":\"jam_server_metrics_v2\"") != std::string::npos,
            "schema field missing");
    require(json.find("\"server_id\":\"server\\\"ops\"") != std::string::npos,
            "server_id was not escaped");
    require(json.find("\"connected_clients\":2") != std::string::npos,
            "connected client count missing");
    require(json.find("\"unknown_audio_interval\":3") != std::string::npos,
            "drop counters missing");
    require(json.find("\"metrics_export_drops_total\":2") != std::string::npos,
            "metrics exporter counters missing");
    require(json.find("\"sfu_send_cap_total\":23") != std::string::npos,
            "SFU send-cap drop counters missing");
    require(json.find("\"sfu_pool_total\":31") != std::string::npos,
            "SFU pool drop counters missing");
    require(json.find("\"expired_media_total\":41") != std::string::npos,
            "expired media counters missing");
    require(json.find("\"ingress\"") != std::string::npos, "ingress array missing");
    require(json.find("\"relay_dwell_us\"") != std::string::npos,
            "relay dwell histogram missing");
    require(json.find("\"maximum_us\":11000") != std::string::npos,
            "receive handler maximum missing");
    require(json.find("\"pings\"") != std::string::npos, "ping array missing");

    const auto dir = std::filesystem::temp_directory_path() / "jam_metrics_self_test";
    std::filesystem::remove_all(dir);
    server_metrics::JsonlExporter exporter(dir / "metrics.jsonl");
    std::string error;
    require(exporter.write(snapshot, &error), "first metrics write failed");
    require(exporter.write(snapshot, &error), "second metrics write failed");

    {
        std::ifstream in(dir / "metrics.jsonl", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        require(body.find("\"server_id\":\"server\\\"ops\"") != std::string::npos,
                "metrics file did not contain snapshot");
        require(body.find('\n') != body.rfind('\n'),
                "metrics file should contain two JSONL rows");
    }

    std::atomic<bool> stalled_writer_entered{false};
    std::atomic<bool> release_stalled_writer{false};
    server_metrics::AsyncJsonlExporter async_exporter(
        [&](const server_metrics::Snapshot&, std::string*) {
            stalled_writer_entered.store(true, std::memory_order_release);
            while (!release_stalled_writer.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return true;
        });
    require(async_exporter.enqueue(snapshot), "first async snapshot should be queued");
    while (!stalled_writer_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const auto enqueue_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 16; ++i) {
        (void)async_exporter.enqueue(snapshot);
    }
    const auto enqueue_elapsed = std::chrono::steady_clock::now() - enqueue_start;
    require(enqueue_elapsed < std::chrono::milliseconds(50),
            "stalled metrics destination must not block the producer");
    require(async_exporter.dropped_snapshots() > 0,
            "bounded async exporter must drop stale snapshots under a stall");
    release_stalled_writer.store(true, std::memory_order_release);
    require(async_exporter.wait_for_idle(std::chrono::seconds(2)),
            "async exporter did not drain after the destination recovered");

    std::filesystem::remove_all(dir);
    std::cout << "server metrics self-test passed\n";
}
