#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <spdlog/spdlog.h>

#include "audio_packet.h"
#include "client_manager.h"
#include "crash_reporter.h"
#include "endpoint_hash.h"
#include "logging_setup.h"
#include "message_validator.h"
#include "performer_join_token.h"
#include "packet_builder.h"
#include "periodic_timer.h"
#include "protocol.h"
#include "room_registry.h"
#include "sequence_tracker.h"
#include "server_options.h"
#include "server_rate_limiter.h"
#include "server_config.h"
#include "server_metrics.h"
#include "session_crypto.h"
#include "sesivo_version.h"
#include "udp_port.h"
#include "udp_socket_config.h"

using asio::ip::udp;
using namespace std::chrono_literals;
using namespace server_config;

constexpr int64_t METRONOME_SCHEDULE_AHEAD_NS = 150'000'000;
constexpr int64_t ROOM_JOIN_TICKET_TTL_MS = 10 * 60 * 1000;
// 128 datagrams are four synchronized 32-sender rounds: 10 ms at 2.5 ms
// packets. Pool exhaustion therefore sheds new media within the relay deadline
// instead of allowing an unbounded application queue.
constexpr size_t MAX_FAN_OUT_BUFFERS = 128;
constexpr size_t MEDIA_WORKER_COUNT = 4;
constexpr size_t MEDIA_WORKER_QUEUE_CAPACITY = 256;
constexpr auto SERVER_MEDIA_DEADLINE = 10ms;

static const char* runtime_platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static const char* runtime_arch_name() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

static bool is_lan_ipv4(const asio::ip::address_v4& address) {
    const auto bytes = address.to_bytes();
    return bytes[0] == 10 ||
           (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
           (bytes[0] == 192 && bytes[1] == 168);
}

static void append_unique_address(std::vector<std::string>& addresses,
                                  std::string address) {
    if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
        addresses.push_back(std::move(address));
    }
}

static std::vector<std::string> lan_ipv4_addresses(asio::io_context& io_context) {
    std::vector<std::string> addresses;
    std::error_code ec;
    const auto hostname = asio::ip::host_name(ec);
    if (ec || hostname.empty()) {
        return addresses;
    }

    udp::resolver resolver(io_context);
    const auto results = resolver.resolve(udp::v4(), hostname, "0", ec);
    if (ec) {
        return addresses;
    }

    for (const auto& entry: results) {
        const auto address = entry.endpoint().address();
        if (!address.is_v4()) {
            continue;
        }
        const auto ipv4 = address.to_v4();
        if (is_lan_ipv4(ipv4)) {
            append_unique_address(addresses, ipv4.to_string());
        }
    }
    return addresses;
}

static void log_server_addresses(asio::io_context& io_context, uint16_t port) {
    spdlog::warn("Sesivo server address: Local: 127.0.0.1:{}", port);
    const auto addresses = lan_ipv4_addresses(io_context);
    if (addresses.empty()) {
        spdlog::warn("Sesivo server address: LAN: not detected");
        return;
    }
    for (const auto& address: addresses) {
        spdlog::warn("Sesivo server address: LAN: {}:{}", address, port);
    }
}

template <size_t N>
std::string fixed_string(const Bytes<N>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), '\0');
    return std::string(bytes.begin(), end);
}

class Server {
    struct ReceiveSlot {
        std::array<char, server_config::RECV_BUF_SIZE> bytes{};
        udp::endpoint remote_endpoint;
    };

    struct FanOutBuffer {
        std::array<unsigned char, server_config::RECV_BUF_SIZE> bytes{};
        size_t size = 0;
        std::chrono::steady_clock::time_point admitted_at{};
        std::atomic<uint32_t> pending_workers{0};
    };

    struct MediaTask {
        FanOutBuffer* buffer = nullptr;
        std::shared_ptr<const std::vector<udp::endpoint>> endpoints;
        udp::endpoint sender;
    };

    struct MediaTaskQueue {
        bool try_push(MediaTask task) {
            const size_t write = write_index.load(std::memory_order_relaxed);
            const size_t next = (write + 1) % MEDIA_WORKER_QUEUE_CAPACITY;
            if (next == read_index.load(std::memory_order_acquire)) {
                return false;
            }
            tasks[write] = std::move(task);
            write_index.store(next, std::memory_order_release);
            return true;
        }

        bool try_pop(MediaTask& task) {
            const size_t read = read_index.load(std::memory_order_relaxed);
            if (read == write_index.load(std::memory_order_acquire)) {
                return false;
            }
            task = std::move(tasks[read]);
            read_index.store((read + 1) % MEDIA_WORKER_QUEUE_CAPACITY,
                             std::memory_order_release);
            return true;
        }

        std::array<MediaTask, MEDIA_WORKER_QUEUE_CAPACITY> tasks{};
        std::atomic<size_t> write_index{0};
        std::atomic<size_t> read_index{0};
    };

    struct MediaWorker {
        MediaWorker() : ready(0) {}
        MediaTaskQueue queue;
        std::counting_semaphore<MEDIA_WORKER_QUEUE_CAPACITY + 1> ready;
        std::thread thread;
        std::atomic<bool> running{false};
    };

    struct ConcurrentLatencyHistogramState {
        std::array<std::atomic<uint64_t>, 9> counts{};
        std::atomic<uint64_t> samples{0};
        std::atomic<uint64_t> maximum_us{0};

        void observe(std::chrono::steady_clock::duration elapsed) {
            static constexpr std::array<uint64_t, 8> BOUNDS_US{
                50, 100, 250, 500, 1'000, 2'500, 5'000, 10'000};
            const auto signed_us =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            const auto value = static_cast<uint64_t>(std::max<int64_t>(0, signed_us));
            const auto bucket = static_cast<size_t>(
                std::lower_bound(BOUNDS_US.begin(), BOUNDS_US.end(), value) -
                BOUNDS_US.begin());
            counts[bucket].fetch_add(1, std::memory_order_relaxed);
            samples.fetch_add(1, std::memory_order_relaxed);
            auto previous = maximum_us.load(std::memory_order_relaxed);
            while (value > previous &&
                   !maximum_us.compare_exchange_weak(previous, value,
                                                     std::memory_order_relaxed)) {
            }
        }

        server_metrics::LatencyHistogram snapshot_and_reset() {
            server_metrics::LatencyHistogram result;
            for (size_t i = 0; i < counts.size(); ++i) {
                result.counts[i] = counts[i].exchange(0, std::memory_order_acq_rel);
                result.samples += result.counts[i];
            }
            (void)samples.exchange(0, std::memory_order_acq_rel);
            result.maximum_us = maximum_us.exchange(0, std::memory_order_acq_rel);
            return result;
        }
    };

    struct LatencyHistogramState {
        std::array<uint64_t, 9> counts{};
        uint64_t samples = 0;
        uint64_t maximum_us = 0;

        void observe(std::chrono::steady_clock::duration elapsed) {
            static constexpr std::array<uint64_t, 8> BOUNDS_US{
                50, 100, 250, 500, 1'000, 2'500, 5'000, 10'000};
            const auto signed_us =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            const auto value = static_cast<uint64_t>(std::max<int64_t>(0, signed_us));
            const auto bucket = static_cast<size_t>(
                std::lower_bound(BOUNDS_US.begin(), BOUNDS_US.end(), value) -
                BOUNDS_US.begin());
            ++counts[bucket];
            ++samples;
            maximum_us = std::max(maximum_us, value);
        }

        server_metrics::LatencyHistogram snapshot() const {
            return {counts, samples, maximum_us};
        }

        void reset() {
            counts.fill(0);
            samples = 0;
            maximum_us = 0;
        }
    };

    struct ReceiveTimingScope {
        Server& server;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        ~ReceiveTimingScope() {
            server.receive_handler_latency_.observe(std::chrono::steady_clock::now() - started);
        }
    };

public:
    Server(asio::io_context& io_context, const ServerOptions& options)
        : options_(options),
          socket_(io_context),
          metrics_exporter_(options.metrics_jsonl_path),
          started_at_(std::chrono::steady_clock::now()),
          alive_check_timer_(io_context, server_config::ALIVE_CHECK_INTERVAL,
                             [this]() { alive_check_timer_callback(); }) {
        std::error_code socket_error;
        const auto protocol =
            udp_network::open_dual_stack_socket(socket_, options.port, socket_error);
        if (socket_error) {
            throw std::runtime_error("Failed to bind UDP socket: " +
                                     socket_error.message());
        }
        const auto local = socket_.local_endpoint();
        spdlog::info("UDP socket bound on {}:{} ({})",
                  udp_network::format_address_for_display(local.address()), local.port(),
                  protocol == udp::v6() ? "IPv6 dual-stack" : "IPv4 fallback");
        socket_.non_blocking(true);

        // Optimize UDP socket buffers for high-throughput packet forwarding
        std::error_code buffer_error;
        udp_network::configure_low_latency_buffers(socket_, buffer_error);
        if (!buffer_error) {
            spdlog::info("UDP socket buffers optimized for packet forwarding ({} bytes)",
                      UDP_SOCKET_BUFFER_BYTES);
        } else {
            spdlog::warn("Failed to set socket buffer sizes: {}", buffer_error.message());
        }

        spdlog::info("SFU server ready: forwarding audio between clients");
        start_media_workers();
        for (auto& slot: receive_slots_) {
            do_receive(slot);
        }
    }

    ~Server() {
        stop_media_workers();
        std::error_code error;
        socket_.close(error);
    }

    uint16_t local_port() const {
        return socket_.local_endpoint().port();
    }

    bool is_dual_stack_socket() const {
        std::error_code ec;
        const auto      local = socket_.local_endpoint(ec);
        return !ec && local.protocol() == udp::v6();
    }

    void begin_shutdown() {
        if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        alive_check_timer_.stop();
        std::error_code error;
        socket_.cancel(error);
        stop_media_workers();
        (void)export_metrics_snapshot();
        socket_.close(error);
        spdlog::info("SFU readiness=false; receive admission stopped");
    }

    bool export_metrics_snapshot() {
        if (!metrics_exporter_.enabled()) {
            return true;
        }

        if (metrics_exporter_.enqueue(build_metrics_snapshot())) {
            return true;
        }

        spdlog::warn("Server metrics snapshot dropped before export '{}': drops={} failures={}",
                     metrics_exporter_.path().string(),
                     metrics_exporter_.dropped_snapshots(),
                     metrics_exporter_.failed_writes());
        return false;
    }

    void do_receive(ReceiveSlot& slot) {
        socket_.async_receive_from(
            asio::buffer(slot.bytes), slot.remote_endpoint,
            [this, &slot](std::error_code error_code, std::size_t bytes) {
                on_receive(slot, error_code, bytes);
            });
    }

    void on_receive(ReceiveSlot& slot, std::error_code error_code, std::size_t bytes) {
        if (error_code) {
            handle_receive_error(slot, error_code);
            return;
        }
        ReceiveTimingScope timing{*this};

        // Copy into the event-loop working buffer, then immediately return this
        // preallocated receive slot to the kernel before parsing or fan-out.
        bytes = std::min(bytes, recv_buf_.size());
        std::memcpy(recv_buf_.data(), slot.bytes.data(), bytes);
        remote_endpoint_ = slot.remote_endpoint;
        do_receive(slot);

        if (!message_validator::has_valid_header(bytes)) {
            return;
        }

        MsgHdr hdr{};
        std::memcpy(&hdr, recv_buf_.data(), sizeof(MsgHdr));

        if (hdr.magic == PING_MAGIC) {
            handle_ping_message(bytes);
        } else if (hdr.magic == CTRL_MAGIC) {
            handle_ctrl_message(bytes);
        } else if (hdr.magic == SECURE_CONTROL_MAGIC) {
            handle_secure_control_message(bytes);
        } else if (hdr.magic == SECURE_AUDIO_MAGIC) {
            handle_secure_audio_message(bytes);
        } else if (hdr.magic == E2E_KEY_ENVELOPE_MAGIC) {
            handle_e2e_key_envelope(bytes);
        }

    }

    // Send with optional shared_ptr to keep data alive during async operation
    void send(void* data, std::size_t len, const udp::endpoint& target,
              const std::shared_ptr<std::vector<unsigned char>>& keep_alive = nullptr) {
        const auto qos = socket_qos_.ensure_flow(socket_, target);
        if (qos.newly_configured &&
            (!qos.ok() || qos.detail.find("failed") != std::string::npos)) {
            spdlog::warn("UDP QoS not fully active for {}:{}: {}",
                      udp_network::format_address_for_display(target.address()), target.port(),
                      qos.detail);
        }

        auto send_buffer = keep_alive;
        if (send_buffer == nullptr) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            send_buffer = std::make_shared<std::vector<unsigned char>>(bytes, bytes + len);
        }

        socket_.async_send_to(asio::buffer(send_buffer->data(), send_buffer->size()), target,
                              [send_buffer](std::error_code error_code, std::size_t) {
                                  if (error_code) {
                                      spdlog::error("send error: {}", error_code.message());
                                  }
                              });
    }

    FanOutBuffer* acquire_fan_out_buffer(const void* data, std::size_t len) {
        if (data == nullptr || len > server_config::RECV_BUF_SIZE) {
            ++sfu_pool_drops_interval_;
            ++sfu_pool_drops_total_;
            return nullptr;
        }

        for (auto& buffer: fan_out_buffers_) {
            uint32_t expected = 0;
            if (!buffer.pending_workers.compare_exchange_strong(
                    expected, std::numeric_limits<uint32_t>::max(),
                    std::memory_order_acq_rel)) {
                continue;
            }
            std::memcpy(buffer.bytes.data(), data, len);
            buffer.size = len;
            buffer.admitted_at = std::chrono::steady_clock::now();
            return &buffer;
        }

        ++sfu_pool_drops_interval_;
        ++sfu_pool_drops_total_;
        return nullptr;
    }

    bool send_media_from_worker(FanOutBuffer* buffer, const udp::endpoint& target) {
        if (std::chrono::steady_clock::now() - buffer->admitted_at >
            SERVER_MEDIA_DEADLINE) {
            ++sfu_expired_media_drops_interval_;
            ++sfu_expired_media_drops_total_;
            return false;
        }
#ifdef _WIN32
        const int sent = ::sendto(
            socket_.native_handle(), reinterpret_cast<const char*>(buffer->bytes.data()),
            static_cast<int>(buffer->size), 0, target.data(), static_cast<int>(target.size()));
        const bool sent_all = sent == static_cast<int>(buffer->size);
#else
        const auto sent = ::sendto(socket_.native_handle(), buffer->bytes.data(), buffer->size,
                                   0, target.data(), target.size());
        const bool sent_all = sent == static_cast<decltype(sent)>(buffer->size);
#endif
        if (sent_all) {
            return true;
        }
        ++sfu_send_cap_drops_interval_;
        const auto drops = ++sfu_send_cap_drops_total_;
        if (drops == 1 || drops % 100 == 0) {
            spdlog::warn("SFU media send failed (drops={})", drops);
        }
        return false;
    }

    void complete_media_task(FanOutBuffer* buffer) {
        uint32_t pending = buffer->pending_workers.load(std::memory_order_acquire);
        for (;;) {
            if (pending > 1 && pending != std::numeric_limits<uint32_t>::max()) {
                if (buffer->pending_workers.compare_exchange_weak(
                        pending, pending - 1, std::memory_order_acq_rel)) {
                    return;
                }
                continue;
            }
            if (pending == 1 && buffer->pending_workers.compare_exchange_weak(
                                    pending, std::numeric_limits<uint32_t>::max(),
                                    std::memory_order_acq_rel)) {
                break;
            }
        }
        relay_dwell_latency_.observe(std::chrono::steady_clock::now() - buffer->admitted_at);
        buffer->size = 0;
        buffer->pending_workers.store(0, std::memory_order_release);
    }

    void media_worker_loop(size_t worker_index) {
        auto& worker = media_workers_[worker_index];
        for (;;) {
            worker.ready.acquire();
            MediaTask task;
            if (!worker.queue.try_pop(task)) {
                if (!worker.running.load(std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if (task.buffer != nullptr && task.endpoints != nullptr) {
                for (size_t i = worker_index; i < task.endpoints->size();
                     i += MEDIA_WORKER_COUNT) {
                    const auto& endpoint = (*task.endpoints)[i];
                    if (endpoint != task.sender) {
                        (void)send_media_from_worker(task.buffer, endpoint);
                    }
                }
                complete_media_task(task.buffer);
            }
        }
    }

    void start_media_workers() {
        try {
            for (size_t i = 0; i < media_workers_.size(); ++i) {
                auto& worker = media_workers_[i];
                worker.running.store(true, std::memory_order_release);
                worker.thread = std::thread([this, i]() { media_worker_loop(i); });
            }
        } catch (...) {
            stop_media_workers();
            throw;
        }
    }

    void stop_media_workers() {
        for (auto& worker: media_workers_) {
            if (worker.running.exchange(false, std::memory_order_acq_rel)) {
                worker.ready.release();
            }
        }
        for (auto& worker: media_workers_) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
        }
    }

private:
    struct UnknownEndpointInfo {
        std::chrono::steady_clock::time_point first_seen;
        std::chrono::steady_clock::time_point last_seen;
        std::chrono::steady_clock::time_point last_join_required_sent;
        uint64_t                              drops = 0;
        bool                                  first_log_emitted = false;
    };

    struct UsedTokenNonce {
        udp::endpoint endpoint;
        int64_t       expires_at_ms = 0;
        std::string   room_id;
        std::string   profile_id;
    };

    struct AudioIngressStats {
        udp::endpoint endpoint;
        uint64_t received_total = 0;
        uint64_t sequence_gaps_total = 0;
        uint64_t sequence_gap_recoveries_total = 0;
        uint64_t sequence_unresolved_gaps = 0;
        uint64_t sequence_late_or_reordered_total = 0;
        uint64_t received_interval = 0;
        uint64_t sequence_gaps_interval = 0;
        uint64_t sequence_gap_recoveries_interval = 0;
        uint64_t sequence_late_or_reordered_interval = 0;
        uint16_t last_frame_count = 0;
        SequenceArrivalTracker sequence_tracker;
    };

    struct PingStats {
        udp::endpoint endpoint;
        uint64_t received_total = 0;
        uint64_t reply_queued_total = 0;
        uint64_t sequence_gaps_total = 0;
        uint64_t sequence_gap_recoveries_total = 0;
        uint64_t sequence_unresolved_gaps = 0;
        uint64_t sequence_late_or_reordered_total = 0;
        uint64_t received_interval = 0;
        uint64_t reply_queued_interval = 0;
        uint64_t sequence_gaps_interval = 0;
        uint64_t sequence_gap_recoveries_interval = 0;
        uint64_t sequence_late_or_reordered_interval = 0;
        SequenceArrivalTracker sequence_tracker;
    };

    void handle_receive_error(ReceiveSlot& slot, std::error_code error_code) {
        if (error_code == asio::error::operation_aborted) {
            return;
        }

        spdlog::warn("UDP receive error: {}; keeping participants registered",
                  error_code.message());
        do_receive(slot);  // keep this receive slot listening
    }

    void handle_ping_message(std::size_t bytes) {
        if (!message_validator::is_valid_ping(bytes) || !client_manager_.exists(remote_endpoint_)) {
            return;
        }

        SyncHdr shdr{};
        std::memcpy(&shdr, recv_buf_.data(), sizeof(SyncHdr));
        const auto now = std::chrono::steady_clock::now();
        if (!rate_limiter_.allow_control(remote_endpoint_, now)) {
            return;
        }
        const uint32_t client_id = client_manager_.get_client_id(remote_endpoint_);
        client_manager_.update_alive(remote_endpoint_, now);
        record_ping_received(client_id, remote_endpoint_, shdr.seq);
        auto nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        shdr.t2_server_recv = nanoseconds;
        shdr.t3_server_send = nanoseconds;
        auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(SyncHdr));
        std::memcpy(packet->data(), &shdr, sizeof(SyncHdr));
        record_ping_reply_queued(client_id);
        send(packet->data(), packet->size(), remote_endpoint_, packet);
    }

    void handle_ctrl_message(std::size_t bytes) {
        if (bytes < sizeof(CtrlHdr)) {
            return;
        }

        CtrlHdr chdr{};
        std::memcpy(&chdr, recv_buf_.data(), sizeof(CtrlHdr));

        auto now = std::chrono::steady_clock::now();
        const bool known_endpoint = client_manager_.exists(remote_endpoint_);
        if (chdr.type == CtrlHdr::Cmd::SERVER_STATUS_REQUEST) {
            if (!rate_limiter_.allow_status(remote_endpoint_, now)) {
                return;
            }
        } else if (chdr.type != CtrlHdr::Cmd::JOIN) {
            const bool allowed =
                known_endpoint ? rate_limiter_.allow_control(remote_endpoint_, now)
                               : rate_limiter_.allow_room_control(remote_endpoint_, now);
            if (!allowed) {
                return;
            }
        }

        switch (chdr.type) {
            case CtrlHdr::Cmd::JOIN:
                handle_join(bytes, now);
                break;
            case CtrlHdr::Cmd::SERVER_STATUS_REQUEST:
                handle_server_status_request(bytes);
                break;
            case CtrlHdr::Cmd::ROOM_CREATE_REQUEST:
                handle_room_create_request(bytes, now);
                break;
            case CtrlHdr::Cmd::ROOM_JOIN_TOKEN_REQUEST:
                handle_room_join_token_request(bytes, now);
                break;
            case CtrlHdr::Cmd::ROOM_ADMIN_REQUEST:
                handle_room_admin_request(bytes, now);
                break;
            case CtrlHdr::Cmd::ROOM_CHAT_SEND:
                handle_room_chat_send(bytes, now);
                break;
            case CtrlHdr::Cmd::ROOM_CHAT_HISTORY_REQUEST:
                handle_room_chat_history_request(bytes, now);
                break;
            case CtrlHdr::Cmd::ROOM_KEY_HANDOFF_ACK:
                handle_room_key_handoff_ack(bytes, now);
                break;
            case CtrlHdr::Cmd::LEAVE: {
                spdlog::info("Client LEAVE: {}:{}", remote_endpoint_.address().to_string(),
                          remote_endpoint_.port());
                auto leaving_client = client_manager_.remove_client_with_info(remote_endpoint_);
                if (leaving_client.has_value()) {
                    release_token_nonce_for_client(*leaving_client);
                    broadcast_participant_leave(leaving_client->client_id);
                    cleanup_empty_rooms(now);
                }
                break;
            }
            case CtrlHdr::Cmd::ALIVE: {
                if (client_manager_.exists(remote_endpoint_)) {
                    client_manager_.update_alive(remote_endpoint_, now);
                } else {
                    send_join_required(remote_endpoint_);
                }
                break;
            }
            case CtrlHdr::Cmd::PARTICIPANT_LEAVE:
                // Clients shouldn't send this, only server broadcasts it
                spdlog::warn("Client sent PARTICIPANT_LEAVE (should only come from server)");
                break;
            case CtrlHdr::Cmd::METRONOME_SYNC:
                handle_metronome_sync(bytes, now);
                break;
            default:
                spdlog::warn("Unknown CTRL cmd: {} from {}:{}", static_cast<int>(chdr.type),
                          remote_endpoint_.address().to_string(), remote_endpoint_.port());
                break;
        }
    }

    static uint8_t room_flags(const room_registry::RoomSnapshot& room) {
        return room.access_mode == ROOM_ACCESS_PASSWORD ? ROOM_FLAG_LOCKED : 0;
    }

    static uint16_t saturating_u16(size_t value) {
        return static_cast<uint16_t>(
            std::min<size_t>(value, std::numeric_limits<uint16_t>::max()));
    }

    bool can_issue_udp_room_join_ticket() const {
        return !options_.join_secret.empty();
    }

    std::optional<std::string> create_udp_room_join_ticket(
        const room_registry::RoomSnapshot& room, const std::string& profile_id) const {
        if (options_.join_secret.empty()) {
            return std::nullopt;
        }

        performer_join_token::Claims claims;
        claims.expires_at_ms = performer_join_token::now_ms() + ROOM_JOIN_TICKET_TTL_MS;
        claims.server_id = options_.server_id;
        claims.room_id = room.room_id;
        claims.profile_id = profile_id;
        claims.room_instance_id = room.room_instance_id;
        claims.access_epoch = room.access_epoch;
        claims.media_key_commitment = room.media_key_commitment;
        claims.nonce = performer_join_token::random_nonce();
        return performer_join_token::create(claims, options_.join_secret);
    }

    void cleanup_empty_rooms(std::chrono::steady_clock::time_point now) {
        room_registry_.remove_empty_rooms(client_manager_.room_counts(), now);
    }

    void handle_server_status_request(std::size_t bytes) {
        if (bytes < sizeof(ServerStatusRequestHdr)) {
            return;
        }
        ServerStatusRequestHdr request{};
        std::memcpy(&request, recv_buf_.data(), sizeof(ServerStatusRequestHdr));

        cleanup_empty_rooms(std::chrono::steady_clock::now());
        const auto counts = client_manager_.room_counts();
        const auto rooms = room_registry_.list_rooms(counts);

        ServerStatusResponseHdr response{};
        response.magic = CTRL_MAGIC;
        response.type = CtrlHdr::Cmd::SERVER_STATUS_RESPONSE;
        response.request_id = request.request_id;
        packet_builder::write_fixed(response.server_id, options_.server_id);
        response.total_rooms = saturating_u16(rooms.size());
        response.active_participants = saturating_u16(client_manager_.count());
        const size_t room_offset =
            std::min<size_t>(request.room_offset, rooms.size());
        const size_t room_limit =
            request.room_limit == 0
                ? MAX_ROOM_STATUS_SUMMARIES
                : std::min<size_t>(request.room_limit, MAX_ROOM_STATUS_SUMMARIES);
        const size_t remaining_rooms = rooms.size() - room_offset;
        response.room_count = static_cast<uint8_t>(
            std::min(remaining_rooms, room_limit));
        response.room_offset = saturating_u16(room_offset);
        response.truncated =
            room_offset + response.room_count < rooms.size() ? 1 : 0;
        response.token_auth_available = can_issue_udp_room_join_ticket() ? 1 : 0;
        for (size_t index = 0; index < response.room_count; ++index) {
            const auto& room = rooms[room_offset + index];
            packet_builder::write_fixed(response.rooms[index].room_id, room.room_id);
            packet_builder::write_fixed(response.rooms[index].room_name, room.room_name);
            packet_builder::write_fixed(response.rooms[index].room_instance_id,
                                        room.room_instance_id);
            response.rooms[index].access_epoch = room.access_epoch;
            response.rooms[index].participant_count =
                saturating_u16(room.participant_count);
            response.rooms[index].flags = room_flags(room);
            response.rooms[index].access_mode = room.access_mode;
        }

        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(response));
        std::memcpy(buf->data(), &response, sizeof(response));
        send(buf->data(), buf->size(), remote_endpoint_, buf);
    }

    void send_room_create_response(const RoomCreateResponseHdr& response) {
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(response));
        std::memcpy(buf->data(), &response, sizeof(response));
        send(buf->data(), buf->size(), remote_endpoint_, buf);
    }

    void handle_room_create_request(std::size_t bytes,
                                    std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(RoomCreateRequestHdr)) {
            return;
        }
        RoomCreateRequestHdr request{};
        std::memcpy(&request, recv_buf_.data(), sizeof(RoomCreateRequestHdr));
        const std::string room_id = fixed_string(request.room_id);
        const std::string room_name = fixed_string(request.room_name);
        const std::string profile_id = fixed_string(request.profile_id);
        const std::string password_hash = fixed_string(request.password_hash);
        const std::string media_key_commitment =
            fixed_string(request.media_key_commitment);
        const uint8_t access_mode = request.access_mode;

        RoomCreateResponseHdr response{};
        response.magic = CTRL_MAGIC;
        response.type = CtrlHdr::Cmd::ROOM_CREATE_RESPONSE;
        response.request_id = request.request_id;

        if (profile_id.empty()) {
            response.status = ROOM_STATUS_BAD_REQUEST;
            packet_builder::write_fixed(response.reason, "missing profile id");
            send_room_create_response(response);
            return;
        }
        if (!can_issue_udp_room_join_ticket()) {
            response.status = ROOM_STATUS_FORBIDDEN;
            packet_builder::write_fixed(response.reason,
                                        "room tickets require a join secret");
            send_room_create_response(response);
            return;
        }

        auto result =
            room_registry_.create_room(room_id, room_name, password_hash,
                                       access_mode, media_key_commitment, now);
        if (!result.ok) {
            response.status = result.reason == "room already exists"
                                  ? ROOM_STATUS_CONFLICT
                                  : ROOM_STATUS_BAD_REQUEST;
            response.flags = room_flags(result.room);
            response.access_mode = result.room.access_mode;
            response.access_epoch = result.room.access_epoch;
            packet_builder::write_fixed(response.room_id, result.room.room_id);
            packet_builder::write_fixed(response.room_name, result.room.room_name);
            packet_builder::write_fixed(response.room_instance_id,
                                        result.room.room_instance_id);
            packet_builder::write_fixed(response.reason, result.reason);
            send_room_create_response(response);
            return;
        }

        const auto ticket = create_udp_room_join_ticket(result.room, profile_id);
        if (!ticket.has_value()) {
            response.status = ROOM_STATUS_SERVER_ERROR;
            packet_builder::write_fixed(response.reason, "could not issue join token");
            send_room_create_response(response);
            return;
        }
        if (!packet_builder::write_fixed_checked(response.join_token, *ticket)) {
            response.status = ROOM_STATUS_SERVER_ERROR;
            packet_builder::write_fixed(response.reason,
                                        "join token exceeds wire limit");
            send_room_create_response(response);
            return;
        }

        response.status = ROOM_STATUS_OK;
        response.flags = room_flags(result.room) | ROOM_FLAG_CREATED;
        response.access_mode = result.room.access_mode;
        response.access_epoch = result.room.access_epoch;
        packet_builder::write_fixed(response.room_id, result.room.room_id);
        packet_builder::write_fixed(response.room_name, result.room.room_name);
        packet_builder::write_fixed(response.room_instance_id, result.room.room_instance_id);
        packet_builder::write_fixed(response.admin_token, result.admin_token);
        send_room_create_response(response);
    }

    void send_room_join_token_response(const RoomJoinTokenResponseHdr& response) {
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(response));
        std::memcpy(buf->data(), &response, sizeof(response));
        send(buf->data(), buf->size(), remote_endpoint_, buf);
    }

    void handle_room_join_token_request(std::size_t bytes,
                                        std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(RoomJoinTokenRequestHdr)) {
            return;
        }
        RoomJoinTokenRequestHdr request{};
        std::memcpy(&request, recv_buf_.data(), sizeof(RoomJoinTokenRequestHdr));
        const std::string room_id = fixed_string(request.room_id);
        const std::string profile_id = fixed_string(request.profile_id);
        const std::string password_hash = fixed_string(request.password_hash);

        RoomJoinTokenResponseHdr response{};
        response.magic = CTRL_MAGIC;
        response.type = CtrlHdr::Cmd::ROOM_JOIN_TOKEN_RESPONSE;
        response.request_id = request.request_id;

        if (profile_id.empty()) {
            response.status = ROOM_STATUS_BAD_REQUEST;
            packet_builder::write_fixed(response.reason, "missing profile id");
            send_room_join_token_response(response);
            return;
        }
        if (!can_issue_udp_room_join_ticket()) {
            response.status = ROOM_STATUS_FORBIDDEN;
            packet_builder::write_fixed(response.reason,
                                        "room tickets require a join secret");
            send_room_join_token_response(response);
            return;
        }

        const auto authorized =
            room_registry_.authorize_join(room_id, password_hash, now);
        if (!authorized.ok) {
            response.status = authorized.reason == "room not found"
                                  ? ROOM_STATUS_NOT_FOUND
                                  : ROOM_STATUS_FORBIDDEN;
            packet_builder::write_fixed(response.reason, authorized.reason);
            send_room_join_token_response(response);
            return;
        }

        const auto ticket = create_udp_room_join_ticket(authorized.room, profile_id);
        if (!ticket.has_value()) {
            response.status = ROOM_STATUS_SERVER_ERROR;
            packet_builder::write_fixed(response.reason, "could not issue join token");
            send_room_join_token_response(response);
            return;
        }
        if (!packet_builder::write_fixed_checked(response.join_token, *ticket)) {
            response.status = ROOM_STATUS_SERVER_ERROR;
            packet_builder::write_fixed(response.reason,
                                        "join token exceeds wire limit");
            send_room_join_token_response(response);
            return;
        }

        response.status = ROOM_STATUS_OK;
        response.flags = room_flags(authorized.room);
        response.access_mode = authorized.room.access_mode;
        response.access_epoch = authorized.room.access_epoch;
        packet_builder::write_fixed(response.room_id, authorized.room.room_id);
        packet_builder::write_fixed(response.room_name, authorized.room.room_name);
        packet_builder::write_fixed(response.room_instance_id,
                                    authorized.room.room_instance_id);
        send_room_join_token_response(response);
    }

    void send_room_admin_response(const RoomAdminResponseHdr& response) {
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(response));
        std::memcpy(buf->data(), &response, sizeof(response));
        send(buf->data(), buf->size(), remote_endpoint_, buf);
    }

    void handle_room_admin_request(std::size_t bytes,
                                   std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(RoomAdminRequestHdr)) {
            return;
        }
        RoomAdminRequestHdr request{};
        std::memcpy(&request, recv_buf_.data(), sizeof(RoomAdminRequestHdr));
        const std::string room_id = fixed_string(request.room_id);
        const std::string admin_token = fixed_string(request.admin_token);
        const std::string password_hash = fixed_string(request.password_hash);
        const std::string media_key_commitment =
            fixed_string(request.media_key_commitment);

        RoomAdminResponseHdr response{};
        response.magic = CTRL_MAGIC;
        response.type = CtrlHdr::Cmd::ROOM_ADMIN_RESPONSE;
        response.request_id = request.request_id;
        response.target_participant_id = request.target_participant_id;
        packet_builder::write_fixed(response.room_id, room_id);

        const bool rotates_media_key =
            request.command == ROOM_ADMIN_CHANGE_PASSWORD ||
            request.command == ROOM_ADMIN_CHANGE_ACCESS ||
            request.command == ROOM_ADMIN_KICK_PARTICIPANT ||
            request.command == ROOM_ADMIN_ROTATE_MEDIA_KEY;
        if (rotates_media_key &&
            !performer_join_token::valid_sha256_hex(media_key_commitment)) {
            response.status = ROOM_STATUS_BAD_REQUEST;
            packet_builder::write_fixed(response.reason,
                                        "invalid media key commitment");
            send_room_admin_response(response);
            return;
        }

        room_registry::AuthorizeResult authorized;
        switch (request.command) {
            case ROOM_ADMIN_CHANGE_PASSWORD:
                authorized =
                    room_registry_.change_password(room_id, admin_token, password_hash,
                                                   media_key_commitment, now);
                break;
            case ROOM_ADMIN_CHANGE_ACCESS:
                authorized = room_registry_.change_access_mode(
                    room_id, admin_token, request.access_mode, password_hash,
                    media_key_commitment, now);
                break;
            case ROOM_ADMIN_KICK_PARTICIPANT:
                authorized = room_registry_.authorize_admin(room_id, admin_token, now);
                if (authorized.ok) {
                    if (kick_room_participant(room_id, request.target_participant_id, now,
                                              &response)) {
                        authorized =
                            room_registry_.rotate_access_epoch(
                                room_id, admin_token, media_key_commitment, now);
                    }
                }
                break;
            case ROOM_ADMIN_CLOSE_ROOM:
                authorized = room_registry_.close_room(room_id, admin_token, now);
                if (authorized.ok) {
                    remove_all_room_participants(room_id, now);
                }
                break;
            case ROOM_ADMIN_ROTATE_MEDIA_KEY:
                authorized = room_registry_.rotate_access_epoch(
                    room_id, admin_token, media_key_commitment, now);
                break;
            default:
                response.status = ROOM_STATUS_BAD_REQUEST;
                packet_builder::write_fixed(response.reason, "unknown admin command");
                send_room_admin_response(response);
                return;
        }

        if (!authorized.ok) {
            response.status = authorized.reason == "room not found" ? ROOM_STATUS_NOT_FOUND
                                                                    : ROOM_STATUS_FORBIDDEN;
            packet_builder::write_fixed(response.reason, authorized.reason);
            send_room_admin_response(response);
            return;
        }
        if (response.status != ROOM_STATUS_OK) {
            send_room_admin_response(response);
            return;
        }

        response.status = ROOM_STATUS_OK;
        response.flags = room_flags(authorized.room);
        response.access_mode = authorized.room.access_mode;
        response.access_epoch = authorized.room.access_epoch;
        client_manager_.set_room_access_epoch(
            authorized.room.room_id, authorized.room.room_instance_id,
            authorized.room.access_epoch);
        packet_builder::write_fixed(response.reason, "ok");
        send_room_admin_response(response);
    }

    static int64_t system_epoch_ms() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    static void fill_chat_event(RoomChatEventHdr& event,
                                const room_registry::ChatMessage& message,
                                uint32_t request_id) {
        event.magic = CTRL_MAGIC;
        event.type = CtrlHdr::Cmd::ROOM_CHAT_EVENT;
        event.participant_id = message.sender_participant_id;
        event.request_id = request_id;
        event.access_epoch = message.access_epoch;
        event.chat_sequence = message.sequence;
        event.server_time_ms = message.server_time_ms;
        event.ciphertext_bytes = message.ciphertext_bytes;
        packet_builder::write_fixed(event.room_id, message.room_id);
        packet_builder::write_fixed(event.room_instance_id, message.room_instance_id);
        event.nonce = message.nonce;
        event.ciphertext = message.ciphertext;
    }

    void send_room_chat_rejected(uint32_t request_id,
                                 const Bytes<SECURE_PACKET_NONCE_BYTES>& nonce,
                                 uint8_t status,
                                 const std::string& reason) {
        RoomChatSendRejectedHdr response{};
        response.magic = CTRL_MAGIC;
        response.type = CtrlHdr::Cmd::ROOM_CHAT_SEND_REJECTED;
        response.request_id = request_id;
        response.status = status;
        response.nonce = nonce;
        packet_builder::write_fixed(response.reason, reason);
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(response));
        std::memcpy(buf->data(), &response, sizeof(response));
        send(buf->data(), buf->size(), remote_endpoint_, buf);
    }

    void handle_room_chat_send(std::size_t bytes,
                               std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(RoomChatSendHdr)) {
            send_room_chat_rejected(0, {}, ROOM_STATUS_BAD_REQUEST,
                                    "short chat message");
            return;
        }
        if (!rate_limiter_.allow_chat_send(remote_endpoint_, now)) {
            return;
        }

        RoomChatSendHdr request{};
        std::memcpy(&request, recv_buf_.data(), sizeof(request));
        const auto sender = client_manager_.get_client_info(remote_endpoint_);
        if (!sender.has_value()) {
            send_room_chat_rejected(request.request_id, request.nonce,
                                    ROOM_STATUS_FORBIDDEN, "not joined");
            return;
        }
        const std::string room_id = fixed_string(request.room_id);
        const std::string room_instance_id = fixed_string(request.room_instance_id);
        if (room_id != sender->room_id ||
            room_instance_id != sender->room_instance_id ||
            request.participant_id != sender->client_id ||
            request.access_epoch != sender->access_epoch) {
            send_room_chat_rejected(request.request_id, request.nonce,
                                    ROOM_STATUS_FORBIDDEN, "wrong room chat sender");
            return;
        }

        const auto stored = room_registry_.store_chat_message(
            room_id, room_instance_id, request.access_epoch, request.participant_id,
            request.nonce, request.ciphertext, request.ciphertext_bytes,
            system_epoch_ms(), now);
        if (!stored.ok) {
            send_room_chat_rejected(request.request_id, request.nonce,
                                    stored.status, stored.reason);
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);
        RoomChatEventHdr event{};
        fill_chat_event(event, stored.message, request.request_id);
        auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(event));
        std::memcpy(packet->data(), &event, sizeof(event));
        for (const auto& [endpoint, _]: client_manager_.get_room_clients(room_id)) {
            send(packet->data(), packet->size(), endpoint, packet);
        }
    }

    void send_room_chat_history_done(uint32_t request_id,
                                     const std::string& room_id,
                                     const std::string& room_instance_id,
                                     uint32_t access_epoch,
                                     uint8_t status,
                                     uint8_t flags) {
        RoomChatHistoryResponseHdr response{};
        response.magic = CTRL_MAGIC;
        response.type = CtrlHdr::Cmd::ROOM_CHAT_HISTORY_RESPONSE;
        response.request_id = request_id;
        response.status = status;
        response.flags = static_cast<uint8_t>(flags | ROOM_CHAT_HISTORY_DONE);
        response.access_epoch = access_epoch;
        packet_builder::write_fixed(response.room_id, room_id);
        packet_builder::write_fixed(response.room_instance_id, room_instance_id);
        auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(response));
        std::memcpy(packet->data(), &response, sizeof(response));
        send(packet->data(), packet->size(), remote_endpoint_, packet);
    }

    void handle_room_chat_history_request(std::size_t bytes,
                                          std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(RoomChatHistoryRequestHdr)) {
            send_room_chat_history_done(0, {}, {}, 0, ROOM_STATUS_BAD_REQUEST,
                                        ROOM_CHAT_HISTORY_DONE);
            return;
        }

        RoomChatHistoryRequestHdr request{};
        std::memcpy(&request, recv_buf_.data(), sizeof(request));
        const auto client = client_manager_.get_client_info(remote_endpoint_);
        const std::string room_id = fixed_string(request.room_id);
        const std::string room_instance_id = fixed_string(request.room_instance_id);
        if (!client.has_value() ||
            room_id != client->room_id ||
            room_instance_id != client->room_instance_id ||
            request.access_epoch != client->access_epoch) {
            send_room_chat_history_done(request.request_id, room_id, room_instance_id,
                                        request.access_epoch, ROOM_STATUS_FORBIDDEN, 0);
            return;
        }

        const auto history = room_registry_.chat_history_since(
            room_id, room_instance_id, request.access_epoch,
            request.after_sequence, now);
        if (!history.ok) {
            send_room_chat_history_done(request.request_id, room_id, room_instance_id,
                                        request.access_epoch, history.status, 0);
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);
        for (const auto& message: history.messages) {
            RoomChatHistoryResponseHdr response{};
            response.magic = CTRL_MAGIC;
            response.type = CtrlHdr::Cmd::ROOM_CHAT_HISTORY_RESPONSE;
            response.participant_id = message.sender_participant_id;
            response.request_id = request.request_id;
            response.status = ROOM_STATUS_OK;
            response.access_epoch = message.access_epoch;
            response.chat_sequence = message.sequence;
            response.server_time_ms = message.server_time_ms;
            response.ciphertext_bytes = message.ciphertext_bytes;
            packet_builder::write_fixed(response.room_id, message.room_id);
            packet_builder::write_fixed(response.room_instance_id, message.room_instance_id);
            response.nonce = message.nonce;
            response.ciphertext = message.ciphertext;
            auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(response));
            std::memcpy(packet->data(), &response, sizeof(response));
            send(packet->data(), packet->size(), remote_endpoint_, packet);
        }
        send_room_chat_history_done(
            request.request_id, room_id, room_instance_id, request.access_epoch,
            ROOM_STATUS_OK, history.truncated ? ROOM_CHAT_HISTORY_TRUNCATED : 0);
    }

    bool kick_room_participant(const std::string& room_id, uint32_t participant_id,
                               std::chrono::steady_clock::time_point now,
                               RoomAdminResponseHdr* response) {
        if (participant_id == 0) {
            if (response != nullptr) {
                response->status = ROOM_STATUS_BAD_REQUEST;
                packet_builder::write_fixed(response->reason, "missing participant id");
            }
            return false;
        }

        const auto removed =
            client_manager_.remove_room_client_with_info(room_id, participant_id);
        if (!removed.has_value()) {
            if (response != nullptr) {
                response->status = ROOM_STATUS_NOT_FOUND;
                packet_builder::write_fixed(response->reason, "participant not found");
            }
            return false;
        }

        send_room_removed(removed->first, removed->second.client_id);
        release_token_nonce_for_client(removed->second);
        rate_limiter_.erase(removed->first);
        broadcast_participant_leave(removed->second.client_id);
        cleanup_empty_rooms(now);
        return true;
    }

    void remove_all_room_participants(const std::string& room_id,
                                      std::chrono::steady_clock::time_point now) {
        const auto removed = client_manager_.remove_room_clients_with_info(room_id);
        for (const auto& [endpoint, info]: removed) {
            send_room_removed(endpoint, info.client_id);
            release_token_nonce_for_client(info);
            rate_limiter_.erase(endpoint);
            broadcast_participant_leave(info.client_id);
        }
        cleanup_empty_rooms(now);
    }

    void cleanup_used_token_nonces() {
        const int64_t now_ms = performer_join_token::now_ms();
        for (auto it = used_token_nonces_.begin(); it != used_token_nonces_.end();) {
            if (it->second.expires_at_ms < now_ms) {
                it = used_token_nonces_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool reserve_token_nonce(const performer_join_token::ValidatedToken& token,
                             const udp::endpoint& endpoint) {
        cleanup_used_token_nonces();
        const std::string nonce_key = session_crypto::nonce_replay_key(token.claims);
        auto it = used_token_nonces_.find(nonce_key);
        if (it != used_token_nonces_.end() && it->second.endpoint != endpoint &&
            client_manager_.exists(it->second.endpoint)) {
            return false;
        }

        used_token_nonces_[nonce_key] = UsedTokenNonce{
            endpoint,
            token.claims.expires_at_ms,
            token.claims.room_id,
            token.claims.profile_id,
        };
        return true;
    }

    void release_token_nonce_for_client(const ClientInfo& client) {
        if (!client.has_authenticated_session || client.token_nonce_key.empty()) {
            return;
        }
        used_token_nonces_.erase(client.token_nonce_key);
    }

    void handle_join(std::size_t bytes, std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(JoinHdr)) {
            spdlog::warn("Rejecting JOIN from {}:{}: packet too small",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port());
            return;
        }

        JoinHdr join{};
        std::memcpy(&join, recv_buf_.data(), std::min(bytes, sizeof(JoinHdr)));

        const std::string room_id      = fixed_string(join.room_id);
        const std::string profile_id   = fixed_string(join.profile_id);
        const std::string display_name = fixed_string(join.display_name);
        const std::string token        = fixed_string(join.join_token);
        const uint32_t client_capabilities = join.capabilities & AUDIO_SUPPORTED_CAPABILITIES;

        if (room_id.empty() || profile_id.empty()) {
            spdlog::warn("Rejecting JOIN from {}:{}: missing room or profile id",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port());
            return;
        }
        if (token.empty()) {
            spdlog::warn("Rejecting JOIN from {}:{} room '{}': missing token",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port(), room_id);
            return;
        }
        const auto validated_token = performer_join_token::validate_with_claims(
            token, options_.join_secret, options_.server_id, room_id, profile_id);
        if (!validated_token.ok) {
            spdlog::warn("Rejecting JOIN from {}:{} room '{}': {}",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port(), room_id,
                      validated_token.reason);
            return;
        }

        room_registry::RoomSnapshot room_snapshot;
        if (!validated_token.claims.room_instance_id.empty()) {
            const auto room_check = room_registry_.validate_claims(
                room_id, validated_token.claims.room_instance_id,
                validated_token.claims.access_epoch,
                validated_token.claims.media_key_commitment, now);
            if (!room_check.ok) {
                spdlog::warn("Rejecting JOIN from {}:{} room '{}': {}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                          room_id, room_check.reason);
                return;
            }
            room_snapshot = room_check.room;
        } else {
            room_snapshot = room_registry_.ensure_open_room(
                room_id, validated_token.claims.media_key_commitment, now);
            if (room_snapshot.room_id.empty()) {
                spdlog::warn(
                    "Rejecting JOIN from {}:{} room '{}': wrong media key commitment",
                    remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                    room_id);
                return;
            }
        }

        if (!reserve_token_nonce(validated_token, remote_endpoint_)) {
            spdlog::warn("Rejecting JOIN from {}:{} room '{}': token nonce replay",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port(), room_id);
            return;
        }
        ClientManager::ClientSecurityConfig security;
        security.token_nonce_key = session_crypto::nonce_replay_key(validated_token.claims);

        const uint32_t registered_capabilities =
            client_capabilities | AUDIO_CAP_SECURE_AUDIO;

        auto registration = client_manager_.register_client(
            remote_endpoint_, now, room_id, room_snapshot.room_instance_id,
            room_snapshot.access_epoch, profile_id, display_name,
            registered_capabilities, join.key_public,
            std::optional<ClientManager::ClientSecurityConfig>{security});
        if (registration.rejected_room_full) {
            spdlog::warn("Rejecting JOIN from {}:{} room '{}': room is full ({} participants max)",
                         remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                         room_id, MAX_ROOM_PARTICIPANTS);
            JoinDeniedHdr denied{};
            denied.magic = CTRL_MAGIC;
            denied.type = CtrlHdr::Cmd::JOIN_DENIED;
            denied.reason = 1;
            send(&denied, sizeof(denied), remote_endpoint_);
            return;
        }
        for (uint32_t removed_client_id: registration.removed_client_ids) {
            spdlog::info("Removed stale duplicate participant ID {} for room='{}' user='{}'",
                      removed_client_id, room_id, profile_id);
            broadcast_participant_leave(removed_client_id);
        }

        uint32_t client_id = registration.client_id;
        room_registry_.mark_joined(room_snapshot.room_id, now);
        spdlog::info(
                  "JOIN: {}:{} room='{}' user='{}' display='{}' "
                  "(ID: {}, {}, capabilities=0x{:08x})",
                  remote_endpoint_.address().to_string(), remote_endpoint_.port(), room_id,
                  profile_id, display_name, client_id,
                  "token-present", registered_capabilities);
        send_join_ack(remote_endpoint_, client_id, AUDIO_SUPPORTED_CAPABILITIES);
        broadcast_participant_info(remote_endpoint_, client_id, profile_id, display_name);
        send_existing_participant_info_to(remote_endpoint_);
    }

    void handle_e2e_key_envelope(std::size_t bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (bytes < sizeof(E2EKeyEnvelopeHdr)) {
            rate_limiter_.allow_strict(remote_endpoint_, now);
            return;
        }

        if (!client_manager_.exists(remote_endpoint_) ||
            !client_manager_.has_authenticated_session(remote_endpoint_)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping E2E key envelope from unauthenticated endpoint {}:{}",
                             remote_endpoint_.address().to_string(),
                             remote_endpoint_.port());
            }
            return;
        }

        E2EKeyEnvelopeHdr envelope{};
        std::memcpy(&envelope, recv_buf_.data(), sizeof(envelope));
        const uint32_t sender_id = client_manager_.get_client_id(remote_endpoint_);
        if (envelope.sender_id == 0 || envelope.sender_id != sender_id ||
            envelope.target_id == 0 || envelope.reserved != 0 ||
            envelope.access_epoch == 0 ||
            envelope.encrypted_bytes == 0 ||
            envelope.encrypted_bytes > E2E_KEY_ENVELOPE_MAX_BYTES) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping malformed E2E key envelope from {}:{}",
                             remote_endpoint_.address().to_string(),
                             remote_endpoint_.port());
            }
            return;
        }

        const auto sender = client_manager_.get_client_info(remote_endpoint_);
        const std::string media_key_commitment(
            envelope.media_key_commitment.begin(),
            envelope.media_key_commitment.end());
        if (!sender.has_value() || sender->access_epoch != envelope.access_epoch ||
            !room_registry_.authorizes_media_key(
                sender->room_id, sender->room_instance_id,
                envelope.access_epoch, media_key_commitment)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn(
                    "Dropping unauthorized E2E key envelope sender={} epoch={}",
                    envelope.sender_id, envelope.access_epoch);
            }
            return;
        }

        const auto target =
            client_manager_.get_room_endpoint_by_client_id(remote_endpoint_,
                                                           envelope.target_id);
        if (!target.has_value() ||
            !client_manager_.has_authenticated_session(*target)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping E2E key envelope sender={} target={} not in room",
                             envelope.sender_id, envelope.target_id);
            }
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);
        auto packet_copy = std::make_shared<std::vector<unsigned char>>(
            recv_buf_.data(), recv_buf_.data() + sizeof(E2EKeyEnvelopeHdr));
        send(packet_copy->data(), packet_copy->size(), *target, packet_copy);
    }

    void handle_room_key_handoff_ack(
        std::size_t bytes, std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(RoomKeyHandoffAckHdr) ||
            !client_manager_.exists(remote_endpoint_) ||
            !client_manager_.has_authenticated_session(remote_endpoint_)) {
            return;
        }

        RoomKeyHandoffAckHdr ack{};
        std::memcpy(&ack, recv_buf_.data(), sizeof(ack));
        const uint32_t sender_id = client_manager_.get_client_id(remote_endpoint_);
        const auto sender = client_manager_.get_client_info(remote_endpoint_);
        const std::string media_key_commitment(
            ack.media_key_commitment.begin(),
            ack.media_key_commitment.end());
        if (sender_id == 0 || ack.participant_id != sender_id ||
            ack.target_id == 0 || ack.target_id == sender_id ||
            ack.access_epoch == 0 || !sender.has_value() ||
            sender->access_epoch != ack.access_epoch ||
            !room_registry_.authorizes_media_key(
                sender->room_id, sender->room_instance_id,
                ack.access_epoch, media_key_commitment)) {
            return;
        }

        const auto target = client_manager_.get_room_endpoint_by_client_id(
            remote_endpoint_, ack.target_id);
        if (!target.has_value() ||
            !client_manager_.has_authenticated_session(*target)) {
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);
        auto packet_copy = std::make_shared<std::vector<unsigned char>>(
            recv_buf_.data(),
            recv_buf_.data() + sizeof(RoomKeyHandoffAckHdr));
        send(packet_copy->data(), packet_copy->size(), *target, packet_copy);
    }

    bool extract_audio_rate_shape(const unsigned char* packet_data, std::size_t bytes,
                                  uint32_t& sample_rate, uint16_t& frame_count) {
        if (packet_data == nullptr || bytes < sizeof(MsgHdr)) {
            return false;
        }

        session_crypto::SecureAudioMetadata metadata;
        uint16_t encrypted_bytes = 0;
        if (session_crypto::parse_secure_audio_header(
                packet_data, bytes, metadata, encrypted_bytes)) {
            sample_rate = metadata.sample_rate;
            frame_count = metadata.frame_count;
            return true;
        }

        return false;
    }

    bool allow_audio_rate(const unsigned char* packet_data, std::size_t bytes) {
        uint32_t sample_rate = 0;
        uint16_t frame_count = 0;
        const auto now = std::chrono::steady_clock::now();
        if (!extract_audio_rate_shape(packet_data, bytes, sample_rate, frame_count)) {
            rate_limiter_.allow_strict(remote_endpoint_, now);
            return false;
        }

        if (rate_limiter_.allow_authenticated_audio(remote_endpoint_, sample_rate,
                                                    frame_count, now)) {
            return true;
        }

        const uint64_t drop_count = ++rate_limited_audio_drops_total_;
        ++rate_limited_audio_drops_interval_;
        if (drop_count == 1 || drop_count % 100 == 0) {
            spdlog::warn("Rate-limited audio from {}:{} sample_rate={} frame_count={} drops={}",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                      sample_rate, frame_count, drop_count);
        }
        return false;
    }

    void handle_secure_control_message(std::size_t bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (bytes < SECURE_CONTROL_HEADER_BYTES + SECURE_PACKET_TAG_BYTES) {
            rate_limiter_.allow_strict(remote_endpoint_, now);
            return;
        }

        if (!client_manager_.exists(remote_endpoint_)) {
            if (rate_limiter_.allow_unknown(remote_endpoint_, now)) {
                record_unknown_audio_drop(remote_endpoint_);
            }
            return;
        }

        if (!client_manager_.has_authenticated_session(remote_endpoint_)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping secure control from unauthenticated endpoint {}:{}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port());
            }
            return;
        }

        if (!rate_limiter_.allow_authenticated_secure_control(
                remote_endpoint_, now)) {
            return;
        }

        const auto* packet_data = reinterpret_cast<const unsigned char*>(recv_buf_.data());
        session_crypto::SecureControlMetadata metadata;
        uint16_t encrypted_bytes = 0;
        std::string reason;
        if (!session_crypto::parse_secure_control_header(packet_data, bytes, metadata,
                                                         encrypted_bytes, &reason)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping invalid secure control from {}:{} reason={} bytes={}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                          reason, bytes);
            }
            return;
        }

        const uint32_t sender_id = client_manager_.get_client_id(remote_endpoint_);
        if (sender_id == 0 || metadata.sender_id != sender_id) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping secure control with wrong sender id from {}:{} "
                             "metadata_sender={} expected_sender={}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                          metadata.sender_id, sender_id);
            }
            return;
        }

        const auto sender = client_manager_.get_client_info(remote_endpoint_);
        if (!sender.has_value() || sender->access_epoch != metadata.access_epoch ||
            !room_registry_.authorizes_media_key(
                sender->room_id, sender->room_instance_id,
                metadata.access_epoch, metadata.media_key_commitment)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn(
                    "Dropping secure control with unauthorized media key epoch "
                    "sender={} epoch={}",
                    metadata.sender_id, metadata.access_epoch);
            }
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);
        auto packet_copy = std::make_shared<std::vector<unsigned char>>(
            packet_data, packet_data + bytes);
        if (metadata.target_id == 0) {
            forward_secure_control_to_others(remote_endpoint_, packet_copy->data(),
                                             packet_copy->size(), packet_copy);
            return;
        }

        const auto target = client_manager_.get_room_endpoint_by_client_id(
            remote_endpoint_, metadata.target_id);
        if (!target.has_value() ||
            !client_manager_.has_authenticated_session(*target)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping secure control sender={} target={} not in room",
                             metadata.sender_id, metadata.target_id);
            }
            return;
        }
        send(packet_copy->data(), packet_copy->size(), *target, packet_copy);
    }

    void handle_secure_audio_message(std::size_t bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (bytes < SECURE_PACKET_HEADER_BYTES + SECURE_PACKET_TAG_BYTES) {
            rate_limiter_.allow_strict(remote_endpoint_, now);
            return;
        }

        if (!client_manager_.exists(remote_endpoint_)) {
            if (rate_limiter_.allow_unknown(remote_endpoint_, now)) {
                record_unknown_audio_drop(remote_endpoint_);
            }
            return;
        }

        if (!client_manager_.has_authenticated_session(remote_endpoint_)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping secure audio from unauthenticated endpoint {}:{}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port());
            }
            return;
        }

        const auto* packet_data = reinterpret_cast<const unsigned char*>(recv_buf_.data());
        session_crypto::SecureAudioMetadata metadata;
        uint16_t encrypted_bytes = 0;
        std::string reason;
        if (!session_crypto::parse_secure_audio_header(packet_data, bytes, metadata,
                                                       encrypted_bytes, &reason)) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping invalid secure audio from {}:{} reason={} bytes={}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                          reason, bytes);
            }
            return;
        }

        const uint32_t sender_id = client_manager_.get_client_id(remote_endpoint_);
        if (sender_id == 0 || metadata.sender_id != sender_id) {
            if (rate_limiter_.allow_strict(remote_endpoint_, now)) {
                spdlog::warn("Dropping secure audio with wrong sender id from {}:{} "
                             "metadata_sender={} expected_sender={}",
                          remote_endpoint_.address().to_string(), remote_endpoint_.port(),
                          metadata.sender_id, sender_id);
            }
            return;
        }

        if (!allow_audio_rate(packet_data, bytes)) {
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);
        record_audio_ingress(sender_id, remote_endpoint_, packet_data, bytes);
        auto* packet_copy = acquire_fan_out_buffer(packet_data, bytes);
        if (packet_copy != nullptr) {
            forward_audio_to_others(remote_endpoint_, packet_copy);
        }
    }

    void handle_metronome_sync(std::size_t bytes, std::chrono::steady_clock::time_point now) {
        if (bytes < sizeof(MetronomeSyncHdr)) {
            spdlog::debug("Metronome sync packet too small: {} bytes", bytes);
            return;
        }

        if (!client_manager_.exists(remote_endpoint_)) {
            spdlog::warn("Dropping metronome sync from unjoined endpoint {}:{}",
                      remote_endpoint_.address().to_string(), remote_endpoint_.port());
            send_join_required(remote_endpoint_);
            return;
        }

        client_manager_.update_alive(remote_endpoint_, now);

        MetronomeSyncHdr sync{};
        std::memcpy(&sync, recv_buf_.data(), sizeof(MetronomeSyncHdr));
        sync.sequence = ++metronome_sequence_;
        sync.effective_server_time_ns = steady_ns(now) + METRONOME_SCHEDULE_AHEAD_NS;
        std::memcpy(recv_buf_.data(), &sync, sizeof(MetronomeSyncHdr));

        auto packet_copy = std::make_shared<std::vector<unsigned char>>(
            recv_buf_.data(), recv_buf_.data() + sizeof(MetronomeSyncHdr));
        const auto endpoints = client_manager_.get_cached_room_endpoints(remote_endpoint_);
        for (const auto& endpoint: *endpoints) {
            send(packet_copy->data(), packet_copy->size(), endpoint, packet_copy);
        }
    }

    static int64_t steady_ns(std::chrono::steady_clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch())
            .count();
    }

    void record_unknown_audio_drop(const udp::endpoint& endpoint) {
        const auto now = std::chrono::steady_clock::now();
        cleanup_unknown_endpoints(now);

        ++unknown_audio_drops_since_log_;
        ++unknown_audio_drops_interval_;
        ++unknown_audio_drops_total_;

        auto it = unknown_endpoints_.find(endpoint);
        if (it == unknown_endpoints_.end()) {
            if (unknown_endpoints_.size() < server_config::MAX_UNKNOWN_ENDPOINTS) {
                auto [inserted_it, inserted] = unknown_endpoints_.emplace(
                    endpoint, UnknownEndpointInfo{now, now, {}, 0, false});
                it = inserted_it;
                (void)inserted;
            }
        }

        if (it != unknown_endpoints_.end()) {
            it->second.last_seen = now;
            ++it->second.drops;
            constexpr auto JOIN_REQUIRED_INTERVAL = 1s;
            if (it->second.last_join_required_sent.time_since_epoch().count() == 0 ||
                now - it->second.last_join_required_sent >= JOIN_REQUIRED_INTERVAL) {
                send_join_required(endpoint);
                it->second.last_join_required_sent = now;
            }
            if (!it->second.first_log_emitted) {
                spdlog::warn("Dropping audio from unjoined endpoint {}:{}",
                          endpoint.address().to_string(), endpoint.port());
                it->second.first_log_emitted = true;
            }
        }

        if (now - last_unknown_audio_summary_ >= server_config::UNKNOWN_ENDPOINT_LOG_INTERVAL) {
            if (unknown_audio_drops_since_log_ > 0) {
                spdlog::warn("Dropped {} audio packets from unjoined endpoints in the last {} ms (tracking {} endpoints)",
                          unknown_audio_drops_since_log_,
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              server_config::UNKNOWN_ENDPOINT_LOG_INTERVAL)
                              .count(),
                          unknown_endpoints_.size());
                unknown_audio_drops_since_log_ = 0;
            }
            last_unknown_audio_summary_ = now;
        }
    }

    void cleanup_unknown_endpoints(std::chrono::steady_clock::time_point now) {
        for (auto it = unknown_endpoints_.begin(); it != unknown_endpoints_.end();) {
            if (now - it->second.last_seen > server_config::UNKNOWN_ENDPOINT_TTL) {
                rate_limiter_.erase(it->first);
                it = unknown_endpoints_.erase(it);
            } else {
                ++it;
            }
        }
        rate_limiter_.expire(now, server_config::UNKNOWN_ENDPOINT_TTL);
    }

    void record_invalid_audio_drop() {
        ++invalid_audio_drops_since_log_;
        ++invalid_audio_drops_interval_;
        ++invalid_audio_drops_total_;
    }

    void alive_check_timer_callback() {
        auto now = std::chrono::steady_clock::now();
        auto timed_out_ids =
            client_manager_.remove_timed_out_clients(now, server_config::CLIENT_TIMEOUT);

        for (uint32_t timed_out_id: timed_out_ids) {
            spdlog::info("Client timed out (ID: {})", timed_out_id);
            broadcast_participant_leave(timed_out_id);
        }
        cleanup_empty_rooms(now);

        export_metrics_snapshot();
        log_audio_forward_summary();
        reset_metrics_intervals();
    }

    void broadcast_participant_leave(uint32_t participant_id) {
        // Broadcast to all clients that a participant has left
        auto buf = packet_builder::create_participant_leave_packet(participant_id);

        // Get endpoints from manager (safe copy)
        auto endpoints = client_manager_.get_all_endpoints();

        for (const auto& endpoint: endpoints) {
            send(buf->data(), sizeof(CtrlHdr), endpoint, buf);
        }
    }

    void send_join_ack(const udp::endpoint& endpoint, uint32_t participant_id,
                       uint32_t capabilities) {
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(JoinAckHdr));
        JoinAckHdr ack{};
        ack.magic = CTRL_MAGIC;
        ack.type = CtrlHdr::Cmd::JOIN_ACK;
        ack.participant_id = participant_id;
        ack.capabilities = capabilities & AUDIO_SUPPORTED_CAPABILITIES;
        std::memcpy(buf->data(), &ack, sizeof(JoinAckHdr));
        send(buf->data(), buf->size(), endpoint, buf);
    }

    void send_join_required(const udp::endpoint& endpoint) {
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(CtrlHdr));
        CtrlHdr required{};
        required.magic = CTRL_MAGIC;
        required.type = CtrlHdr::Cmd::JOIN_REQUIRED;
        std::memcpy(buf->data(), &required, sizeof(CtrlHdr));
        send(buf->data(), buf->size(), endpoint, buf);
    }

    void send_room_removed(const udp::endpoint& endpoint, uint32_t participant_id) {
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(CtrlHdr));
        CtrlHdr removed{};
        removed.magic = CTRL_MAGIC;
        removed.type = CtrlHdr::Cmd::ROOM_REMOVED;
        removed.participant_id = participant_id;
        std::memcpy(buf->data(), &removed, sizeof(CtrlHdr));
        send(buf->data(), buf->size(), endpoint, buf);
    }

    void send_audio_path_stats(uint32_t sender_id, const AudioIngressStats& stats) {
        if (sender_id == 0 || !client_manager_.exists(stats.endpoint)) {
            return;
        }

        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(AudioPathStatsHdr));
        AudioPathStatsHdr path{};
        path.magic = CTRL_MAGIC;
        path.type = CtrlHdr::Cmd::AUDIO_PATH_STATS;
        path.participant_id = sender_id;
        path.interval_received = static_cast<uint32_t>(
            std::min<uint64_t>(stats.received_interval,
                               std::numeric_limits<uint32_t>::max()));
        path.interval_sequence_gaps = static_cast<uint32_t>(
            std::min<uint64_t>(stats.sequence_gaps_interval,
                               std::numeric_limits<uint32_t>::max()));
        path.interval_unrecovered_sequence_gaps = static_cast<uint32_t>(
            std::min<uint64_t>(
                unrecovered_gap_count(stats.sequence_gaps_interval,
                                      stats.sequence_gap_recoveries_interval),
                std::numeric_limits<uint32_t>::max()));
        path.total_received = static_cast<uint32_t>(
            std::min<uint64_t>(stats.received_total,
                               std::numeric_limits<uint32_t>::max()));
        path.total_sequence_gaps = static_cast<uint32_t>(
            std::min<uint64_t>(stats.sequence_gaps_total,
                               std::numeric_limits<uint32_t>::max()));
        path.total_unrecovered_sequence_gaps = static_cast<uint32_t>(
            std::min<uint64_t>(
                unrecovered_gap_count(stats.sequence_gaps_total,
                                      stats.sequence_gap_recoveries_total),
                std::numeric_limits<uint32_t>::max()));
        path.observed_frame_count = stats.last_frame_count;
        std::memcpy(buf->data(), &path, sizeof(AudioPathStatsHdr));
        send(buf->data(), buf->size(), stats.endpoint, buf);
    }

    void broadcast_participant_info(const udp::endpoint& joined_endpoint, uint32_t participant_id,
                                    const std::string& profile_id,
                                    const std::string& display_name) {
        const uint32_t capabilities =
            client_manager_.get_client_capabilities(joined_endpoint);
        Bytes<E2E_PUBLIC_KEY_BYTES> key_public{};
        client_manager_.with_client(joined_endpoint, [&](const ClientInfo& info) {
            key_public = info.key_public;
        });
        auto buf = packet_builder::create_participant_info_packet(
            participant_id, profile_id, display_name, capabilities, key_public);
        const auto endpoints = client_manager_.get_cached_room_endpoints(joined_endpoint);

        for (const auto& endpoint: *endpoints) {
            send(buf->data(), buf->size(), endpoint, buf);
        }
    }

    void send_existing_participant_info_to(const udp::endpoint& joined_endpoint) {
        auto existing_clients = client_manager_.get_room_clients_except(joined_endpoint);
        for (const auto& [endpoint, info]: existing_clients) {
            if (info.profile_id.empty() && info.display_name.empty()) {
                continue;
            }
            auto buf = packet_builder::create_participant_info_packet(
                info.client_id, info.profile_id, info.display_name,
                info.capabilities, info.key_public);
            send(buf->data(), buf->size(), joined_endpoint, buf);
        }
    }

    void forward_audio_to_others(const udp::endpoint& sender, FanOutBuffer* buffer) {
        const auto endpoints = client_manager_.get_cached_room_endpoints(sender);
        buffer->pending_workers.store(static_cast<uint32_t>(MEDIA_WORKER_COUNT),
                                      std::memory_order_release);
        for (auto& worker: media_workers_) {
            if (worker.queue.try_push(MediaTask{buffer, endpoints, sender})) {
                worker.ready.release();
                continue;
            }
            ++sfu_pool_drops_interval_;
            ++sfu_pool_drops_total_;
            complete_media_task(buffer);
        }
    }

    void forward_secure_control_to_others(
        const udp::endpoint& sender, void* packet_data, std::size_t packet_size,
        const std::shared_ptr<std::vector<unsigned char>>& keep_alive = nullptr) {
        const auto endpoints = client_manager_.get_cached_room_endpoints(sender);
        for (const auto& endpoint: *endpoints) {
            if (endpoint == sender) {
                continue;
            }
            if (client_manager_.has_authenticated_session(endpoint)) {
                send(packet_data, packet_size, endpoint, keep_alive);
            }
        }
    }

    void record_audio_ingress(uint32_t sender_id, const udp::endpoint& endpoint,
                              const void* packet_data, std::size_t packet_size,
                              bool count_received = true) {
        if (sender_id == 0 || packet_size < sizeof(MsgHdr)) {
            return;
        }

        session_crypto::SecureAudioMetadata metadata;
        uint16_t encrypted_bytes = 0;
        if (!session_crypto::parse_secure_audio_header(
                static_cast<const unsigned char*>(packet_data), packet_size, metadata,
                encrypted_bytes) ||
            metadata.sender_id != sender_id) {
            return;
        }

        auto& stats = audio_ingress_stats_[sender_id];
        stats.endpoint = endpoint;
        stats.last_frame_count = metadata.frame_count;
        if (count_received) {
            ++stats.received_total;
            ++stats.received_interval;
        }
        const auto sequence_delta = stats.sequence_tracker.record(metadata.sequence);
        if (sequence_delta.gaps_detected > 0) {
            stats.sequence_gaps_total += sequence_delta.gaps_detected;
            stats.sequence_gaps_interval += sequence_delta.gaps_detected;
        }
        if (sequence_delta.gaps_recovered > 0) {
            stats.sequence_gap_recoveries_total += sequence_delta.gaps_recovered;
            stats.sequence_gap_recoveries_interval += sequence_delta.gaps_recovered;
        }
        stats.sequence_unresolved_gaps = stats.sequence_tracker.unresolved_gaps();
        if (sequence_delta.late_or_duplicate &&
            (count_received || sequence_delta.gaps_recovered > 0)) {
            ++stats.sequence_late_or_reordered_total;
            ++stats.sequence_late_or_reordered_interval;
        }
    }

    void record_ping_received(uint32_t client_id, const udp::endpoint& endpoint,
                              uint32_t sequence) {
        if (client_id == 0) {
            return;
        }

        auto& stats = ping_stats_[client_id];
        stats.endpoint = endpoint;
        ++stats.received_total;
        ++stats.received_interval;
        const auto sequence_delta = stats.sequence_tracker.record(sequence);
        if (sequence_delta.gaps_detected > 0) {
            stats.sequence_gaps_total += sequence_delta.gaps_detected;
            stats.sequence_gaps_interval += sequence_delta.gaps_detected;
        }
        if (sequence_delta.gaps_recovered > 0) {
            stats.sequence_gap_recoveries_total += sequence_delta.gaps_recovered;
            stats.sequence_gap_recoveries_interval += sequence_delta.gaps_recovered;
        }
        stats.sequence_unresolved_gaps = stats.sequence_tracker.unresolved_gaps();
        if (sequence_delta.late_or_duplicate) {
            ++stats.sequence_late_or_reordered_total;
            ++stats.sequence_late_or_reordered_interval;
        }
    }

    void record_ping_reply_queued(uint32_t client_id) {
        if (client_id == 0) {
            return;
        }

        auto& stats = ping_stats_[client_id];
        ++stats.reply_queued_total;
        ++stats.reply_queued_interval;
    }

    static server_metrics::TrafficCounters audio_counters_from_ingress(
        const AudioIngressStats& stats) {
        server_metrics::TrafficCounters counters;
        counters.interval = stats.received_interval;
        counters.total = stats.received_total;
        counters.sequence_gaps_interval = stats.sequence_gaps_interval;
        counters.sequence_gaps_total = stats.sequence_gaps_total;
        counters.sequence_gap_recoveries_interval =
            stats.sequence_gap_recoveries_interval;
        counters.sequence_gap_recoveries_total = stats.sequence_gap_recoveries_total;
        counters.sequence_unresolved_gaps = stats.sequence_unresolved_gaps;
        counters.sequence_late_or_reordered_interval =
            stats.sequence_late_or_reordered_interval;
        counters.sequence_late_or_reordered_total =
            stats.sequence_late_or_reordered_total;
        return counters;
    }

    static server_metrics::TrafficCounters ping_counters_from_stats(
        const PingStats& stats) {
        server_metrics::TrafficCounters counters;
        counters.interval = stats.received_interval;
        counters.total = stats.received_total;
        counters.sequence_gaps_interval = stats.sequence_gaps_interval;
        counters.sequence_gaps_total = stats.sequence_gaps_total;
        counters.sequence_gap_recoveries_interval =
            stats.sequence_gap_recoveries_interval;
        counters.sequence_gap_recoveries_total = stats.sequence_gap_recoveries_total;
        counters.sequence_unresolved_gaps = stats.sequence_unresolved_gaps;
        counters.sequence_late_or_reordered_interval =
            stats.sequence_late_or_reordered_interval;
        counters.sequence_late_or_reordered_total =
            stats.sequence_late_or_reordered_total;
        return counters;
    }

    static std::string metric_endpoint(const udp::endpoint& endpoint) {
        const auto address = udp_network::format_address_for_display(endpoint.address());
        if (endpoint.address().is_v6() && !endpoint.address().to_v6().is_v4_mapped()) {
            return "[" + address + "]:" + std::to_string(endpoint.port());
        }
        return address + ":" + std::to_string(endpoint.port());
    }

    server_metrics::Snapshot build_metrics_snapshot() {
        const auto now = std::chrono::steady_clock::now();
        server_metrics::Snapshot snapshot;
        snapshot.server_id = options_.server_id;
        snapshot.timestamp_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        snapshot.uptime_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at_)
                .count();
        snapshot.connected_clients = client_manager_.count();
        snapshot.unknown_endpoint_count = unknown_endpoints_.size();
        snapshot.token_nonce_count = used_token_nonces_.size();
        snapshot.metrics_export_drops_total = metrics_exporter_.dropped_snapshots();
        snapshot.metrics_export_failures_total = metrics_exporter_.failed_writes();
        snapshot.drops.unknown_audio_interval = unknown_audio_drops_interval_;
        snapshot.drops.unknown_audio_total = unknown_audio_drops_total_;
        snapshot.drops.invalid_audio_interval = invalid_audio_drops_interval_;
        snapshot.drops.invalid_audio_total = invalid_audio_drops_total_;
        snapshot.drops.rate_limited_audio_interval = rate_limited_audio_drops_interval_;
        snapshot.drops.rate_limited_audio_total = rate_limited_audio_drops_total_;
        snapshot.drops.sfu_send_cap_interval =
            sfu_send_cap_drops_interval_.load(std::memory_order_relaxed);
        snapshot.drops.sfu_send_cap_total =
            sfu_send_cap_drops_total_.load(std::memory_order_relaxed);
        snapshot.drops.sfu_pool_interval =
            sfu_pool_drops_interval_.load(std::memory_order_relaxed);
        snapshot.drops.sfu_pool_total =
            sfu_pool_drops_total_.load(std::memory_order_relaxed);
        snapshot.drops.expired_media_interval =
            sfu_expired_media_drops_interval_.load(std::memory_order_relaxed);
        snapshot.drops.expired_media_total =
            sfu_expired_media_drops_total_.load(std::memory_order_relaxed);
        snapshot.receive_handler_us = receive_handler_latency_.snapshot();
        snapshot.relay_dwell_us = relay_dwell_latency_.snapshot_and_reset();

        snapshot.ingress.reserve(audio_ingress_stats_.size());
        for (const auto& [sender_id, stats]: audio_ingress_stats_) {
            snapshot.ingress.push_back(server_metrics::IngressMetric{
                sender_id,
                metric_endpoint(stats.endpoint),
                audio_counters_from_ingress(stats),
                stats.last_frame_count,
            });
        }

        snapshot.pings.reserve(ping_stats_.size());
        for (const auto& [client_id, stats]: ping_stats_) {
            snapshot.pings.push_back(server_metrics::PingMetric{
                client_id,
                metric_endpoint(stats.endpoint),
                stats.reply_queued_interval,
                stats.reply_queued_total,
                ping_counters_from_stats(stats),
            });
        }

        return snapshot;
    }

    void reset_metrics_intervals() {
        unknown_audio_drops_interval_ = 0;
        invalid_audio_drops_interval_ = 0;
        rate_limited_audio_drops_interval_ = 0;
        sfu_send_cap_drops_interval_.store(0, std::memory_order_relaxed);
        sfu_pool_drops_interval_.store(0, std::memory_order_relaxed);
        sfu_expired_media_drops_interval_.store(0, std::memory_order_relaxed);
        receive_handler_latency_.reset();
    }

    void log_audio_forward_summary() {
        const auto pool_drops = sfu_pool_drops_interval_.load(std::memory_order_relaxed);
        const auto expired_drops =
            sfu_expired_media_drops_interval_.load(std::memory_order_relaxed);
        const auto send_drops =
            sfu_send_cap_drops_interval_.load(std::memory_order_relaxed);
        if (pool_drops != 0 || expired_drops != 0 || send_drops != 0) {
            spdlog::warn(
                "Media admission interval: pool_drops={} expired_target_drops={} send_drops={}",
                pool_drops, expired_drops, send_drops);
        }
        if (invalid_audio_drops_since_log_ > 0) {
            spdlog::warn("Dropped {} invalid/incomplete audio packets in the last interval",
                      invalid_audio_drops_since_log_);
            invalid_audio_drops_since_log_ = 0;
        }

        for (auto& [sender_id, stats]: audio_ingress_stats_) {
            if (stats.received_interval == 0 && stats.sequence_gaps_interval == 0 &&
                stats.sequence_late_or_reordered_interval == 0) {
                continue;
            }

            spdlog::info(
                "Ingress diag interval sender={} endpoint={}:{} received={} seq_gap={} "
                "net_gap={} gap_rate={:.1f}% seq_recovered={} seq_unresolved={} "
                "seq_late={} late={:.1f}% total received={} seq_gap={} net_gap={} "
                "seq_recovered={} seq_unresolved={} seq_late={}",
                sender_id, stats.endpoint.address().to_string(), stats.endpoint.port(),
                stats.received_interval, stats.sequence_gaps_interval,
                unrecovered_gap_count(stats.sequence_gaps_interval,
                                      stats.sequence_gap_recoveries_interval),
                percent_missing(unrecovered_gap_count(
                                    stats.sequence_gaps_interval,
                                    stats.sequence_gap_recoveries_interval),
                                stats.received_interval),
                stats.sequence_gap_recoveries_interval, stats.sequence_unresolved_gaps,
                stats.sequence_late_or_reordered_interval,
                percent_of_packets(stats.sequence_late_or_reordered_interval,
                                   stats.received_interval),
                stats.received_total, stats.sequence_gaps_total,
                unrecovered_gap_count(stats.sequence_gaps_total,
                                      stats.sequence_gap_recoveries_total),
                stats.sequence_gap_recoveries_total, stats.sequence_unresolved_gaps,
                stats.sequence_late_or_reordered_total);
            send_audio_path_stats(sender_id, stats);
            stats.received_interval = 0;
            stats.sequence_gaps_interval = 0;
            stats.sequence_gap_recoveries_interval = 0;
            stats.sequence_late_or_reordered_interval = 0;
        }

        for (auto& [client_id, stats]: ping_stats_) {
            if (stats.received_interval == 0 && stats.reply_queued_interval == 0 &&
                stats.sequence_gaps_interval == 0 &&
                stats.sequence_late_or_reordered_interval == 0) {
                continue;
            }

            spdlog::info(
                "Ping diag interval client={} endpoint={}:{} received={} reply_queued={} "
                "seq_gap={} gap_rate={:.1f}% seq_recovered={} seq_unresolved={} seq_late={} "
                "late={:.1f}% total received={} reply_queued={} seq_gap={} seq_recovered={} "
                "seq_unresolved={} seq_late={}",
                client_id, stats.endpoint.address().to_string(), stats.endpoint.port(),
                stats.received_interval, stats.reply_queued_interval,
                stats.sequence_gaps_interval,
                percent_missing(stats.sequence_gaps_interval, stats.received_interval),
                stats.sequence_gap_recoveries_interval, stats.sequence_unresolved_gaps,
                stats.sequence_late_or_reordered_interval,
                percent_of_packets(stats.sequence_late_or_reordered_interval,
                                   stats.received_interval),
                stats.received_total, stats.reply_queued_total, stats.sequence_gaps_total,
                stats.sequence_gap_recoveries_total, stats.sequence_unresolved_gaps,
                stats.sequence_late_or_reordered_total);
            stats.received_interval = 0;
            stats.reply_queued_interval = 0;
            stats.sequence_gaps_interval = 0;
            stats.sequence_gap_recoveries_interval = 0;
            stats.sequence_late_or_reordered_interval = 0;
        }
    }

    static double percent_missing(uint64_t missing_events, uint64_t received_packets) {
        const uint64_t denominator = missing_events + received_packets;
        if (denominator == 0) {
            return 0.0;
        }
        return (static_cast<double>(missing_events) * 100.0) /
               static_cast<double>(denominator);
    }

    static uint64_t unrecovered_gap_count(uint64_t gaps, uint64_t recoveries) {
        return gaps > recoveries ? gaps - recoveries : 0;
    }

    static double percent_of_packets(uint64_t events, uint64_t packets) {
        if (packets == 0) {
            return 0.0;
        }
        return (static_cast<double>(events) * 100.0) / static_cast<double>(packets);
    }

    ServerOptions options_;
    udp::socket   socket_;
    server_metrics::AsyncJsonlExporter metrics_exporter_;
    std::chrono::steady_clock::time_point started_at_;

    ClientManager client_manager_;
    room_registry::RoomRegistry room_registry_;
    std::unordered_map<udp::endpoint, UnknownEndpointInfo, endpoint_hash> unknown_endpoints_;
    std::unordered_map<std::string, UsedTokenNonce> used_token_nonces_;
    std::unordered_map<uint32_t, AudioIngressStats> audio_ingress_stats_;
    std::unordered_map<uint32_t, PingStats> ping_stats_;
    std::array<FanOutBuffer, MAX_FAN_OUT_BUFFERS> fan_out_buffers_{};
    std::array<MediaWorker, MEDIA_WORKER_COUNT> media_workers_{};
    server_rate_limiter::ProtocolRateLimiter rate_limiter_;
    udp_network::UdpSocketQos socket_qos_;
    LatencyHistogramState receive_handler_latency_;
    ConcurrentLatencyHistogramState relay_dwell_latency_;
    uint64_t unknown_audio_drops_since_log_ = 0;
    uint64_t unknown_audio_drops_interval_ = 0;
    uint64_t unknown_audio_drops_total_ = 0;
    uint64_t invalid_audio_drops_since_log_ = 0;
    uint64_t invalid_audio_drops_interval_ = 0;
    uint64_t invalid_audio_drops_total_ = 0;
    uint64_t rate_limited_audio_drops_interval_ = 0;
    uint64_t rate_limited_audio_drops_total_ = 0;
    std::atomic<uint64_t> sfu_send_cap_drops_interval_{0};
    std::atomic<uint64_t> sfu_send_cap_drops_total_{0};
    std::atomic<uint64_t> sfu_pool_drops_interval_{0};
    std::atomic<uint64_t> sfu_pool_drops_total_{0};
    std::atomic<uint64_t> sfu_expired_media_drops_interval_{0};
    std::atomic<uint64_t> sfu_expired_media_drops_total_{0};
    std::atomic<bool> shutting_down_{false};
    uint32_t metronome_sequence_ = 0;
    std::chrono::steady_clock::time_point last_unknown_audio_summary_ =
        std::chrono::steady_clock::now();

    std::array<char, server_config::RECV_BUF_SIZE> recv_buf_;
    udp::endpoint                                  remote_endpoint_;
    std::array<ReceiveSlot, server_config::RECEIVE_SLOT_COUNT> receive_slots_;

    PeriodicTimer alive_check_timer_;
};

int main(int argc, char** argv) {
    try {
        asio::io_context io_context;
        auto             options = parse_server_options(argc, argv);

        const auto log_level = options.log_file_path.empty()
                                   ? logging::default_level()
                                   : spdlog::level::info;
        logging::init(true, false, !options.log_file_path.empty(), options.log_file_path,
                      log_level, options.log_max_bytes, options.log_max_files);
        spdlog::warn("Sesivo server version {}", SESIVO_VERSION);
        log_server_addresses(io_context, options.port);

        if (options.crash_reports_enabled) {
            crash_reporter::Options crash_options;
            crash_options.report_dir = options.crash_report_dir;
            crash_options.process_name = "server";
            crash_options.platform = runtime_platform_name();
            crash_options.arch = runtime_arch_name();
            crash_reporter::install(crash_options);
            spdlog::info("Crash reports enabled: {}", options.crash_report_dir);
        }

        spdlog::info("Starting SFU server on [::]:{} (dual-stack preferred)", options.port);
        spdlog::info("Runtime: process=server platform={} arch={}", runtime_platform_name(),
                  runtime_arch_name());
        if (!options.log_file_path.empty()) {
            spdlog::info("Logging to {}", options.log_file_path);
            spdlog::info("Log rotation: max_bytes={} max_files={}", options.log_max_bytes,
                      options.log_max_files);
        }
        if (!options.metrics_jsonl_path.empty()) {
            spdlog::info("Server metrics JSONL export: {}", options.metrics_jsonl_path);
        }
        spdlog::info("Forwarding audio packets between clients");
        if (options.join_secret_ephemeral) {
            spdlog::info(
                "Generated an ephemeral 256-bit join signing key; outstanding "
                "tickets become invalid when this server restarts");
        } else {
            spdlog::info(
                "Persistent join signing key configured; protect it and coordinate "
                "rotation across ticket issuers and server instances");
        }

        Server server(io_context, options);

        asio::signal_set shutdown_signals(io_context, SIGINT, SIGTERM);
        shutdown_signals.async_wait([&](const std::error_code& error, int signal_number) {
            if (error) {
                return;
            }
            spdlog::info("Shutdown signal {} received", signal_number);
            server.begin_shutdown();
            io_context.stop();
        });

        io_context.run();
        logging::flush();
    } catch (std::exception& e) {
        spdlog::error("ERR: {}", e.what());
        logging::flush();
        return 1;
    }
}
