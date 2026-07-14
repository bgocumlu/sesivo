#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // Prevent Windows from defining min/max macros
#endif
#include <windows.h>
#include <avrt.h>
#include <winsock2.h>
#endif

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>
#include <concurrentqueue.h>
#include <opus.h>
#include <spdlog/spdlog.h>

#include "audio_analysis.h"
#include "audio_backend_policy.h"
#include "audio_callback_policy.h"
#include "audio_packet.h"
#include "audio_stream.h"
#include "client_audio_devices.h"
#include "client_audio_state.h"
#include "client_join_session.h"
#include "client_media_state.h"
#include "client_metronome.h"
#include "client_network_path.h"
#include "clock_offset_estimator.h"
#include "congestion_policy.h"
#include "client_runtime.h"
#include "delivery_stall_policy.h"
#include "delivery_report_feedback.h"
#include "jitter_policy.h"
#include "message_validator.h"
#include "opus_decoder.h"
#include "opus_defines.h"
#include "opus_encoder.h"
#include "opus_network_clock.h"
#include "opus_tail_fade.h"
#include "packet_builder.h"
#include "participant_info.h"
#include "participant_manager.h"
#include "performer_join_token.h"
#include "periodic_timer.h"
#include "post_drop_rate_recovery.h"
#include "protocol.h"
#include "replay_window.h"
#include "session_crypto.h"
#include "udp_port.h"
#include "udp_socket_config.h"

using asio::ip::udp;
using namespace std::chrono_literals;

constexpr int OPUS_AUTO_JITTER_CONTROL_WINDOW_CALLBACKS = 200;
constexpr int OPUS_AUTO_JITTER_EVENTS_BEFORE_INCREASE = 3;
constexpr size_t CLIENT_CHAT_RETAINED_MESSAGES = 100;
constexpr bool AUDIO_CALLBACK_NOTIFY_ENABLED = false;

class Client {
public:
    Client(asio::io_context& io_context,
           PerformerJoinOptions performer_join_options = {},
           std::filesystem::path audio_preferences_path = {},
           AudioDevicePreferences audio_preferences = {})
        : io_context_(io_context),
          socket_(io_context),
          join_session_(std::move(performer_join_options), 1s),
          audio_state_(audio_preferences.audio_api),
          audio_preferences_path_(std::move(audio_preferences_path)),
          ping_timer_(io_context, 250ms, [this]() { ping_timer_callback(); }),
          join_retry_timer_(io_context, 1s, [this]() { join_retry_timer_callback(); }),
          alive_timer_(io_context, 1s, [this]() { alive_timer_callback(); }),
          cleanup_timer_(io_context, 1s, [this]() { cleanup_timer_callback(); }),
          chat_timer_(io_context, 500ms, [this]() { chat_timer_callback(); }) {
        initialize_e2e_keypair();
        std::error_code socket_error;
        const auto protocol =
            udp_network::open_dual_stack_socket(socket_, 0, socket_error);
        if (socket_error) {
            throw std::runtime_error("Failed to bind UDP socket: " +
                                     socket_error.message());
        }
        spdlog::info("Client local port: {} ({})", socket_.local_endpoint().port(),
                  protocol == udp::v6() ? "IPv6 dual-stack" : "IPv4 fallback");

        // Optimize UDP socket buffers for low-latency audio streaming
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            configure_udp_socket_locked();
        }

        // Server join is resolved by the GUI startup job so the window can
        // show connection progress instead of blocking launch in this constructor.
    }

    ~Client() {
        lifetime_token_.reset();
        audio_stream_requested_.store(false, std::memory_order_release);
        audio_device_generation_.fetch_add(1, std::memory_order_acq_rel);
        audio_device_worker_.stop();
        audio_device_worker_.join();
        std::lock_guard<std::mutex> lock(audio_lifecycle_mutex_);
        audio_.stop_audio_stream();
        stop_audio_sender_thread();
    }

    // Start connection to server (or switch to new server)
    void start_connection(const std::string& server_address, uint16_t server_port) {
        if (io_context_.get_executor().running_in_this_thread()) {
            start_connection_on_io_context(server_address, server_port);
            return;
        }

        auto started = std::make_shared<std::promise<void>>();
        auto future = started->get_future();
        asio::post(io_context_, [this, server_address, server_port, started]() {
            try {
                start_connection_on_io_context(server_address, server_port);
                started->set_value();
            } catch (...) {
                started->set_exception(std::current_exception());
            }
        });
        future.get();
    }

    void join_room(const std::string& server_address, uint16_t server_port,
                   const std::string& room_id, const std::string& profile_id,
                   const std::string& display_name, const std::string& join_token,
                   const std::string& media_secret = {},
                   uint8_t access_mode = ROOM_ACCESS_OPEN) {
        PerformerJoinOptions options;
        options.room_id = room_id;
        options.room_handle = room_id;
        options.user_id = profile_id;
        options.display_name = display_name;
        options.join_token = join_token;
        options.media_secret = media_secret;
        options.access_mode = access_mode;
        options.key_public = e2e_key_public_bytes_;

        auto started = std::make_shared<std::promise<void>>();
        auto future = started->get_future();
        asio::post(io_context_, [this, server_address, server_port,
                                 options = std::move(options), started]() mutable {
            try {
                join_session_.configure(std::move(options));
                participant_manager_.clear();
                delivery_report_accumulators_.clear();
                downstream_delivery_by_reporter_.clear();
                downstream_delivery_last_activity_.clear();
                downstream_delivery_tombstone_since_.clear();
                downstream_feedback_by_reporter_.clear();
                downstream_feedback_pending_ = false;
                delivery_report_generated_baseline_ =
                    audio_packets_generated_.load(std::memory_order_acquire);
                next_delivery_report_epoch_ = 1;
                reset_packet_duration_adaptation();
                clear_access_request_state();
                clear_chat_state();
                start_connection_on_io_context(server_address, server_port);
                started->set_value();
            } catch (...) {
                started->set_exception(std::current_exception());
            }
        });
        future.get();
    }

    void start_connection_on_io_context(const std::string& server_address,
                                        uint16_t server_port) {
        spdlog::info("Connecting to {}:{}...", server_address, server_port);
        join_denied_room_full_.store(false, std::memory_order_release);
        receiving_enabled_.store(false, std::memory_order_release);
        outbound_enabled_.store(false, std::memory_order_release);
        outbound_generation_.fetch_add(1, std::memory_order_acq_rel);
        std::error_code cancel_error;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            socket_.cancel(cancel_error);
        }

        // Resolve hostname or IP address
        udp::resolver               resolver(io_context_);
        udp::resolver::results_type endpoints =
            resolver.resolve(server_address, std::to_string(server_port));
        const auto selected_endpoint = choose_client_endpoint(endpoints);
        if (!selected_endpoint.has_value()) {
            throw std::runtime_error("No compatible UDP endpoint resolved for " +
                                     server_address);
        }
        const udp::endpoint resolved_endpoint = *selected_endpoint;

        spdlog::info("Resolved to: {}:{}",
                  udp_network::format_address_for_display(resolved_endpoint.address()),
                  resolved_endpoint.port());
        {
            std::lock_guard<std::mutex> lock(server_endpoint_mutex_);
            server_endpoint_ = resolved_endpoint;
            outbound_generation_.fetch_add(1, std::memory_order_acq_rel);
        }
        udp_network::QosResult qos;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            reopen_socket_for_endpoint_locked(resolved_endpoint);
            qos = socket_qos_.ensure_flow(socket_, resolved_endpoint);
        }
        log_udp_qos_result(resolved_endpoint, qos);
        join_session_.reset();
        reset_session_security();
        reset_server_clock_and_ping_state();
        receive_generation_.fetch_add(1, std::memory_order_acq_rel);
        receiving_enabled_.store(true, std::memory_order_release);
        outbound_enabled_.store(true, std::memory_order_release);

        start_receive_slots();

        spdlog::info("Connected and receiving!");

        send_join();
    }

    void send_join() {
        const JoinHdr join = join_session_.make_join_header();
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(JoinHdr));
        std::memcpy(buf->data(), &join, sizeof(JoinHdr));
        update_media_key_from_secret();
        join_session_.mark_join_sent(std::chrono::steady_clock::now());
        send(buf->data(), buf->size(), buf);
        spdlog::info("Sent JOIN for room '{}' user '{}' token {} room_key {}",
                  join_session_.room_id(),
                  join_session_.user_id(),
                  join_session_.has_join_token() ? "present" : "missing",
                  join_session_.has_media_secret() ? "present" : "missing");
    }

    void reset_session_security() {
        std::shared_ptr<const session_crypto::SessionKey> cleared;
        std::atomic_store_explicit(&session_key_, std::move(cleared),
                                   std::memory_order_release);
        secure_control_replay_windows_.clear();
        secure_control_sequence_.store(1, std::memory_order_release);
    }

    void update_media_key_from_secret() {
        if (!join_session_.has_media_secret()) {
            reset_session_security();
            return;
        }
        if (!join_session_.has_join_token()) {
            reset_session_security();
            spdlog::warn("Media key requires a signed join token");
            return;
        }

        std::string reason;
        const auto token =
            performer_join_token::parse_unverified(join_session_.join_token(), reason);
        if (!token.has_value() || token->claims.room_id != join_session_.room_id()) {
            reset_session_security();
            spdlog::warn("Join token is not usable for E2E media key derivation: {}",
                         reason.empty() ? "room claims unavailable" : reason);
            return;
        }
        if (!session_crypto::media_secret_matches_commitment(
                join_session_.media_secret(),
                join_session_.media_key_commitment())) {
            reset_session_security();
            spdlog::warn("Media secret does not match the server-authorized room key");
            return;
        }

        const auto derived = session_crypto::derive_media_key_from_secret(
            token->claims.room_id, token->claims.room_instance_id,
            join_session_.media_secret());
        if (!derived.has_value()) {
            reset_session_security();
            spdlog::warn("Could not derive E2E media key: crypto unavailable");
            return;
        }

        std::shared_ptr<const session_crypto::SessionKey> published =
            std::make_shared<const session_crypto::SessionKey>(*derived);
        std::atomic_store_explicit(&session_key_, std::move(published),
                                   std::memory_order_release);
    }

    bool rotate_media_key(const std::string& media_secret, uint32_t access_epoch) {
        auto completed = std::make_shared<std::promise<bool>>();
        auto future = completed->get_future();
        asio::post(io_context_, [this, media_secret, access_epoch, completed]() {
            completed->set_value(rotate_media_key_on_io_context(media_secret, access_epoch));
        });
        return future.get();
    }

    bool has_media_key() const {
        return current_session_key() != nullptr && join_session_.has_media_secret();
    }

    std::string current_media_secret() const {
        return join_session_.media_secret();
    }

    void set_room_access_mode(uint8_t access_mode) {
        asio::post(io_context_, [this, access_mode]() {
            join_session_.set_access_mode(access_mode);
            if (access_mode != ROOM_ACCESS_APPROVE) {
                std::vector<uint32_t> pending_ids;
                {
                    std::lock_guard<std::mutex> lock(access_request_mutex_);
                    pending_ids.reserve(pending_access_requests_.size());
                    for (const auto& [participant_id, _]: pending_access_requests_) {
                        pending_ids.push_back(participant_id);
                    }
                }
                for (uint32_t participant_id: pending_ids) {
                    if (queue_media_key_handoff_on_io(participant_id)) {
                        remove_pending_access_request(participant_id);
                    }
                }
            }
        });
    }

    bool approve_waiting_participant(uint32_t participant_id) {
        auto completed = std::make_shared<std::promise<bool>>();
        auto future = completed->get_future();
        asio::post(io_context_, [this, participant_id, completed]() {
            const bool ok = queue_media_key_handoff_on_io(participant_id);
            if (ok) {
                remove_pending_access_request(participant_id);
            }
            completed->set_value(ok);
        });
        return future.get();
    }

    std::vector<ParticipantInfo> get_waiting_participant_info() const {
        std::lock_guard<std::mutex> lock(access_request_mutex_);
        std::vector<ParticipantInfo> result;
        result.reserve(pending_access_requests_.size());
        for (const auto& [_, info]: pending_access_requests_) {
            result.push_back(info);
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.display_name < right.display_name;
        });
        return result;
    }

    void clear_room_session_state() {
        join_session_.configure({});
        participant_manager_.clear();
        delivery_report_accumulators_.clear();
        downstream_delivery_by_reporter_.clear();
        downstream_delivery_last_activity_.clear();
        downstream_delivery_tombstone_since_.clear();
        downstream_feedback_by_reporter_.clear();
        downstream_feedback_pending_ = false;
        delivery_report_generated_baseline_ =
            audio_packets_generated_.load(std::memory_order_acquire);
        next_delivery_report_epoch_ = 1;
        reset_packet_duration_adaptation();
        clear_access_request_state();
        clear_chat_state();
        reset_session_security();
        reset_server_clock_and_ping_state();
        audio_path_interval_received_.store(0, std::memory_order_relaxed);
        audio_path_interval_sequence_gaps_.store(0, std::memory_order_relaxed);
        audio_path_interval_gaps_.store(0, std::memory_order_relaxed);
        request_recent_opus_audio_packets_reset();
        OpusSendFrame discarded_opus;
        while (opus_send_queue_.try_dequeue(discarded_opus)) {
        }
        opus_tx_accumulator_reset_requested_.store(true, std::memory_order_release);
    }

    void clear_server_endpoint() {
        std::lock_guard<std::mutex> lock(server_endpoint_mutex_);
        server_endpoint_ = {};
        outbound_generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    void disconnect_from_server(bool notify_server) {
        const auto target = current_server_endpoint();
        spdlog::info(target.port() == 0 ? "Clearing disconnected room state..."
                                        : "Disconnecting from server...");

        receiving_enabled_.store(false, std::memory_order_release);
        receive_generation_.fetch_add(1, std::memory_order_acq_rel);
        outbound_enabled_.store(false, std::memory_order_release);
        outbound_generation_.fetch_add(1, std::memory_order_acq_rel);

        std::error_code leave_error;
        std::error_code cancel_error;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (notify_server && target.port() != 0) {
                CtrlHdr chdr{};
                chdr.magic = CTRL_MAGIC;
                chdr.type  = CtrlHdr::Cmd::LEAVE;
                socket_.send_to(asio::buffer(&chdr, sizeof(CtrlHdr)), target, 0,
                                leave_error);
            }
            socket_.cancel(cancel_error);
            socket_qos_.reset();
        }
        if (leave_error) {
            spdlog::warn("LEAVE send failed: {}", leave_error.message());
        }
        if (cancel_error) {
            spdlog::warn("socket cancel failed: {}", cancel_error.message());
        }

        clear_server_endpoint();
        clear_room_session_state();

        spdlog::info("Disconnected (room session cleared)");
    }

    // Stop connection (stops sending/receiving UDP packets)
    void stop_connection() {
        disconnect_from_server(true);
    }

    bool consume_room_removed_by_server() {
        return room_removed_by_server_.exchange(false, std::memory_order_acq_rel);
    }

    bool consume_join_denied_room_full() {
        return join_denied_room_full_.exchange(false, std::memory_order_acq_rel);
    }

    bool start_audio_stream(AudioStream::DeviceIndex input_device,
                            AudioStream::DeviceIndex output_device,
                            const AudioStream::AudioConfig& config = AudioStream::AudioConfig{}) {
        std::lock_guard<std::mutex> lock(audio_lifecycle_mutex_);
        audio_device_generation_.fetch_add(1, std::memory_order_acq_rel);
        audio_stream_requested_.store(false, std::memory_order_release);
        const bool success = start_audio_stream_locked(input_device, output_device, config);
        if (success) {
            audio_stream_requested_.store(true, std::memory_order_release);
            audio_recovery_attempts_.store(0, std::memory_order_release);
            next_audio_recovery_ns_.store(0, std::memory_order_release);
        }
        return success;
    }

    bool start_audio_stream_locked(AudioStream::DeviceIndex input_device,
                                   AudioStream::DeviceIndex output_device,
                                   const AudioStream::AudioConfig& config) {
        stop_audio_sender_thread();
        AudioStream::AudioConfig runtime_config = config;

        // Get input channel count from device info before creating encoder
        // (audio_.get_input_channel_count() returns 0 before stream starts)
        const auto* input_info_ptr = AudioStream::get_device_info(input_device);
        if (input_info_ptr == nullptr) {
            spdlog::error("Invalid input device");
            return false;
        }
        auto input_info = *input_info_ptr;
        const int input_device_channels = std::max(input_info.max_input_channels, 1);
        runtime_config.input_channel_index =
            std::clamp(runtime_config.input_channel_index, 0, input_device_channels - 1);
        int input_channels = 1;  // The network pipeline remains mono.

        // Get output device info
        const auto* output_info_ptr = AudioStream::get_device_info(output_device);
        if (output_info_ptr == nullptr) {
            spdlog::error("Invalid output device");
            return false;
        }
        auto output_info = *output_info_ptr;

        if (!audio_backend::is_latency_certified_api(input_info.api_name)) {
            spdlog::warn(
                "Audio backend '{}' is degraded mode; no low-latency certification applies",
                input_info.api_name);
        }

        audio_state_.set_input_device_info(input_info, runtime_config.input_channel_index);
        audio_state_.set_output_device_info(output_info);

        // Initialize Opus encoder for sending own audio BEFORE starting stream
        // This prevents data race where callback might access encoder during initialization
        if (!audio_encoder_.create(runtime_config.sample_rate, input_channels,
                                   runtime_config.bitrate, runtime_config.complexity)) {
            spdlog::error("Failed to create Opus encoder");
            return false;
        }
        congestion_max_bitrate_.store(
            std::max(runtime_config.bitrate, congestion_policy::MIN_BITRATE),
            std::memory_order_release);
        congestion_target_bitrate_.store(
            std::max(runtime_config.bitrate, congestion_policy::MIN_BITRATE),
            std::memory_order_release);
        congestion_healthy_intervals_.store(0, std::memory_order_release);
        adaptive_redundancy_depth_.store(-1, std::memory_order_release);
        publish_audio_config(runtime_config);

        // Store encoder info (get actual bitrate from encoder)
        audio_state_.set_encoder_info(
            ClientEncoderInfo{input_channels, runtime_config.sample_rate,
                              runtime_config.bitrate, audio_encoder_.get_actual_bitrate(),
                              runtime_config.complexity});

        spdlog::info("Starting audio stream...");
        bool success =
            audio_.start_audio_stream(input_device, output_device, runtime_config, audio_callback,
                                      this);
        if (success) {
            start_audio_sender_thread();
            audio_.print_latency_info();
            const auto latency = audio_.get_latency_info();
            spdlog::info(
                "Audio runtime: api={} sample_rate={} requested_buffer={} actual_buffer={} "
                "input_latency_ms={:.3f} output_latency_ms={:.3f} backend_latency_available={}",
                input_info.api_name, latency.sample_rate, latency.requested_buffer_frames,
                latency.actual_buffer_frames, latency.input_latency_ms,
                latency.output_latency_ms, latency.backend_latency_available);
        } else {
            // Clean up encoder if stream start failed
            audio_encoder_.destroy();
        }
        return success;
    }

    void stop_audio_stream() {
        std::lock_guard<std::mutex> lock(audio_lifecycle_mutex_);
        audio_device_generation_.fetch_add(1, std::memory_order_acq_rel);
        audio_stream_requested_.store(false, std::memory_order_release);
        audio_recovery_in_flight_.store(false, std::memory_order_release);
        audio_.stop_audio_stream();
        stop_audio_sender_thread();
        stop_recording();
    }

    // Getters for UI access
    std::string get_server_address() const {
        return udp_network::format_address_for_display(current_server_endpoint().address());
    }

    unsigned short get_server_port() const {
        return current_server_endpoint().port();
    }

    std::string get_room_id() const {
        return join_session_.room_id();
    }

    bool is_join_confirmed() const {
        return join_session_.is_join_confirmed();
    }

    unsigned short get_local_port() const {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        return socket_.local_endpoint().port();
    }

    size_t get_participant_count() const {
        return participant_manager_.count();
    }

    bool is_audio_stream_active() const {
        return audio_.is_stream_active();
    }

    std::vector<ParticipantInfo> get_participant_info() const {
        return participant_manager_.get_all_info();
    }

    ClientChatState get_chat_state() const {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        ClientChatState state;
        state.messages = chat_messages_;
        state.unread_count = chat_unread_count_;
        state.dialog_open = chat_dialog_open_;
        state.history_truncated = chat_history_truncated_;
        state.available = chat_available_on_io_snapshot();
        state.status = chat_status_;
        state.retry_text = chat_retry_text_;
        return state;
    }

    void set_chat_dialog_open(bool open) {
        asio::post(io_context_, [this, open]() {
            {
                std::lock_guard<std::mutex> lock(chat_mutex_);
                chat_dialog_open_ = open;
                if (open) {
                    chat_unread_count_ = 0;
                }
            }
            if (open) {
                request_chat_history_on_io(last_seen_chat_sequence_snapshot());
            }
        });
    }

    bool send_chat_message(const std::string& text) {
        auto completed = std::make_shared<std::promise<bool>>();
        auto future = completed->get_future();
        asio::post(io_context_, [this, text, completed]() {
            completed->set_value(send_chat_message_on_io(text));
        });
        return future.get();
    }

    void request_chat_history() {
        asio::post(io_context_, [this]() {
            request_chat_history_on_io(last_seen_chat_sequence_snapshot());
        });
    }

    // Get own audio level (for displaying user's own microphone level)
    float get_own_audio_level() const {
        return own_audio_level_.load();
    }

    float get_own_audio_peak() const {
        return own_audio_peak_.load();
    }

    // Microphone mute control
    void set_mic_muted(bool muted) {
        mic_muted_.store(muted, std::memory_order_release);
    }

    bool get_mic_muted() const {
        return mic_muted_.load(std::memory_order_acquire);
    }

    void set_self_monitor_enabled(bool enabled) {
        self_monitor_enabled_.store(enabled, std::memory_order_release);
    }

    bool get_self_monitor_enabled() const {
        return self_monitor_enabled_.load(std::memory_order_acquire);
    }

    // Master input gain control (0.0 - 2.0, 1.0 = unity)
    void set_input_gain(float gain) {
        audio_state_.set_input_gain(gain);
    }

    float get_input_gain() const {
        return audio_state_.input_gain();
    }

    // Device and encoder info structure
    using DeviceInfo = ClientDeviceInfo;
    using EncoderInfo = ClientEncoderInfo;
    using CallbackTimingInfo = ClientCallbackTimingInfo;

    struct LatencyPercentileWindow {
        static constexpr size_t CAPACITY = 256;

        std::array<int64_t, CAPACITY> samples{};
        size_t next = 0;
        size_t count = 0;
        mutable std::mutex mutex;

        void observe(int64_t sample_ns) {
            std::lock_guard<std::mutex> lock(mutex);
            samples[next] = sample_ns;
            next = (next + 1) % CAPACITY;
            count = std::min(count + 1, CAPACITY);
        }

        int64_t percentile_99_ns() const {
            std::array<int64_t, CAPACITY> copy{};
            size_t local_count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                local_count = count;
                std::copy_n(samples.begin(), local_count, copy.begin());
            }
            if (local_count == 0) {
                return 0;
            }
            std::sort(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(local_count));
            const size_t index = std::min(
                local_count - 1,
                static_cast<size_t>(
                    std::ceil(static_cast<double>(local_count) * 0.99) - 1.0));
            return copy[index];
        }

        void clear() {
            std::lock_guard<std::mutex> lock(mutex);
            next = 0;
            count = 0;
            samples.fill(0);
        }
    };

    using PathDiagnostics = ClientPathDiagnostics;

    DeviceInfo get_device_info() const {
        return audio_state_.device_info();
    }

    EncoderInfo get_encoder_info() const {
        return audio_state_.encoder_info();
    }

    AudioStream::LatencyInfo get_latency_info() const {
        return audio_.get_latency_info();
    }

    double get_rtt_ms() const {
        return rtt_ms_.load(std::memory_order_relaxed);
    }

    PathDiagnostics get_path_diagnostics() const {
        auto ns_to_ms = [](int64_t ns) {
            return static_cast<double>(ns) / 1'000'000.0;
        };
        auto gap_percent = [](uint32_t received, uint32_t missing) {
            const uint64_t total =
                static_cast<uint64_t>(received) + static_cast<uint64_t>(missing);
            if (total == 0) {
                return 0.0;
            }
            return (static_cast<double>(missing) * 100.0) /
                   static_cast<double>(total);
        };

        PathDiagnostics diagnostics;
        diagnostics.rtt_last_ms = get_rtt_ms();
        diagnostics.rtt_min_ms = ns_to_ms(rtt_min_ns_.load(std::memory_order_relaxed));
        diagnostics.rtt_avg_ms = ns_to_ms(rtt_avg_ns_.load(std::memory_order_relaxed));
        diagnostics.rtt_max_ms = ns_to_ms(rtt_max_ns_.load(std::memory_order_relaxed));
        diagnostics.ping_received =
            ping_path_total_received_.load(std::memory_order_relaxed);
        diagnostics.ping_missing =
            ping_path_total_missing_.load(std::memory_order_relaxed);
        diagnostics.ping_consecutive_missing =
            ping_path_consecutive_missing_.load(std::memory_order_relaxed);
        diagnostics.ping_gap_percent =
            gap_percent(diagnostics.ping_received, diagnostics.ping_missing);
        diagnostics.audio_ingress_received =
            audio_path_interval_received_.load(std::memory_order_relaxed);
        diagnostics.audio_ingress_gaps =
            audio_path_interval_gaps_.load(std::memory_order_relaxed);
        diagnostics.audio_ingress_gap_percent =
            client_network_path::net_gap_rate(
                diagnostics.audio_ingress_received,
                audio_path_interval_sequence_gaps_.load(std::memory_order_relaxed),
                diagnostics.audio_ingress_gaps) *
            100.0;
        diagnostics.opus_send_queue_avg_ms =
            ns_to_ms(opus_send_queue_age_avg_ns_.load(std::memory_order_relaxed));
        diagnostics.opus_send_queue_max_ms =
            ns_to_ms(opus_send_queue_age_max_ns_.load(std::memory_order_relaxed));
        diagnostics.opus_send_queue_p99_ms =
            ns_to_ms(opus_send_queue_age_p99_ns_.load(std::memory_order_relaxed));
        const auto latency = get_latency_info();
        const double fallback_buffer_ms =
            latency.buffer_duration_ms > 0.0 ? latency.buffer_duration_ms
                                              : get_opus_network_packet_ms();
        diagnostics.total_input_ms =
            latency.input_latency_ms > 0.0 ? latency.input_latency_ms : fallback_buffer_ms;
        diagnostics.total_opus_ms = get_opus_network_packet_ms();
        diagnostics.total_network_ms = std::max(0.0, diagnostics.rtt_last_ms * 0.5);
        diagnostics.total_jitter_ms =
            static_cast<double>(get_opus_jitter_buffer_packets()) *
            diagnostics.total_opus_ms;
        diagnostics.total_output_ms =
            latency.output_latency_ms > 0.0 ? latency.output_latency_ms : fallback_buffer_ms;
        diagnostics.total_estimate_ms =
            diagnostics.total_input_ms + diagnostics.total_opus_ms +
            diagnostics.total_network_ms + diagnostics.total_jitter_ms +
            diagnostics.total_output_ms + diagnostics.opus_send_queue_avg_ms;
        diagnostics.tx_pace_avg_ms =
            ns_to_ms(tx_send_pace_avg_ns_.load(std::memory_order_relaxed));
        diagnostics.tx_pace_max_ms =
            ns_to_ms(tx_send_pace_max_ns_.load(std::memory_order_relaxed));
        diagnostics.udp_rebind_count =
            udp_path_rebind_count_.load(std::memory_order_relaxed);

        for (const auto& participant: participant_manager_.get_all_info()) {
            diagnostics.rx_queue_current += participant.queue_size;
            diagnostics.rx_queue_avg_max =
                std::max(diagnostics.rx_queue_avg_max, participant.queue_size_avg);
            diagnostics.rx_queue_peak =
                std::max(diagnostics.rx_queue_peak, participant.queue_size_max);
            diagnostics.underruns += participant.underrun_count;
            diagnostics.plc_frames += participant.plc_count;
            diagnostics.age_drops += participant.jitter_age_drops;
            diagnostics.e2e_latency_avg_max_ms =
                std::max(diagnostics.e2e_latency_avg_max_ms,
                         participant.capture_to_playout_latency_avg_ms);
            diagnostics.e2e_latency_peak_ms =
                std::max(diagnostics.e2e_latency_peak_ms,
                         participant.capture_to_playout_latency_max_ms);
            diagnostics.e2e_latency_samples +=
                participant.capture_to_playout_latency_samples;
        }
        return diagnostics;
    }

    uint64_t get_total_bytes_rx() const {
        return total_bytes_rx_.load(std::memory_order_relaxed);
    }

    uint64_t get_total_bytes_tx() const {
        return total_bytes_tx_.load(std::memory_order_relaxed);
    }

    uint64_t get_stray_udp_packets() const {
        return stray_udp_packets_.load(std::memory_order_relaxed);
    }

    uint64_t get_inbound_malformed_audio_drops() const {
        return inbound_malformed_audio_drops_.load(std::memory_order_relaxed);
    }

    AudioStream::AudioConfig get_audio_config() const {
        return audio_state_.config();
    }

    int current_audio_sample_rate() const {
        return audio_state_.sample_rate();
    }

    int current_audio_frames_per_buffer() const {
        return audio_state_.frames_per_buffer();
    }

    void publish_audio_config(const AudioStream::AudioConfig& config) {
        audio_state_.publish_config(config);
    }

    void set_requested_frames_per_buffer(int frames_per_buffer) {
        audio_state_.set_requested_frames_per_buffer(frames_per_buffer);
    }

    int get_input_channel_index() const {
        return audio_state_.input_channel_index();
    }

    int max_input_channel_count_for_device(AudioStream::DeviceIndex device_index) const {
        const auto* input_info = AudioStream::get_device_info(device_index);
        if (input_info == nullptr) {
            return 1;
        }
        return std::max(input_info->max_input_channels, 1);
    }

    void set_input_channel_index(int channel_index) {
        audio_state_.set_input_channel_index(
            channel_index, max_input_channel_count_for_device(get_selected_input_device()));
    }

    uint16_t get_opus_network_frame_count() const {
        return opus_network_frame_count_.load(std::memory_order_acquire);
    }

    double get_opus_network_packet_ms() const {
        const uint32_t sample_rate =
            static_cast<uint32_t>(std::max(1, current_audio_sample_rate()));
        return opus_network_clock::frame_duration_ms(sample_rate, get_opus_network_frame_count());
    }

    size_t opus_jitter_packets_for_target_ms(int target_ms) const {
        const uint32_t sample_rate =
            static_cast<uint32_t>(std::max(1, current_audio_sample_rate()));
        const uint16_t frame_count = get_opus_network_frame_count();
        size_t packets = opus_jitter_packets_for_ms(
            target_ms,
            sample_rate,
            frame_count);
        const int age_limit_ms = get_jitter_packet_age_limit_ms();
        if (age_limit_ms > 0) {
            packets = std::min(packets,
                               opus_jitter_packets_within_ms(age_limit_ms,
                                                             sample_rate,
                                                             frame_count));
        }
        return packets;
    }

    int opus_jitter_effective_ms_for_packets(size_t packets) const {
        return opus_jitter_ms_for_packets(
            packets,
            static_cast<uint32_t>(std::max(1, current_audio_sample_rate())),
            get_opus_network_frame_count());
    }

    void set_opus_network_frame_count(int frame_count) {
        const uint32_t sample_rate =
            static_cast<uint32_t>(std::max(1, current_audio_sample_rate()));
        const uint16_t normalized =
            opus_network_clock::normalize_frame_count(sample_rate, frame_count);
        {
            std::lock_guard lock(packet_duration_mutex_);
            congestion_policy::configure_packet_duration(packet_duration_state_,
                                                          normalized);
        }
        apply_opus_network_frame_count(normalized);
    }

    void apply_opus_network_frame_count(uint16_t normalized) {
        const uint32_t sample_rate =
            static_cast<uint32_t>(std::max(1, current_audio_sample_rate()));
        const uint16_t previous =
            opus_network_frame_count_.exchange(normalized, std::memory_order_acq_rel);
        if (previous != normalized) {
            opus_tx_accumulator_reset_requested_.store(true, std::memory_order_release);
            spdlog::info("Opus network packet changed from {} to {} frames ({:.1f} ms)",
                      previous, normalized,
                      opus_network_clock::frame_duration_ms(sample_rate, normalized));
            apply_opus_jitter_buffer_ms(
                opus_jitter_buffer_ms_.load(std::memory_order_acquire));
        }
    }

    void reset_packet_duration_adaptation() {
        uint16_t configured_frames = opus_network_clock::DEFAULT_FRAME_COUNT;
        {
            std::lock_guard lock(packet_duration_mutex_);
            configured_frames = static_cast<uint16_t>(
                packet_duration_state_.configured_frames);
            congestion_policy::configure_packet_duration(packet_duration_state_,
                                                          configured_frames);
        }
        apply_opus_network_frame_count(configured_frames);
    }

    size_t get_opus_jitter_buffer_packets() const {
        return opus_jitter_packets_for_target_ms(get_opus_jitter_buffer_ms());
    }

    int get_opus_jitter_buffer_ms() const {
        return opus_jitter_buffer_ms_.load(std::memory_order_acquire);
    }

    size_t get_opus_auto_start_jitter_packets() const {
        return opus_auto_start_jitter_packets_for_audio(
            get_opus_jitter_buffer_packets(),
            static_cast<uint32_t>(std::max(1, current_audio_sample_rate())),
            get_opus_network_frame_count());
    }

    uint16_t opus_frame_count_for_participant(const ParticipantData& participant) const {
        const size_t sender_frame_count =
            participant.last_packet_frame_count.load(std::memory_order_relaxed);
        return sender_frame_count > 0
                   ? static_cast<uint16_t>(sender_frame_count)
                   : get_opus_network_frame_count();
    }

    size_t opus_jitter_packets_for_participant_ms(
        const ParticipantData& participant, int target_ms) const {
        const uint16_t frame_count = opus_frame_count_for_participant(participant);
        size_t packets = jitter_floor_packets_for_audio(
            frame_count, target_ms, opus_network_clock::SAMPLE_RATE);
        const int age_limit_ms = get_jitter_packet_age_limit_ms();
        if (age_limit_ms > 0) {
            packets = std::min(
                packets,
                opus_jitter_packets_within_ms(
                    age_limit_ms, opus_network_clock::SAMPLE_RATE, frame_count));
        }
        return packets;
    }

    size_t opus_auto_start_jitter_packets_for_participant(
        const ParticipantData& participant, size_t floor_packets) const {
        return opus_auto_start_jitter_packets_for_audio(
            floor_packets, opus_network_clock::SAMPLE_RATE,
            opus_frame_count_for_participant(participant));
    }

    int get_opus_auto_start_jitter_ms() const {
        return opus_jitter_effective_ms_for_packets(get_opus_auto_start_jitter_packets());
    }

    size_t get_opus_queue_limit_packets() const {
        return opus_queue_limit_packets_.load(std::memory_order_acquire);
    }

    static int normalize_opus_redundancy_depth(int depth) {
        if (depth < 0) {
            return OPUS_REDUNDANCY_DEPTH_AUTO;
        }
        return std::clamp(depth, 0, MAX_OPUS_REDUNDANCY_DEPTH_PACKETS);
    }

    static int auto_opus_redundancy_depth_for_frame_count(uint16_t frame_count) {
        if (frame_count <= opus_network_clock::LOW_LATENCY_FRAME_COUNT) {
            return 1;
        }
        if (frame_count >= opus_network_clock::STABLE_FRAME_COUNT) {
            return 3;
        }
        return 2;
    }

    static int effective_opus_redundancy_depth(int configured_depth,
                                               uint16_t frame_count) {
        const int normalized = normalize_opus_redundancy_depth(configured_depth);
        if (normalized == OPUS_REDUNDANCY_DEPTH_AUTO) {
            return auto_opus_redundancy_depth_for_frame_count(frame_count);
        }
        return normalized;
    }

    static size_t opus_redundancy_child_count_for_policy(int configured_depth,
                                                         uint16_t frame_count,
                                                         size_t available_previous) {
        const int effective_depth =
            effective_opus_redundancy_depth(configured_depth, frame_count);
        if (effective_depth <= 0) {
            return 1;
        }
        return 1 + std::min(available_previous, static_cast<size_t>(effective_depth));
    }

    int get_opus_redundancy_depth_setting() const {
        return opus_redundancy_depth_packets_.load(std::memory_order_acquire);
    }

    int get_effective_opus_redundancy_depth() const {
        const int adaptive = adaptive_redundancy_depth_.load(std::memory_order_acquire);
        if (adaptive >= 0) {
            return adaptive;
        }
        return effective_opus_redundancy_depth(
            get_opus_redundancy_depth_setting(),
            static_cast<uint16_t>(get_opus_network_frame_count()));
    }

    void set_opus_redundancy_depth(int depth) {
        opus_redundancy_depth_packets_.store(normalize_opus_redundancy_depth(depth),
                                             std::memory_order_release);
    }

    int get_jitter_packet_age_limit_ms() const {
        return jitter_packet_age_limit_ms_.load(std::memory_order_acquire);
    }

    static void apply_opus_jitter_policy_to_participant(ParticipantData& participant,
                                                        size_t floor_packets,
                                                        size_t auto_start_packets,
                                                        bool auto_enabled,
                                                        bool reset_target) {
        const size_t floor = clamp_opus_jitter_packets(floor_packets);
        const size_t auto_start =
            std::max(floor, clamp_opus_jitter_packets(auto_start_packets));
        const size_t target =
            auto_enabled ? auto_start : floor;

        participant.opus_jitter_auto_enabled.store(auto_enabled,
                                                   std::memory_order_relaxed);
        participant.opus_jitter_auto_floor_packets.store(floor,
                                                         std::memory_order_relaxed);
        participant.opus_jitter_auto_stable_callbacks.store(0,
                                                            std::memory_order_relaxed);
        participant.opus_jitter_auto_instability_events.store(0,
                                                              std::memory_order_relaxed);
        participant.jitter_buffer_floor_packets.store(floor,
                                                      std::memory_order_relaxed);

        const size_t current_target =
            participant.jitter_buffer_min_packets.load(std::memory_order_relaxed);
        if (reset_target || current_target < target) {
            participant.jitter_buffer_min_packets.store(target,
                                                        std::memory_order_relaxed);
            const size_t queue_limit =
                participant.opus_queue_limit_packets.load(std::memory_order_relaxed);
            participant.opus_queue_limit_packets.store(
                std::max(queue_limit, target + 3), std::memory_order_relaxed);
            if (participant.opus_queue.size_approx() < std::max<size_t>(1, target)) {
                participant.buffer_ready.store(false, std::memory_order_relaxed);
            }
        }

        participant.opus_consecutive_empty_callbacks.store(0,
                                                           std::memory_order_relaxed);
    }

    void set_opus_jitter_buffer_packets(size_t packets) {
        const size_t clamped =
            std::clamp(packets, MIN_OPUS_JITTER_PACKETS, MAX_OPUS_JITTER_PACKETS);
        set_opus_jitter_buffer_ms(opus_jitter_effective_ms_for_packets(clamped));
    }

    static int clamp_opus_jitter_ms_for_age_limit(int target_ms, int age_limit_ms) {
        int clamped_ms = std::clamp(target_ms, 0, MAX_JITTER_PACKET_AGE_MS);
        if (age_limit_ms > 0) {
            clamped_ms = std::min(clamped_ms, age_limit_ms);
        }
        return clamped_ms;
    }

    void set_opus_jitter_buffer_ms(int target_ms) {
        const int clamped_ms =
            clamp_opus_jitter_ms_for_age_limit(target_ms, get_jitter_packet_age_limit_ms());
        apply_opus_jitter_buffer_ms(clamped_ms);
    }

    void apply_opus_jitter_buffer_ms(int clamped_ms) {
        opus_jitter_buffer_ms_.store(clamped_ms, std::memory_order_release);

        participant_manager_.for_each([this, clamped_ms](
                                          uint32_t, ParticipantData& participant) {
            if (participant.opus_jitter_manual_override.load(std::memory_order_relaxed)) {
                return;
            }
            if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus ||
                participant.jitter_buffer_floor_packets.load(std::memory_order_relaxed) >=
                    DEFAULT_OPUS_JITTER_PACKETS) {
                const size_t floor_packets =
                    opus_jitter_packets_for_participant_ms(participant, clamped_ms);
                const size_t auto_start_packets =
                    opus_auto_start_jitter_packets_for_participant(participant,
                                                                   floor_packets);
                apply_opus_jitter_policy_to_participant(
                    participant, floor_packets, auto_start_packets,
                    participant.opus_jitter_auto_enabled.load(std::memory_order_relaxed),
                    true);
            }
        });
    }

    void set_opus_queue_limit_packets(size_t packets) {
        const size_t min_limit =
            std::max(MIN_OPUS_QUEUE_LIMIT_PACKETS, get_opus_jitter_buffer_packets());
        const size_t clamped = std::clamp(packets, min_limit, MAX_OPUS_QUEUE_LIMIT_PACKETS);
        opus_queue_limit_packets_.store(clamped, std::memory_order_release);

        participant_manager_.for_each([clamped](uint32_t, ParticipantData& participant) {
            participant.opus_queue_limit_packets.store(clamped, std::memory_order_relaxed);
        });
    }

    void set_jitter_packet_age_limit_ms(int age_ms) {
        const int clamped =
            std::clamp(age_ms, MIN_JITTER_PACKET_AGE_MS, MAX_JITTER_PACKET_AGE_MS);
        jitter_packet_age_limit_ms_.store(clamped, std::memory_order_release);
        if (clamped <= 0) {
            return;
        }

        const int previous_jitter_ms = get_opus_jitter_buffer_ms();
        if (previous_jitter_ms > clamped) {
            apply_opus_jitter_buffer_ms(clamped);
            spdlog::info("Clamped global Opus jitter from {} ms to packet age limit {} ms",
                      previous_jitter_ms, clamped);
        }

        participant_manager_.for_each([this, clamped](uint32_t,
                                                      ParticipantData& participant) {
            const size_t max_packets = opus_jitter_packets_within_ms(
                clamped, opus_network_clock::SAMPLE_RATE,
                opus_frame_count_for_participant(participant));
            const size_t current_target =
                participant.jitter_buffer_min_packets.load(std::memory_order_relaxed);
            if (current_target <= max_packets) {
                return;
            }
            participant.jitter_buffer_min_packets.store(max_packets,
                                                        std::memory_order_relaxed);
            participant.jitter_buffer_floor_packets.store(
                std::min(participant.jitter_buffer_floor_packets.load(
                             std::memory_order_relaxed),
                         max_packets),
                std::memory_order_relaxed);
            participant.opus_jitter_auto_floor_packets.store(
                std::min(participant.opus_jitter_auto_floor_packets.load(
                             std::memory_order_relaxed),
                         max_packets),
                std::memory_order_relaxed);
        });
    }

    void set_opus_auto_jitter_default(bool enabled) {
        opus_auto_jitter_default_.store(enabled, std::memory_order_release);
        participant_manager_.for_each([this, enabled](
                                          uint32_t, ParticipantData& participant) {
            if (participant.opus_jitter_manual_override.load(std::memory_order_relaxed)) {
                return;
            }
            const size_t global_default = opus_jitter_packets_for_participant_ms(
                participant, get_opus_jitter_buffer_ms());
            const size_t auto_start_packets =
                opus_auto_start_jitter_packets_for_participant(participant,
                                                               global_default);
            apply_opus_jitter_policy_to_participant(
                participant, global_default, auto_start_packets, enabled, !enabled);
        });
    }

    bool get_opus_auto_jitter_default() const {
        return opus_auto_jitter_default_.load(std::memory_order_acquire);
    }

    void set_participant_opus_jitter_buffer_ms(uint32_t id, int target_ms) {
        const int clamped_ms =
            clamp_opus_jitter_ms_for_age_limit(target_ms, get_jitter_packet_age_limit_ms());
        participant_manager_.with_participant(id, [this, clamped_ms](
                                                      ParticipantData& participant) {
            const size_t clamped =
                opus_jitter_packets_for_participant_ms(participant, clamped_ms);
            participant.opus_jitter_manual_override.store(true, std::memory_order_relaxed);
            participant.opus_jitter_override_ms.store(clamped_ms, std::memory_order_relaxed);
            participant.opus_jitter_auto_enabled.store(false, std::memory_order_relaxed);
            participant.jitter_buffer_floor_packets.store(clamped, std::memory_order_relaxed);
            participant.jitter_buffer_min_packets.store(clamped, std::memory_order_relaxed);
            const size_t queue_limit =
                participant.opus_queue_limit_packets.load(std::memory_order_relaxed);
            participant.opus_queue_limit_packets.store(std::max(queue_limit, clamped),
                                                       std::memory_order_relaxed);
            if (participant.opus_queue.size_approx() < std::max<size_t>(1, clamped)) {
                participant.buffer_ready.store(false, std::memory_order_relaxed);
            }
            participant.opus_consecutive_empty_callbacks.store(0, std::memory_order_relaxed);
        });
    }

    void reset_participant_opus_jitter_buffer_packets(uint32_t id) {
        participant_manager_.with_participant(id, [this](
                                                  ParticipantData& participant) {
            const size_t global_default = opus_jitter_packets_for_participant_ms(
                participant, get_opus_jitter_buffer_ms());
            const size_t auto_start_packets =
                opus_auto_start_jitter_packets_for_participant(participant,
                                                               global_default);
            participant.opus_jitter_manual_override.store(false, std::memory_order_relaxed);
            participant.opus_jitter_override_ms.store(0, std::memory_order_relaxed);
            apply_opus_jitter_policy_to_participant(
                participant, global_default, auto_start_packets, false, true);
        });
    }

    void set_participant_opus_auto_jitter(uint32_t id, bool enabled) {
        participant_manager_.with_participant(id, [this, enabled](
                                                  ParticipantData& participant) {
            const size_t global_default = opus_jitter_packets_for_participant_ms(
                participant, get_opus_jitter_buffer_ms());
            const size_t auto_start_packets =
                opus_auto_start_jitter_packets_for_participant(participant,
                                                               global_default);
            participant.opus_jitter_manual_override.store(false, std::memory_order_relaxed);
            participant.opus_jitter_override_ms.store(0, std::memory_order_relaxed);
            apply_opus_jitter_policy_to_participant(
                participant, global_default, auto_start_packets, enabled, !enabled);
        });
    }

    void apply_default_opus_jitter_policy(ParticipantData& participant) {
        if (participant.opus_jitter_manual_override.load(std::memory_order_relaxed)) {
            return;
        }
        // Before the first remote packet, use the local frame count. The arrival
        // path re-derives this policy as soon as the sender's duration is known.
        const size_t floor_packets = opus_jitter_packets_for_participant_ms(
            participant, get_opus_jitter_buffer_ms());
        apply_opus_jitter_policy_to_participant(
            participant, floor_packets,
            opus_auto_start_jitter_packets_for_participant(participant, floor_packets),
            opus_auto_jitter_default_.load(std::memory_order_acquire), true);
    }

    CallbackTimingInfo get_callback_timing_info() const {
        CallbackTimingInfo info{};
        info.last_ms = callback_last_ns_.load(std::memory_order_relaxed) / 1e6;
        info.max_ms = callback_max_ns_.load(std::memory_order_relaxed) / 1e6;
        info.avg_ms = callback_avg_ns_.load(std::memory_order_relaxed) / 1e6;
        info.deadline_ms = callback_deadline_ns_.load(std::memory_order_relaxed) / 1e6;
        info.callback_count = callback_count_.load(std::memory_order_relaxed);
        info.over_deadline_count = callback_over_deadline_count_.load(std::memory_order_relaxed);
        return info;
    }

    using MetronomeState = ClientMetronomeState;

    MetronomeState get_metronome_state() const {
        return metronome_.state(server_clock_ready_.load(std::memory_order_acquire),
                                server_clock_offset_ns_.load(std::memory_order_relaxed));
    }

    void set_metronome_bpm(float bpm, bool send_sync = true) {
        const int bpm_milli = std::clamp(static_cast<int>(std::lrint(bpm * 1000.0F)), 30000,
                                         240000);
        if (send_sync) {
            commit_metronome_bpm_milli(bpm_milli);
        } else {
            metronome_.set_bpm_milli_local(bpm_milli);
        }
    }

    void commit_metronome_bpm(float bpm) {
        const int bpm_milli = std::clamp(static_cast<int>(std::lrint(bpm * 1000.0F)), 30000,
                                         240000);
        commit_metronome_bpm_milli(bpm_milli);
    }

    void start_metronome() {
        send_metronome_sync(metronome_.bpm_milli(), true, 0);
    }

    void stop_metronome() {
        send_metronome_sync(metronome_.bpm_milli(), false, metronome_.beat_number());
    }

    void tap_metronome_tempo() {
        const auto bpm_milli = metronome_.tap_tempo_bpm_milli(std::chrono::steady_clock::now());
        if (bpm_milli.has_value()) {
            commit_metronome_bpm_milli(*bpm_milli);
        }
    }

    using RecordingState = ClientRecordingState;

    RecordingState get_recording_state() const {
        return media_state_.recording_state();
    }

    bool start_recording() {
        const bool started = media_state_.start_recording(
            static_cast<uint32_t>(current_audio_sample_rate()));
        if (started) {
            spdlog::info("Recording started: {}", media_state_.recording_state().folder);
        } else {
            spdlog::error("Recording failed to start");
        }
        return started;
    }

    void stop_recording() {
        const auto state = media_state_.recording_state();
        media_state_.stop_recording();
        if (state.active && !state.folder.empty()) {
            spdlog::info("Recording stopped: {}", state.folder);
        }
    }

    // WAV file playback methods
    bool load_wav_file(const std::string& path) {
        return media_state_.load_wav_file(path);
    }

    void wav_play() {
        media_state_.wav_play();
    }

    void wav_pause() {
        media_state_.wav_pause();
    }

    void wav_seek(int64_t frame_position) {
        media_state_.wav_seek(frame_position);
    }

    using WavState = ClientWavState;

    void set_wav_gain(float gain) {
        media_state_.set_wav_gain(gain);
    }

    float get_wav_gain() const {
        return media_state_.wav_gain();
    }

    void set_wav_muted_local(bool muted) {
        media_state_.set_wav_muted_local(muted);
    }

    bool get_wav_muted_local() const {
        return media_state_.wav_muted_local();
    }

    WavState get_wav_state() const {
        return media_state_.wav_state();
    }

    // Device selection methods (removed - use AudioStream static methods directly)

    AudioStream::DeviceIndex get_selected_input_device() const {
        return audio_state_.selected_input_device();
    }

    AudioStream::DeviceIndex get_selected_output_device() const {
        return audio_state_.selected_output_device();
    }

    std::string get_audio_api_filter() const {
        return audio_state_.audio_api_filter();
    }

    void set_audio_api_filter(std::string api_filter) {
        audio_state_.set_audio_api_filter(std::move(api_filter));
    }

    bool save_audio_device_preferences() const {
        return ::save_audio_device_preferences(
            audio_preferences_path_, get_audio_api_filter(), get_selected_input_device(),
            get_selected_output_device(), get_audio_config().input_channel_index);
    }

    bool set_input_device(AudioStream::DeviceIndex device_index) {
        const auto* input_info = AudioStream::get_device_info(device_index);
        if (input_info == nullptr || input_info->max_input_channels <= 0) {
            spdlog::error("Invalid input device index: {}", device_index);
            return false;
        }
        audio_state_.set_selected_input_device(device_index);

        auto config = get_audio_config();
        const int input_channel_count = std::max(input_info->max_input_channels, 1);
        config.input_channel_index =
            std::clamp(config.input_channel_index, 0, input_channel_count - 1);
        publish_audio_config(config);
        audio_state_.set_input_device_info(*input_info, config.input_channel_index);
        return true;
    }

    bool set_output_device(AudioStream::DeviceIndex device_index) {
        const auto* output_info = AudioStream::get_device_info(device_index);
        if (output_info == nullptr || output_info->max_output_channels <= 0) {
            spdlog::error("Invalid output device index: {}", device_index);
            return false;
        }
        audio_state_.set_selected_output_device(device_index);

        audio_state_.set_output_device_info(*output_info);
        return true;
    }

    // Hot-swap audio devices (stops current stream and starts new one)
    bool swap_audio_devices(AudioStream::DeviceIndex input_device,
                            AudioStream::DeviceIndex output_device) {
        bool was_active = audio_.is_stream_active();

        // Stop current stream if active
        if (was_active) {
            stop_audio_stream();
        }

        if (!set_input_device(input_device) || !set_output_device(output_device)) {
            return false;
        }

        // Start new stream if it was active before
        if (was_active) {
            return start_audio_stream(input_device, output_device, get_audio_config());
        }

        return true;
    }

    void reset_audio_path() {
        const bool was_active = audio_.is_stream_active();
        const auto input_device = get_selected_input_device();
        const auto output_device = get_selected_output_device();
        const auto config = get_audio_config();

        if (was_active) {
            stop_audio_stream();
        }

        clear_audio_path_queues();

        if (was_active && input_device != AudioStream::NO_DEVICE &&
            output_device != AudioStream::NO_DEVICE) {
            start_audio_stream(input_device, output_device, config);
        }

        spdlog::warn("Manual audio path reset: cleared local queues and restarted audio stream");
    }

    // =========================================================================
    // Participant control methods (mute, gain, pan)
    // =========================================================================

    // Set participant mute state
    void set_participant_muted(uint32_t id, bool muted) {
        participant_manager_.with_participant(id,
                                              [muted](ParticipantData& p) {
                                                  p.is_muted.store(muted,
                                                                   std::memory_order_relaxed);
                                              });
    }

    // Get participant mute state
    bool get_participant_muted(uint32_t id) {
        bool muted = false;
        participant_manager_.with_participant(id, [&muted](ParticipantData& p) {
            muted = p.is_muted.load(std::memory_order_relaxed);
        });
        return muted;
    }

    // Set participant gain (0.0 - 2.0, 1.0 = unity)
    void set_participant_gain(uint32_t id, float gain) {
        participant_manager_.with_participant(
            id, [gain](ParticipantData& p) {
                p.gain.store(std::clamp(gain, 0.0F, 2.0F), std::memory_order_relaxed);
            });
    }

    // Get participant gain
    float get_participant_gain(uint32_t id) {
        float gain = 1.0F;
        participant_manager_.with_participant(id, [&gain](ParticipantData& p) {
            gain = p.gain.load(std::memory_order_relaxed);
        });
        return gain;
    }

    // Set participant pan (0.0 = full left, 0.5 = center, 1.0 = full right)
    void set_participant_pan(uint32_t id, float pan) {
        participant_manager_.with_participant(
            id, [pan](ParticipantData& p) {
                p.pan.store(std::clamp(pan, 0.0F, 1.0F), std::memory_order_relaxed);
            });
    }

    // Get participant pan
    float get_participant_pan(uint32_t id) {
        float pan = 0.5F;
        participant_manager_.with_participant(id, [&pan](ParticipantData& p) {
            pan = p.pan.load(std::memory_order_relaxed);
        });
        return pan;
    }

    struct ReceiveState {
        std::array<char, 2048> buffer{};
        udp::endpoint endpoint;
        uint64_t generation = 0;
        bool in_flight = false;
    };

    static constexpr size_t RECEIVE_SLOT_COUNT = 8;

    void reset_server_clock_and_ping_state() {
        {
            std::lock_guard lock(server_clock_estimator_mutex_);
            server_clock_estimator_.reset();
        }
        server_clock_ready_.store(false, std::memory_order_release);
        server_clock_offset_ns_.store(0, std::memory_order_release);
        server_clock_uncertainty_ns_.store(0, std::memory_order_release);
        ping_tx_sequence_.store(0, std::memory_order_release);
        reset_ping_path_feedback(0);
        rtt_ms_.store(0.0, std::memory_order_relaxed);
        rtt_last_ns_.store(0, std::memory_order_relaxed);
        rtt_min_ns_.store(0, std::memory_order_relaxed);
        rtt_avg_ns_.store(0, std::memory_order_relaxed);
        rtt_max_ns_.store(0, std::memory_order_relaxed);
    }

    void reset_ping_path_feedback(uint32_t watch_start_sequence) {
        have_ping_reply_sequence_.store(false, std::memory_order_release);
        last_ping_reply_sequence_.store(0, std::memory_order_release);
        ping_path_watch_start_sequence_.store(watch_start_sequence,
                                             std::memory_order_release);
        ping_path_interval_received_.store(0, std::memory_order_relaxed);
        ping_path_interval_missing_.store(0, std::memory_order_relaxed);
        ping_path_total_received_.store(0, std::memory_order_relaxed);
        ping_path_total_missing_.store(0, std::memory_order_relaxed);
        ping_path_consecutive_missing_.store(0, std::memory_order_relaxed);
    }

    void reset_ping_path_feedback_to_current_sequence() {
        reset_ping_path_feedback(ping_tx_sequence_.load(std::memory_order_acquire));
    }

    void on_receive(ReceiveState& state,
                    std::error_code error_code, std::size_t bytes) {
        state.in_flight = false;
        if (state.generation != receive_generation_.load(std::memory_order_acquire)) {
            do_receive(state);
            return;
        }

        if (!receiving_enabled_.load(std::memory_order_acquire)) {
            return;
        }

        if (error_code) {
            if (error_code == asio::error::operation_aborted) {
                do_receive(state);
                return;
            }
            do_receive(state);  // keep this receive slot listening
            return;
        }

        // Copy into bounded stack storage, then return the fixed slot to the
        // kernel before validation, decryption, or queue admission.
        std::array<char, 2048> packet{};
        bytes = std::min(bytes, packet.size());
        std::memcpy(packet.data(), state.buffer.data(), bytes);
        const auto received_endpoint = state.endpoint;
        do_receive(state);

        const auto expected_endpoint = current_server_endpoint();
        if (received_endpoint != expected_endpoint) {
            const uint64_t count =
                stray_udp_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn(
                    "Ignoring UDP packet from unexpected endpoint {}:{} (server is {}:{}, "
                    "ignored={})",
                    received_endpoint.address().to_string(), received_endpoint.port(),
                    expected_endpoint.address().to_string(), expected_endpoint.port(), count);
            }
            return;
        }

        if (bytes < sizeof(MsgHdr)) {
            return;
        }

        MsgHdr hdr{};
        std::memcpy(&hdr, packet.data(), sizeof(MsgHdr));

        if (hdr.magic == PING_MAGIC && bytes >= sizeof(SyncHdr)) {
            handle_ping_message(bytes, packet.data());
        } else if (hdr.magic == CTRL_MAGIC && bytes >= sizeof(CtrlHdr)) {
            handle_ctrl_message(bytes, packet.data());
        } else if (hdr.magic == SECURE_CONTROL_MAGIC &&
                   bytes >= SECURE_CONTROL_HEADER_BYTES + SECURE_PACKET_TAG_BYTES) {
            handle_secure_control_message(bytes, packet.data());
        } else if (hdr.magic == SECURE_AUDIO_MAGIC &&
                   bytes >= SECURE_PACKET_HEADER_BYTES + SECURE_PACKET_TAG_BYTES) {
            handle_secure_audio_message(bytes, packet.data());
        } else if (hdr.magic == E2E_KEY_ENVELOPE_MAGIC &&
                   bytes >= sizeof(E2EKeyEnvelopeHdr)) {
            handle_e2e_key_envelope(bytes, packet.data());
        } else {
            // Log unknown message with hex dump for debugging
            std::string hex_dump;
            hex_dump.reserve(bytes * 3);
            for (size_t i = 0; i < std::min(bytes, size_t(32)); ++i) {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%02x ",
                              static_cast<unsigned char>(packet[i]));
                hex_dump += hex;
            }
            spdlog::warn("Unknown message (magic=0x{:08x}, bytes={}, hex={}...)", hdr.magic, bytes,
                      hex_dump);
        }
    }

    void do_receive(ReceiveState& state) {
        if (!receiving_enabled_.load(std::memory_order_acquire)) {
            return;
        }
        if (state.in_flight) {
            return;
        }
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (!receiving_enabled_.load(std::memory_order_acquire)) {
            return;
        }
        state.generation = receive_generation_.load(std::memory_order_acquire);
        state.in_flight = true;
        socket_.async_receive_from(asio::buffer(state.buffer), state.endpoint,
                                   [this, &state](std::error_code error_code, std::size_t bytes) {
                                       on_receive(state, error_code, bytes);
                                   });
    }

    void start_receive_slots() {
        for (auto& state: receive_states_) {
            do_receive(state);
        }
    }

private:
    struct OutboundEndpointSnapshot {
        udp::endpoint endpoint;
        uint64_t generation = 0;
    };

    struct DeliveryReportAccumulator {
        uint32_t report_epoch = 0;
        delivery_report_feedback::LifetimeCounter delivered;
        delivery_report_feedback::LifetimeCounter lost;
        uint64_t last_sent_delivered = 0;
        uint64_t last_sent_lost = 0;
        std::chrono::steady_clock::time_point inactive_since{};
    };

    struct DownstreamFeedbackInterval {
        delivery_report_feedback::ResolvedOutcomes outcomes;
        std::chrono::steady_clock::time_point updated_at{};
    };

    struct DownstreamDeliveryReporter {
        delivery_report_feedback::State report;
        uint64_t claimed_outcomes = 0;
    };

    static constexpr auto DELIVERY_REPORT_STATE_RETENTION = std::chrono::minutes(5);
    static constexpr auto DOWNSTREAM_FEEDBACK_RETENTION = std::chrono::seconds(3);

    uint32_t next_delivery_report_epoch() {
        uint32_t epoch = next_delivery_report_epoch_++;
        if (epoch == 0) {
            epoch = next_delivery_report_epoch_++;
        }
        return epoch;
    }

    OutboundEndpointSnapshot current_outbound_endpoint_snapshot() const {
        std::lock_guard<std::mutex> lock(server_endpoint_mutex_);
        return OutboundEndpointSnapshot{
            server_endpoint_,
            outbound_generation_.load(std::memory_order_acquire),
        };
    }

public:
    // Send with optional shared_ptr to keep data alive during async operation
    void send(void* data, std::size_t len,
              const std::shared_ptr<std::vector<unsigned char>>& keep_alive = nullptr) {
        if (!validate_outbound_audio_packet(data, len)) {
            return;
        }

        if (!outbound_enabled_.load(std::memory_order_acquire)) {
            return;
        }

        auto send_buffer = keep_alive;
        if (send_buffer == nullptr) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            send_buffer = std::make_shared<std::vector<unsigned char>>(bytes, bytes + len);
        }

        const auto outbound = current_outbound_endpoint_snapshot();
        asio::post(io_context_, [this, send_buffer, len, outbound]() {
            udp_network::QosResult qos;
            if (!outbound_enabled_.load(std::memory_order_acquire) ||
                outbound.generation !=
                    outbound_generation_.load(std::memory_order_acquire)) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                if (!outbound_enabled_.load(std::memory_order_acquire) ||
                    outbound.generation !=
                        outbound_generation_.load(std::memory_order_acquire)) {
                    return;
                }
                qos = socket_qos_.ensure_flow(socket_, outbound.endpoint);
                total_bytes_tx_.fetch_add(len, std::memory_order_relaxed);
                socket_.async_send_to(asio::buffer(send_buffer->data(), send_buffer->size()),
                                      outbound.endpoint,
                                      [send_buffer](std::error_code error_code, std::size_t) {
                                          if (error_code) {
                                              spdlog::error("send error: {}", error_code.message());
                                          }
                                      });
            }
            log_udp_qos_result(outbound.endpoint, qos);
        });
    }

private:
    static Bytes<E2E_PUBLIC_KEY_BYTES> to_protocol_public_key(
        const session_crypto::E2EPublicKey& key) {
        Bytes<E2E_PUBLIC_KEY_BYTES> out{};
        std::memcpy(out.data(), key.data(), out.size());
        return out;
    }

    static session_crypto::E2EPublicKey to_crypto_public_key(
        const Bytes<E2E_PUBLIC_KEY_BYTES>& key) {
        session_crypto::E2EPublicKey out{};
        std::memcpy(out.data(), key.data(), out.size());
        return out;
    }

    void initialize_e2e_keypair() {
        e2e_keypair_ready_ =
            session_crypto::make_e2e_keypair(e2e_public_key_, e2e_secret_key_);
        if (!e2e_keypair_ready_) {
            spdlog::warn("Could not generate E2E keypair; room key handoff disabled");
            e2e_key_public_bytes_ = {};
            return;
        }
        e2e_key_public_bytes_ = to_protocol_public_key(e2e_public_key_);
        join_session_.set_key_public(e2e_key_public_bytes_);
    }

    void clear_access_request_state() {
        std::lock_guard<std::mutex> lock(access_request_mutex_);
        participant_key_public_.clear();
        pending_access_requests_.clear();
        pending_key_handoff_acks_.clear();
    }

    struct PendingChatMessage {
        Bytes<SECURE_PACKET_NONCE_BYTES> nonce{};
        std::string text;
        std::chrono::steady_clock::time_point sent_at{};
        bool history_requested = false;
    };

    static std::string nonce_key(const Bytes<SECURE_PACKET_NONCE_BYTES>& nonce) {
        return std::string(nonce.begin(), nonce.end());
    }

    void clear_chat_state() {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        chat_messages_.clear();
        pending_chat_messages_.clear();
        last_seen_chat_sequence_ = 0;
        chat_unread_count_ = 0;
        chat_history_truncated_ = false;
        chat_status_.clear();
        chat_retry_text_.clear();
    }

    bool chat_available_on_io_snapshot() const {
        return join_session_.is_join_confirmed() &&
               join_session_.participant_id() != 0 &&
               !join_session_.room_id().empty() &&
               !join_session_.room_instance_id().empty() &&
               join_session_.access_epoch() != 0 &&
               join_session_.has_media_secret();
    }

    uint64_t last_seen_chat_sequence_snapshot() const {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        return last_seen_chat_sequence_;
    }

    std::string chat_sender_name(uint32_t sender_id) const {
        if (sender_id == join_session_.participant_id()) {
            return "You";
        }
        for (const auto& participant: participant_manager_.get_all_info()) {
            if (participant.id == sender_id) {
                if (!participant.display_name.empty()) {
                    return participant.display_name;
                }
                if (!participant.profile_id.empty()) {
                    return participant.profile_id;
                }
            }
        }
        return "Participant " + std::to_string(sender_id);
    }

    std::optional<session_crypto::SessionKey> current_chat_key_on_io() const {
        if (!chat_available_on_io_snapshot()) {
            return std::nullopt;
        }
        return session_crypto::derive_chat_key_from_secret(
            join_session_.room_id(), join_session_.room_instance_id(),
            join_session_.media_secret());
    }

    bool send_chat_message_on_io(const std::string& text) {
        if (text.empty() || text.size() > ROOM_CHAT_PLAINTEXT_MAX_BYTES) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_status_ = text.empty() ? "Message is empty" : "Message is too long";
            return false;
        }
        const auto key = current_chat_key_on_io();
        if (!key.has_value()) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_status_ = "Chat unavailable until the room key is present";
            return false;
        }

        RoomChatSendHdr request{};
        request.magic = CTRL_MAGIC;
        request.type = CtrlHdr::Cmd::ROOM_CHAT_SEND;
        request.participant_id = join_session_.participant_id();
        request.request_id = chat_request_id_.fetch_add(1, std::memory_order_relaxed);
        request.access_epoch = join_session_.access_epoch();
        packet_builder::write_fixed(request.room_id, join_session_.room_id());
        packet_builder::write_fixed(request.room_instance_id, join_session_.room_instance_id());
        if (!session_crypto::make_chat_nonce(request.nonce)) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_status_ = "Could not create chat nonce";
            return false;
        }

        session_crypto::ChatMetadata metadata;
        metadata.room_id = join_session_.room_id();
        metadata.room_instance_id = join_session_.room_instance_id();
        metadata.sender_id = join_session_.participant_id();
        metadata.access_epoch = join_session_.access_epoch();
        metadata.nonce = request.nonce;

        size_t ciphertext_bytes = 0;
        if (!session_crypto::seal_chat_message(
                *key, metadata, reinterpret_cast<const unsigned char*>(text.data()),
                text.size(), reinterpret_cast<unsigned char*>(request.ciphertext.data()),
                request.ciphertext.size(), ciphertext_bytes) ||
            ciphertext_bytes == 0 ||
            ciphertext_bytes > std::numeric_limits<uint16_t>::max()) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_status_ = "Could not encrypt chat message";
            return false;
        }
        request.ciphertext_bytes = static_cast<uint16_t>(ciphertext_bytes);

        {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            pending_chat_messages_[nonce_key(request.nonce)] =
                PendingChatMessage{request.nonce, text, std::chrono::steady_clock::now(), false};
            chat_status_ = "Sending...";
            chat_retry_text_.clear();
        }

        auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(request));
        std::memcpy(packet->data(), &request, sizeof(request));
        send(packet->data(), packet->size(), packet);
        return true;
    }

    void request_chat_history_on_io(uint64_t after_sequence) {
        if (!chat_available_on_io_snapshot()) {
            return;
        }
        RoomChatHistoryRequestHdr request{};
        request.magic = CTRL_MAGIC;
        request.type = CtrlHdr::Cmd::ROOM_CHAT_HISTORY_REQUEST;
        request.request_id = chat_request_id_.fetch_add(1, std::memory_order_relaxed);
        request.access_epoch = join_session_.access_epoch();
        request.after_sequence = after_sequence;
        packet_builder::write_fixed(request.room_id, join_session_.room_id());
        packet_builder::write_fixed(request.room_instance_id, join_session_.room_instance_id());
        auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(request));
        std::memcpy(packet->data(), &request, sizeof(request));
        send(packet->data(), packet->size(), packet);
    }

    void accept_chat_message(const RoomChatEventHdr& event, const std::string& plaintext) {
        const bool local_sender = event.participant_id == join_session_.participant_id();
        bool request_history = false;
        uint64_t history_after = 0;
        {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            const auto duplicate = std::find_if(
                chat_messages_.begin(), chat_messages_.end(),
                [&](const ClientChatMessage& message) {
                    return message.sequence == event.chat_sequence;
                });
            if (duplicate != chat_messages_.end()) {
                pending_chat_messages_.erase(nonce_key(event.nonce));
                return;
            }
            if (event.chat_sequence > last_seen_chat_sequence_ + 1 &&
                last_seen_chat_sequence_ != 0) {
                request_history = true;
                history_after = last_seen_chat_sequence_;
            }
            last_seen_chat_sequence_ =
                std::max(last_seen_chat_sequence_, event.chat_sequence);

            ClientChatMessage message;
            message.sequence = event.chat_sequence;
            message.sender_id = event.participant_id;
            message.access_epoch = event.access_epoch;
            message.server_time_ms = event.server_time_ms;
            message.sender_name = chat_sender_name(event.participant_id);
            message.text = plaintext;
            message.local_sender = local_sender;
            chat_messages_.push_back(std::move(message));
            std::sort(chat_messages_.begin(), chat_messages_.end(),
                      [](const auto& left, const auto& right) {
                          return left.sequence < right.sequence;
                      });
            if (chat_messages_.size() > CLIENT_CHAT_RETAINED_MESSAGES) {
                chat_messages_.erase(chat_messages_.begin(),
                                     chat_messages_.begin() +
                                         static_cast<std::ptrdiff_t>(
                                             chat_messages_.size() -
                                             CLIENT_CHAT_RETAINED_MESSAGES));
            }
            pending_chat_messages_.erase(nonce_key(event.nonce));
            if (!chat_dialog_open_ && !local_sender) {
                ++chat_unread_count_;
            }
            chat_status_.clear();
        }
        if (request_history) {
            request_chat_history_on_io(history_after);
        }
    }

    void handle_chat_event_message(std::size_t bytes, const char* recv_data) {
        if (bytes < sizeof(RoomChatEventHdr)) {
            return;
        }
        RoomChatEventHdr event{};
        std::memcpy(&event, recv_data, sizeof(event));
        if (event.ciphertext_bytes == 0 ||
            event.ciphertext_bytes > ROOM_CHAT_CIPHERTEXT_MAX_BYTES ||
            fixed_string(event.room_id) != join_session_.room_id() ||
            fixed_string(event.room_instance_id) != join_session_.room_instance_id() ||
            event.access_epoch != join_session_.access_epoch()) {
            return;
        }
        const auto key = current_chat_key_on_io();
        if (!key.has_value()) {
            return;
        }
        session_crypto::ChatMetadata metadata;
        metadata.room_id = fixed_string(event.room_id);
        metadata.room_instance_id = fixed_string(event.room_instance_id);
        metadata.sender_id = event.participant_id;
        metadata.access_epoch = event.access_epoch;
        metadata.nonce = event.nonce;
        std::array<unsigned char, ROOM_CHAT_PLAINTEXT_MAX_BYTES> plaintext{};
        size_t plaintext_bytes = 0;
        if (!session_crypto::open_chat_message(
                *key, metadata,
                reinterpret_cast<const unsigned char*>(event.ciphertext.data()),
                event.ciphertext_bytes, plaintext.data(), plaintext.size(),
                plaintext_bytes)) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_status_ = "Could not decrypt a chat message";
            return;
        }
        accept_chat_message(
            event, std::string(reinterpret_cast<const char*>(plaintext.data()),
                               plaintext_bytes));
    }

    void handle_chat_rejected_message(std::size_t bytes, const char* recv_data) {
        if (bytes < sizeof(RoomChatSendRejectedHdr)) {
            return;
        }
        RoomChatSendRejectedHdr rejected{};
        std::memcpy(&rejected, recv_data, sizeof(rejected));
        std::lock_guard<std::mutex> lock(chat_mutex_);
        const auto it = pending_chat_messages_.find(nonce_key(rejected.nonce));
        if (it != pending_chat_messages_.end()) {
            chat_retry_text_ = it->second.text;
            pending_chat_messages_.erase(it);
        }
        const auto reason = fixed_string(rejected.reason);
        chat_status_ = reason.empty() ? "Message rejected" : reason;
    }

    void handle_chat_history_response_message(std::size_t bytes, const char* recv_data) {
        if (bytes < sizeof(RoomChatHistoryResponseHdr)) {
            return;
        }
        RoomChatHistoryResponseHdr response{};
        std::memcpy(&response, recv_data, sizeof(response));
        if (response.status != ROOM_STATUS_OK) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_status_ = "Chat history unavailable";
            return;
        }
        if ((response.flags & ROOM_CHAT_HISTORY_TRUNCATED) != 0) {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            chat_history_truncated_ = true;
            chat_status_ = "Earlier chat messages are no longer available.";
        }
        if ((response.flags & ROOM_CHAT_HISTORY_DONE) != 0) {
            confirm_stale_chat_pending_after_history();
            return;
        }

        RoomChatEventHdr event{};
        event.magic = CTRL_MAGIC;
        event.type = CtrlHdr::Cmd::ROOM_CHAT_EVENT;
        event.participant_id = response.participant_id;
        event.request_id = response.request_id;
        event.access_epoch = response.access_epoch;
        event.chat_sequence = response.chat_sequence;
        event.server_time_ms = response.server_time_ms;
        event.ciphertext_bytes = response.ciphertext_bytes;
        event.room_id = response.room_id;
        event.room_instance_id = response.room_instance_id;
        event.nonce = response.nonce;
        event.ciphertext = response.ciphertext;
        handle_chat_event_message(sizeof(event), reinterpret_cast<const char*>(&event));
    }

    void confirm_stale_chat_pending_after_history() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(chat_mutex_);
        for (auto it = pending_chat_messages_.begin(); it != pending_chat_messages_.end();) {
            if (it->second.history_requested &&
                now - it->second.sent_at > std::chrono::seconds(2)) {
                if (chat_retry_text_.empty()) {
                    chat_retry_text_ = it->second.text;
                }
                chat_status_ = "Message not confirmed";
                it = pending_chat_messages_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void remove_pending_access_request(uint32_t participant_id) {
        std::lock_guard<std::mutex> lock(access_request_mutex_);
        pending_access_requests_.erase(participant_id);
    }

    void remember_participant_key(uint32_t participant_id,
                                  const std::string& profile_id,
                                  const std::string& display_name,
                                  Bytes<E2E_PUBLIC_KEY_BYTES> key_public,
                                  bool has_key_public) {
        if (participant_id == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(access_request_mutex_);
        if (has_key_public) {
            participant_key_public_[participant_id] = key_public;
        } else {
            participant_key_public_.erase(participant_id);
        }

        if (participant_id == join_session_.participant_id()) {
            pending_access_requests_.erase(participant_id);
            return;
        }

        if (join_session_.access_mode() == ROOM_ACCESS_APPROVE &&
            join_session_.has_media_secret()) {
            ParticipantInfo info{};
            info.id = participant_id;
            info.profile_id = profile_id;
            info.display_name = display_name.empty() ? "Player" : display_name;
            info.key_public = key_public;
            info.has_key_public = has_key_public;
            pending_access_requests_[participant_id] = info;
        }
    }

    bool send_media_key_envelope_to_participant_on_io(uint32_t participant_id) {
        if (!e2e_keypair_ready_ || participant_id == 0 ||
            participant_id == join_session_.participant_id() ||
            !join_session_.has_media_secret() ||
            join_session_.participant_id() == 0 ||
            !outbound_enabled_.load(std::memory_order_acquire)) {
            return false;
        }

        Bytes<E2E_PUBLIC_KEY_BYTES> target_public_key{};
        {
            std::lock_guard<std::mutex> lock(access_request_mutex_);
            const auto it = participant_key_public_.find(participant_id);
            if (it == participant_key_public_.end()) {
                return false;
            }
            target_public_key = it->second;
        }

        const std::string& media_secret = join_session_.media_secret();
        if (media_secret.empty() || media_secret.size() > MEDIA_SECRET_MAX_BYTES) {
            return false;
        }

        E2EKeyEnvelopePayload payload{};
        payload.access_epoch = join_session_.access_epoch();
        payload.media_secret_bytes =
            static_cast<uint16_t>(std::min(media_secret.size(),
                                           payload.media_secret.size()));
        if (payload.access_epoch == 0 ||
            payload.media_secret_bytes != media_secret.size()) {
            return false;
        }
        std::copy_n(media_secret.begin(), media_secret.size(),
                    payload.media_secret.begin());

        E2EKeyEnvelopeHdr envelope{};
        envelope.magic = E2E_KEY_ENVELOPE_MAGIC;
        envelope.sender_id = join_session_.participant_id();
        envelope.target_id = participant_id;
        envelope.access_epoch = join_session_.access_epoch();
        std::copy(join_session_.media_key_commitment().begin(),
                  join_session_.media_key_commitment().end(),
                  envelope.media_key_commitment.begin());

        size_t encrypted_bytes = 0;
        const auto crypto_public_key = to_crypto_public_key(target_public_key);
        if (!session_crypto::seal_key_envelope(
                crypto_public_key,
                reinterpret_cast<const unsigned char*>(&payload), sizeof(payload),
                reinterpret_cast<unsigned char*>(envelope.encrypted.data()),
                envelope.encrypted.size(), encrypted_bytes) ||
            encrypted_bytes == 0 ||
            encrypted_bytes > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        envelope.encrypted_bytes = static_cast<uint16_t>(encrypted_bytes);

        auto packet = std::make_shared<std::vector<unsigned char>>(
            sizeof(E2EKeyEnvelopeHdr));
        std::memcpy(packet->data(), &envelope, sizeof(envelope));
        send(packet->data(), packet->size(), packet);
        spdlog::info("Sent encrypted room key to participant {}", participant_id);
        return true;
    }

    bool queue_media_key_handoff_on_io(uint32_t participant_id) {
        {
            std::lock_guard<std::mutex> lock(access_request_mutex_);
            pending_key_handoff_acks_.insert(participant_id);
        }
        if (send_media_key_envelope_to_participant_on_io(participant_id)) {
            return true;
        }
        std::lock_guard<std::mutex> lock(access_request_mutex_);
        pending_key_handoff_acks_.erase(participant_id);
        return false;
    }

    void retry_pending_key_handoffs_on_io() {
        std::vector<uint32_t> participant_ids;
        {
            std::lock_guard<std::mutex> lock(access_request_mutex_);
            participant_ids.assign(pending_key_handoff_acks_.begin(),
                                   pending_key_handoff_acks_.end());
        }
        for (uint32_t participant_id: participant_ids) {
            (void)send_media_key_envelope_to_participant_on_io(participant_id);
        }
    }

    void send_key_handoff_ack_on_io(uint32_t target_id, uint32_t access_epoch,
                                    const std::string& media_key_commitment) {
        if (target_id == 0 || target_id == join_session_.participant_id() ||
            access_epoch == 0 ||
            !performer_join_token::valid_sha256_hex(media_key_commitment) ||
            join_session_.participant_id() == 0 ||
            !outbound_enabled_.load(std::memory_order_acquire)) {
            return;
        }
        RoomKeyHandoffAckHdr ack{};
        ack.magic = CTRL_MAGIC;
        ack.type = CtrlHdr::Cmd::ROOM_KEY_HANDOFF_ACK;
        ack.participant_id = join_session_.participant_id();
        ack.target_id = target_id;
        ack.access_epoch = access_epoch;
        std::copy(media_key_commitment.begin(), media_key_commitment.end(),
                  ack.media_key_commitment.begin());
        auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(ack));
        std::memcpy(packet->data(), &ack, sizeof(ack));
        send(packet->data(), packet->size(), packet);
    }

    void maybe_auto_share_media_key(uint32_t participant_id) {
        if (join_session_.access_mode() == ROOM_ACCESS_APPROVE) {
            return;
        }
        if (queue_media_key_handoff_on_io(participant_id)) {
            spdlog::info("Auto-shared encrypted room key with participant {}",
                         participant_id);
        }
    }

    std::shared_ptr<const session_crypto::SessionKey> current_session_key() const {
        return std::atomic_load_explicit(&session_key_, std::memory_order_acquire);
    }

    bool accept_secure_control_sequence(uint32_t sender_id, uint32_t sequence) {
        if (sender_id == 0 || sequence == 0) {
            return false;
        }
        return secure_control_replay_windows_[sender_id].accept(sequence);
    }

    bool apply_media_secret_rotation(std::string media_secret,
                                     uint32_t access_epoch,
                                     const std::string& media_key_commitment) {
        if (access_epoch == 0 ||
            !session_crypto::media_secret_matches_commitment(
                media_secret, media_key_commitment) ||
            access_epoch < join_session_.access_epoch() ||
            (access_epoch == join_session_.access_epoch() &&
             join_session_.has_media_secret()) ||
            (access_epoch == join_session_.access_epoch() &&
             media_key_commitment != join_session_.media_key_commitment())) {
            return false;
        }
        join_session_.set_media_secret(std::move(media_secret));
        join_session_.set_access_epoch(access_epoch);
        join_session_.set_media_key_commitment(media_key_commitment);
        clear_chat_state();
        update_media_key_from_secret();
        return true;
    }

    bool rotate_media_key_on_io_context(const std::string& media_secret,
                                        uint32_t access_epoch) {
        const auto key = current_session_key();
        if (media_secret.empty() || key == nullptr ||
            !join_session_.server_supports(AUDIO_CAP_SECURE_AUDIO) ||
            join_session_.participant_id() == 0 ||
            access_epoch == 0 ||
            !outbound_enabled_.load(std::memory_order_acquire)) {
            return false;
        }
        const auto media_key_commitment =
            session_crypto::media_key_commitment(media_secret);
        if (!media_key_commitment.has_value()) {
            return false;
        }

        MediaKeyRotationPayload payload{};
        payload.command = SECURE_CONTROL_ROTATE_MEDIA_KEY;
        payload.access_epoch = access_epoch;
        payload.media_secret_bytes = static_cast<uint16_t>(
            std::min(media_secret.size(), payload.media_secret.size()));
        if (payload.media_secret_bytes == 0 ||
            payload.media_secret_bytes != media_secret.size()) {
            return false;
        }
        std::copy_n(media_secret.begin(), media_secret.size(),
                    payload.media_secret.begin());

        session_crypto::SecureControlMetadata metadata;
        metadata.sender_id = join_session_.participant_id();
        metadata.sequence =
            secure_control_sequence_.fetch_add(1, std::memory_order_acq_rel);
        metadata.access_epoch = access_epoch;
        metadata.plaintext_bytes = sizeof(payload);
        metadata.media_key_commitment = *media_key_commitment;

        std::array<unsigned char, 512> sealed{};
        size_t sealed_bytes = 0;
        if (!session_crypto::seal_control_packet(
                *key, metadata,
                reinterpret_cast<const unsigned char*>(&payload), sizeof(payload),
                sealed.data(), sealed.size(), sealed_bytes)) {
            return false;
        }

        auto packet = std::make_shared<std::vector<unsigned char>>(
            sealed.begin(), sealed.begin() + static_cast<std::ptrdiff_t>(sealed_bytes));
        send(packet->data(), packet->size(), packet);
        (void)apply_media_secret_rotation(media_secret, access_epoch,
                                          *media_key_commitment);
        std::vector<uint32_t> participant_ids;
        {
            std::lock_guard<std::mutex> lock(access_request_mutex_);
            participant_ids.reserve(participant_key_public_.size());
            for (const auto& [participant_id, _]: participant_key_public_) {
                if (participant_id != join_session_.participant_id()) {
                    participant_ids.push_back(participant_id);
                }
            }
        }
        for (uint32_t participant_id: participant_ids) {
            (void)queue_media_key_handoff_on_io(participant_id);
        }
        spdlog::info("Rotated local E2E media key and sent encrypted room rotation");
        return true;
    }

    bool should_secure_audio_packet(const unsigned char* data, std::size_t len) const {
        if (!join_session_.server_supports(AUDIO_CAP_SECURE_AUDIO) ||
            join_session_.participant_id() == 0 ||
            data == nullptr || len < sizeof(MsgHdr) ||
            len > std::numeric_limits<uint16_t>::max() - SECURE_PACKET_TAG_BYTES) {
            return false;
        }

        MsgHdr hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        return hdr.magic == AUDIO_V3_MAGIC || hdr.magic == AUDIO_REDUNDANT_MAGIC;
    }

    bool secure_audio_metadata_for_packet(const unsigned char* data, std::size_t len,
                                          session_crypto::SecureAudioMetadata& metadata) const {
        metadata = {};
        if (!should_secure_audio_packet(data, len)) {
            return false;
        }

        const auto use_child_header = [&](const unsigned char* child, size_t child_len) {
            const auto parsed = audio_packet::parse_audio_header(child, child_len);
            if (!parsed.valid) {
                return false;
            }
            metadata.sender_id = join_session_.participant_id();
            metadata.sequence = parsed.sequence;
            metadata.sample_rate = parsed.sample_rate;
            metadata.frame_count = parsed.frame_count;
            metadata.plaintext_bytes = static_cast<uint16_t>(len);
            metadata.channels = parsed.channels;
            metadata.codec = parsed.codec;
            metadata.capture_server_time_ns = parsed.capture_server_time_ns;
            return true;
        };

        MsgHdr hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        if (hdr.magic == AUDIO_V3_MAGIC) {
            return use_child_header(data, len);
        }

        if (hdr.magic != AUDIO_REDUNDANT_MAGIC) {
            return false;
        }

        bool ok = false;
        std::string reason;
        audio_packet::for_each_redundant_audio_child(
            data, len,
            [&](const unsigned char* child, size_t child_len, uint8_t index) {
                if (index == 0) {
                    ok = use_child_header(child, child_len);
                }
            },
            &reason);
        return ok;
    }

    void send_audio_packet_sync(const unsigned char* data, std::size_t len) {
        if (data == nullptr || !validate_outbound_audio_packet(
                                   const_cast<unsigned char*>(data), len)) {
            return;
        }
        if (!outbound_enabled_.load(std::memory_order_acquire)) {
            return;
        }

        const auto outbound = current_outbound_endpoint_snapshot();
        const unsigned char* send_data = data;
        std::size_t send_len = len;
        std::array<unsigned char, 2048> secure_packet{};
        size_t secure_bytes = 0;
        session_crypto::SecureAudioMetadata secure_metadata;
        const auto key = current_session_key();
        if (key != nullptr &&
            secure_audio_metadata_for_packet(data, len, secure_metadata)) {
            if (!session_crypto::seal_audio_packet(
                    *key, secure_metadata, data, len, secure_packet.data(),
                    secure_packet.size(), secure_bytes)) {
                outbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            send_data = secure_packet.data();
            send_len = secure_bytes;
        }

        std::error_code error_code;
        udp_network::QosResult qos;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!outbound_enabled_.load(std::memory_order_acquire) ||
                outbound.generation !=
                    outbound_generation_.load(std::memory_order_acquire)) {
                return;
            }
            qos = socket_qos_.ensure_flow(socket_, outbound.endpoint);
            socket_.send_to(asio::buffer(send_data, send_len), outbound.endpoint, 0,
                            error_code);
        }
        log_udp_qos_result(outbound.endpoint, qos);

        if (!error_code) {
            total_bytes_tx_.fetch_add(send_len, std::memory_order_relaxed);
            return;
        }
        if (error_code == asio::error::would_block ||
            error_code == asio::error::try_again) {
            tx_socket_would_block_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (error_code != asio::error::operation_aborted &&
            outbound_enabled_.load(std::memory_order_acquire)) {
            spdlog::error("audio send error: {}", error_code.message());
        }
    }

    static void configure_udp_socket(udp::socket& socket) {
        std::error_code buffer_error;
        udp_network::configure_low_latency_buffers(socket, buffer_error);
        if (!buffer_error) {
            spdlog::info("UDP socket buffers optimized for low latency ({} bytes)",
                      UDP_SOCKET_BUFFER_BYTES);
        } else {
            spdlog::warn("Failed to set socket buffer sizes: {}", buffer_error.message());
        }

        std::error_code non_blocking_error;
        socket.non_blocking(true, non_blocking_error);
        if (non_blocking_error) {
            throw std::runtime_error("Failed to make UDP socket non-blocking: " +
                                     non_blocking_error.message());
        }
    }

    static void log_udp_qos_result(const udp::endpoint& endpoint,
                                   const udp_network::QosResult& result) {
        if (!result.newly_configured) {
            return;
        }
        const auto address = udp_network::format_address_for_display(endpoint.address());
        if (!result.ok() || result.detail.find("failed") != std::string::npos) {
            spdlog::warn("UDP QoS not fully active for {}:{}: {}", address, endpoint.port(),
                      result.detail);
        } else {
            spdlog::info("UDP QoS active for {}:{}: {}", address, endpoint.port(),
                      result.detail);
        }
    }

    void configure_udp_socket_locked() {
        configure_udp_socket(socket_);
    }

    static std::optional<udp::endpoint> choose_client_endpoint(
        const udp::resolver::results_type& endpoints) {
        std::optional<udp::endpoint> first_v6;
        for (const auto& entry : endpoints) {
            const auto endpoint = entry.endpoint();
            if (endpoint.address().is_v4()) {
                return endpoint;
            }
            if (!first_v6.has_value() && endpoint.address().is_v6()) {
                first_v6 = endpoint;
            }
        }
        return first_v6;
    }

    void reopen_socket_for_endpoint_locked(const udp::endpoint& endpoint) {
        std::error_code ec;
        const bool protocol_matches =
            socket_.is_open() && socket_.local_endpoint(ec).protocol() == endpoint.protocol();
        if (!ec && protocol_matches) {
            return;
        }

        socket_qos_.reset();
        std::error_code ignored;
        socket_.close(ignored);
        socket_.open(endpoint.protocol(), ec);
        if (!ec) {
            socket_.bind(udp::endpoint(endpoint.protocol(), 0), ec);
        }
        if (ec) {
            throw std::runtime_error("Failed to bind UDP socket: " + ec.message());
        }
        configure_udp_socket_locked();
        spdlog::info("Client local port: {} ({})", socket_.local_endpoint().port(),
                     endpoint.protocol() == udp::v6() ? "IPv6" : "IPv4");
    }

    void clear_audio_path_queues() {
        // Precondition: the audio stream must be stopped. This resets decoder
        // state that the audio callback mutates without locks; running
        // it concurrently with the callback is undefined behavior.
        assert(!audio_.is_stream_active());
        OpusSendFrame discarded_opus;
        while (opus_send_queue_.try_dequeue(discarded_opus)) {
        }
        opus_send_queue_age_window_.clear();
        opus_send_queue_age_p99_ns_.store(0, std::memory_order_relaxed);
        opus_tx_accumulator_.fill(0.0F);
        opus_tx_accumulated_frames_ = 0;
        opus_tx_accumulator_capture_time_ = {};
        opus_tx_accumulator_reset_requested_.store(true, std::memory_order_release);
        request_recent_opus_audio_packets_reset();

        participant_manager_.for_each([](uint32_t, ParticipantData& participant) {
            participant.opus_queue.clear();
            participant.opus_pcm_buffered_frames = 0;
            clear_opus_capture_chunks(participant);
            participant.opus_pcm_buffered_frames_observed.store(
                0, std::memory_order_relaxed);
            participant.buffer_ready.store(false, std::memory_order_relaxed);
            participant.opus_consecutive_empty_callbacks.store(
                0, std::memory_order_relaxed);
            participant.queue_depth_avg.store(0, std::memory_order_relaxed);
            participant.queue_depth_max.store(0, std::memory_order_relaxed);
            participant.queue_depth_drift_milli.store(0, std::memory_order_relaxed);
            participant.queue_size_history.fill(0);
            participant.history_index = 0;
            if (participant.decoder) {
                participant.decoder->reset();
            }
        });
    }

    void request_udp_path_rebind(std::string reason) {
        if (!join_session_.is_join_confirmed()) {
            return;
        }

        const int64_t now_ns = steady_now_ns();
        int64_t       next_allowed =
            next_udp_path_rebind_allowed_ns_.load(std::memory_order_acquire);
        if (now_ns < next_allowed) {
            return;
        }

        const int64_t cooldown_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                client_network_path::UDP_REBIND_COOLDOWN)
                .count();
        while (!next_udp_path_rebind_allowed_ns_.compare_exchange_weak(
            next_allowed, now_ns + cooldown_ns, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
            if (now_ns < next_allowed) {
                return;
            }
        }

        if (udp_path_rebind_pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        asio::post(io_context_, [this, reason = std::move(reason)]() mutable {
            udp_path_rebind_pending_.store(false, std::memory_order_release);
            rebind_udp_socket_and_rejoin(reason);
        });
    }

    void rebind_udp_socket_and_rejoin(const std::string& reason) {
        const auto target = current_server_endpoint();
        if (target.port() == 0 || !outbound_enabled_.load(std::memory_order_acquire)) {
            return;
        }

        std::error_code ec;
        std::error_code ignored;
        udp::socket     replacement(io_context_);
        udp_network::open_compatible_socket(replacement, target, 0, ec);
        if (ec) {
            spdlog::error("UDP path rebind failed after '{}': {}", reason, ec.message());
            return;
        }
        configure_udp_socket(replacement);
        const uint16_t new_port = replacement.local_endpoint(ignored).port();

        receiving_enabled_.store(false, std::memory_order_release);
        outbound_enabled_.store(false, std::memory_order_release);
        receive_generation_.fetch_add(1, std::memory_order_acq_rel);
        outbound_generation_.fetch_add(1, std::memory_order_acq_rel);

        uint16_t old_port = 0;
        udp_network::QosResult qos;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);

            old_port = socket_.local_endpoint(ignored).port();

            CtrlHdr leave{};
            leave.magic = CTRL_MAGIC;
            leave.type  = CtrlHdr::Cmd::LEAVE;
            socket_.send_to(asio::buffer(&leave, sizeof(leave)), target, 0, ignored);

            socket_.cancel(ignored);
            socket_.close(ignored);
            socket_ = std::move(replacement);
            socket_qos_.reset();
            qos = socket_qos_.ensure_flow(socket_, target);
        }
        log_udp_qos_result(target, qos);

        join_session_.reset();
        reset_server_clock_and_ping_state();
        audio_path_interval_received_.store(0, std::memory_order_relaxed);
        audio_path_interval_sequence_gaps_.store(0, std::memory_order_relaxed);
        audio_path_interval_gaps_.store(0, std::memory_order_relaxed);
        request_recent_opus_audio_packets_reset();

        receive_generation_.fetch_add(1, std::memory_order_acq_rel);
        outbound_generation_.fetch_add(1, std::memory_order_acq_rel);
        receiving_enabled_.store(true, std::memory_order_release);
        outbound_enabled_.store(true, std::memory_order_release);

        const uint32_t rebind_count =
            udp_path_rebind_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        spdlog::warn(
            "UDP path rebind #{} after '{}': local port {} -> {}; rejoining room '{}'",
            rebind_count, reason, old_port, new_port, join_session_.room_id());

        start_receive_slots();
        send_join();
    }

    udp::endpoint current_server_endpoint() const {
        std::lock_guard<std::mutex> lock(server_endpoint_mutex_);
        return server_endpoint_;
    }

    bool validate_outbound_audio_packet(void* data, std::size_t len) {
        if (data == nullptr || len < sizeof(MsgHdr)) {
            return true;
        }

        MsgHdr hdr{};
        std::memcpy(&hdr, data, sizeof(MsgHdr));
        if (hdr.magic != AUDIO_V3_MAGIC) {
            return true;
        }

        std::string reason;
        const auto* packet_bytes = reinterpret_cast<const unsigned char*>(data);
        if (audio_packet::validate_audio_packet_bytes(packet_bytes, len, &reason)) {
            return true;
        }

        outbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);

        uint32_t sequence = 0;
        uint16_t payload_bytes = 0;
        AudioCodec codec = AudioCodec::Opus;
        const auto parsed = audio_packet::parse_audio_header(packet_bytes, len);
        if (parsed.valid) {
            sequence = parsed.sequence;
            payload_bytes = parsed.payload_bytes;
            codec = parsed.codec;
        }

        spdlog::error(
            "BUG: refusing malformed outbound versioned audio: reason={} len={} magic=0x{:08x} "
            "header_size={} payload_bytes={} codec={} seq={}",
            reason, len, hdr.magic, parsed.header_size, payload_bytes, static_cast<int>(codec),
            sequence);
        return false;
    }

    template <size_t N>
    static std::string fixed_string(const Bytes<N>& bytes) {
        const auto end = std::find(bytes.begin(), bytes.end(), '\0');
        return std::string(bytes.begin(), end);
    }

    struct OpusSendFrame {
        std::array<float, 960> samples{};
        uint16_t frame_count = 0;
        uint32_t sample_rate = 48000;
        std::chrono::steady_clock::time_point capture_time;
    };

    static constexpr size_t TX_PACKET_BUFFER_BYTES = AUDIO_REDUNDANT_TARGET_BYTES;
    static constexpr size_t TX_PACKET_POOL_SIZE = 8;
    static constexpr size_t RECENT_OPUS_PACKET_SLOTS =
        static_cast<size_t>(MAX_AUDIO_REDUNDANT_PACKETS) - 1;

    struct TxPacketBuffer {
        std::array<unsigned char, TX_PACKET_BUFFER_BYTES> bytes{};
        size_t size = 0;

        unsigned char* data() {
            return bytes.data();
        }

        const unsigned char* data() const {
            return bytes.data();
        }

        size_t capacity() const {
            return bytes.size();
        }

        audio_packet::AudioPacketView view() const {
            return audio_packet::AudioPacketView{bytes.data(), size};
        }
    };

    class TxPacketBufferPool {
    public:
        TxPacketBufferPool() {
            for (size_t i = 0; i < free_indices_.size(); ++i) {
                free_indices_[i] = free_indices_.size() - 1 - i;
            }
            free_count_ = free_indices_.size();
        }

        TxPacketBuffer* acquire() {
            if (free_count_ == 0) {
                return nullptr;
            }
            TxPacketBuffer& buffer = buffers_[free_indices_[--free_count_]];
            buffer.size = 0;
            return &buffer;
        }

        void release(TxPacketBuffer* buffer) {
            if (buffer == nullptr) {
                return;
            }
            const auto index = static_cast<size_t>(buffer - buffers_.data());
            if (index >= buffers_.size() || free_count_ >= free_indices_.size()) {
                return;
            }
            buffer->size = 0;
            free_indices_[free_count_++] = index;
        }

    private:
        std::array<TxPacketBuffer, TX_PACKET_POOL_SIZE> buffers_{};
        std::array<size_t, TX_PACKET_POOL_SIZE> free_indices_{};
        size_t free_count_ = 0;
    };

    class TxPacketLease {
    public:
        explicit TxPacketLease(TxPacketBufferPool& pool)
            : pool_(&pool),
              buffer_(pool.acquire()) {
        }

        ~TxPacketLease() {
            if (pool_ != nullptr) {
                pool_->release(buffer_);
            }
        }

        TxPacketLease(const TxPacketLease&) = delete;
        TxPacketLease& operator=(const TxPacketLease&) = delete;

        TxPacketBuffer* get() const {
            return buffer_;
        }

        TxPacketBuffer& operator*() const {
            return *buffer_;
        }

        TxPacketBuffer* operator->() const {
            return buffer_;
        }

    private:
        TxPacketBufferPool* pool_ = nullptr;
        TxPacketBuffer* buffer_ = nullptr;
    };

    static int64_t steady_time_ns(std::chrono::steady_clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch())
            .count();
    }

    int64_t server_time_for_steady_time_ns(std::chrono::steady_clock::time_point time) const {
        return steady_time_ns(time) +
               server_clock_offset_ns_.load(std::memory_order_acquire);
    }

    std::optional<int64_t> server_time_for_steady_time_ns_if_ready(
        std::chrono::steady_clock::time_point time) const {
        if (!server_clock_ready_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const int64_t offset_ns = server_clock_offset_ns_.load(std::memory_order_acquire);
        if (!server_clock_ready_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return steady_time_ns(time) + offset_ns;
    }

    bool build_audio_packet_into(TxPacketBuffer& out, uint32_t sequence, uint32_t sample_rate,
                                 uint16_t frame_count, uint8_t channels,
                                 const unsigned char* payload,
                                 uint16_t payload_bytes,
                                 std::chrono::steady_clock::time_point capture_time) const {
        const auto capture_server_time_ns = server_time_for_steady_time_ns_if_ready(capture_time);
        return audio_packet::write_audio_packet_v3(
            sequence, sample_rate, frame_count, channels, payload, payload_bytes,
            capture_server_time_ns.value_or(0), out.data(), out.capacity(), out.size);
    }

    static int64_t steady_now_ns() {
        return steady_time_ns(std::chrono::steady_clock::now());
    }

    uint32_t next_metronome_boundary_beat() const {
        return metronome_.next_boundary_beat();
    }

    void send_metronome_sync(int bpm_milli, bool running, uint32_t beat_number) {
        MetronomeSyncHdr sync{};
        sync.magic = CTRL_MAGIC;
        sync.type = CtrlHdr::Cmd::METRONOME_SYNC;
        sync.bpm_milli = static_cast<uint32_t>(std::max(1, bpm_milli));
        sync.beat_number = beat_number;
        sync.flags = running ? METRONOME_FLAG_RUNNING : 0;
        sync.sender_time_ns = steady_now_ns();

        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(MetronomeSyncHdr));
        std::memcpy(buf->data(), &sync, sizeof(MetronomeSyncHdr));
        send(buf->data(), buf->size(), buf);
        metronome_.mark_sync_sent();
    }

    void commit_metronome_bpm_milli(int bpm_milli) {
        if (!metronome_.should_send_bpm_milli(bpm_milli)) {
            return;
        }
        send_metronome_sync(bpm_milli, metronome_.running(), next_metronome_boundary_beat());
    }

    void start_audio_sender_thread() {
        if (audio_sender_running_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        audio_sender_thread_ = std::thread([this]() { audio_sender_loop(); });
    }

    void stop_audio_sender_thread() {
        audio_sender_running_.store(false, std::memory_order_release);
        audio_sender_wake_.store(true, std::memory_order_release);
        audio_sender_cv_.notify_one();
        if (audio_sender_thread_.joinable()) {
            audio_sender_thread_.join();
        }

        OpusSendFrame discarded_opus;
        while (opus_send_queue_.try_dequeue(discarded_opus)) {
        }
    }

    void request_recent_opus_audio_packets_reset() {
        recent_opus_audio_packets_reset_requested_.store(true, std::memory_order_release);
        wake_audio_sender_thread();
    }

    bool consume_recent_opus_audio_packets_reset_request_on_sender_thread() {
        if (!recent_opus_audio_packets_reset_requested_.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
        recent_opus_audio_packet_count_ = 0;
        return true;
    }

    class ScopedSenderThreadPriority {
    public:
        ScopedSenderThreadPriority() {
#ifdef _WIN32
            DWORD task_index = 0;
            handle_ = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
            if (handle_ != nullptr) {
                AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
            } else {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            }
#elif defined(__APPLE__)
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        }

        ~ScopedSenderThreadPriority() {
#ifdef _WIN32
            if (handle_ != nullptr) {
                AvRevertMmThreadCharacteristics(handle_);
            }
#endif
        }

        ScopedSenderThreadPriority(const ScopedSenderThreadPriority&) = delete;
        ScopedSenderThreadPriority& operator=(const ScopedSenderThreadPriority&) = delete;

    private:
#ifdef _WIN32
        HANDLE handle_ = nullptr;
#endif
    };

    void audio_sender_loop() {
        ScopedSenderThreadPriority sender_priority;
        TxPacketBufferPool packet_pool;
        std::array<unsigned char, AUDIO_BUF_SIZE> encoded_data{};
        int applied_bitrate = audio_encoder_.get_actual_bitrate();

        while (audio_sender_running_.load(std::memory_order_acquire)) {
            consume_recent_opus_audio_packets_reset_request_on_sender_thread();

            OpusSendFrame opus_frame;
            if (opus_send_queue_.try_dequeue(opus_frame)) {
                if (!join_session_.can_send_audio()) {
                    continue;
                }
                const int desired_bitrate =
                    congestion_target_bitrate_.load(std::memory_order_acquire);
                if (desired_bitrate != applied_bitrate &&
                    audio_encoder_.set_bitrate(desired_bitrate)) {
                    applied_bitrate = desired_bitrate;
                }
                observe_opus_send_queue_age(opus_frame.capture_time);
                uint16_t encoded_bytes = 0;
                const auto encode_start = std::chrono::steady_clock::now();
                if (audio_encoder_.encode(opus_frame.samples.data(), opus_frame.frame_count,
                                          encoded_data.data(), encoded_data.size(),
                                          encoded_bytes) &&
                    encoded_bytes <= AUDIO_BUF_SIZE) {
                    observe_tx_encode_time(std::chrono::steady_clock::now() - encode_start);
                    TxPacketLease packet(packet_pool);
                    TxPacketLease redundant_packet(packet_pool);
                    if (packet.get() == nullptr || redundant_packet.get() == nullptr) {
                        opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    const uint32_t seq =
                        audio_tx_sequence_.fetch_add(1, std::memory_order_relaxed);
                    audio_packets_generated_.fetch_add(1, std::memory_order_release);
                    if (!build_audio_packet_into(
                            *packet, seq, opus_frame.sample_rate,
                            opus_frame.frame_count, 1, encoded_data.data(), encoded_bytes,
                            opus_frame.capture_time)) {
                        opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (current_session_key() != nullptr &&
                        join_session_.server_supports(AUDIO_CAP_SECURE_AUDIO)) {
                        const uint32_t participant_id = join_session_.participant_id();
                        if (participant_id == 0) {
                            opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        packet_builder::embed_sender_id(packet->data(), participant_id);
                    }

                    consume_recent_opus_audio_packets_reset_request_on_sender_thread();
                    TxPacketBuffer* send_packet =
                        maybe_wrap_opus_packet_with_redundancy(*packet, *redundant_packet);
                    if (send_packet == nullptr) {
                        send_packet = packet.get();
                    }
                    observe_audio_packet_send_pacing();
                    send_audio_packet_sync(send_packet->data(), send_packet->size);
                    remember_recent_opus_audio_packet(*packet);
                } else {
                    observe_tx_encode_time(std::chrono::steady_clock::now() - encode_start);
                }
                continue;
            }

            std::unique_lock<std::mutex> lock(audio_sender_wait_mutex_);
            audio_sender_cv_.wait_for(lock, 1ms, [this]() {
                return !audio_sender_running_.load(std::memory_order_acquire) ||
                       audio_sender_wake_.exchange(false, std::memory_order_acq_rel);
            });
        }
    }

    TxPacketBuffer* maybe_wrap_opus_packet_with_redundancy(
        const TxPacketBuffer& packet, TxPacketBuffer& redundant_out) {
        if (!join_session_.server_supports(AUDIO_CAP_REDUNDANCY)) {
            recent_opus_audio_packet_count_ = 0;
            return nullptr;
        }

        const auto parsed = audio_packet::parse_audio_header(packet.data(), packet.size);
        if (!parsed.valid) {
            return nullptr;
        }

        const int adaptive_depth =
            adaptive_redundancy_depth_.load(std::memory_order_acquire);
        const int configured_depth = adaptive_depth >= 0
            ? adaptive_depth
            : get_opus_redundancy_depth_setting();
        const int effective_depth =
            effective_opus_redundancy_depth(configured_depth, parsed.frame_count);
        if (effective_depth <= 0) {
            recent_opus_audio_packet_count_ = 0;
            return nullptr;
        }

        const size_t child_count = opus_redundancy_child_count_for_policy(
            configured_depth, parsed.frame_count, recent_opus_audio_packet_count_);

        std::array<audio_packet::AudioPacketView, MAX_AUDIO_REDUNDANT_PACKETS> views{};
        size_t view_count = 0;
        views[view_count++] = packet.view();
        for (size_t i = 0;
             i < recent_opus_audio_packet_count_ && view_count < child_count; ++i) {
            views[view_count++] = recent_opus_audio_packets_[i].view();
        }
        if (view_count <= 1) {
            return nullptr;
        }

        size_t bytes_written = 0;
        if (!audio_packet::write_redundant_audio_packet(
                views.data(), view_count, redundant_out.data(), redundant_out.capacity(),
                AUDIO_REDUNDANT_TARGET_BYTES, bytes_written)) {
            return nullptr;
        }
        redundant_out.size = bytes_written;
        return &redundant_out;
    }

    void remember_recent_opus_audio_packet(const TxPacketBuffer& packet) {
        if (packet.size == 0) {
            return;
        }
        const size_t limit = recent_opus_audio_packets_.size();
        if (limit == 0) {
            return;
        }
        const size_t move_count = std::min(recent_opus_audio_packet_count_, limit - 1);
        for (size_t i = move_count; i > 0; --i) {
            recent_opus_audio_packets_[i] = recent_opus_audio_packets_[i - 1];
        }
        recent_opus_audio_packets_[0] = packet;
        recent_opus_audio_packet_count_ =
            std::min(recent_opus_audio_packet_count_ + 1, limit);
    }

    static size_t max_send_queue_frames(uint16_t frame_count) {
        if (frame_count <= 128) {
            return 8;
        }
        return 3;
    }

    void enqueue_opus_send_frame(const float* samples, uint16_t frame_count, uint32_t sample_rate,
                                 std::chrono::steady_clock::time_point capture_time) {
        if (!OpusEncoderWrapper::is_legal_frame_size(static_cast<int>(sample_rate), frame_count)) {
            opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const size_t max_queue_frames = max_send_queue_frames(frame_count);
        while (opus_send_queue_.size_approx() >= max_queue_frames) {
            OpusSendFrame discarded;
            if (!opus_send_queue_.try_dequeue(discarded)) {
                break;
            }
            opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
        }

        OpusSendFrame frame;
        std::copy_n(samples, frame_count, frame.samples.begin());
        frame.frame_count = frame_count;
        frame.sample_rate = sample_rate;
        frame.capture_time = capture_time;
        if (!opus_send_queue_.try_enqueue(frame)) {
            opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        wake_audio_sender_thread();
    }

    static uint16_t preferred_opus_tx_frame_count(uint32_t sample_rate,
                                                  uint16_t requested_frame_count) {
        if (sample_rate == opus_network_clock::SAMPLE_RATE) {
            return opus_network_clock::normalize_frame_count(sample_rate, requested_frame_count);
        }

        if (OpusEncoderWrapper::is_legal_frame_size(static_cast<int>(sample_rate),
                                                    requested_frame_count)) {
            return requested_frame_count;
        }

        const int low_latency_frame_count = static_cast<int>(sample_rate) / 400;
        if (OpusEncoderWrapper::is_legal_frame_size(static_cast<int>(sample_rate),
                                                    low_latency_frame_count)) {
            return static_cast<uint16_t>(low_latency_frame_count);
        }

        return 0;
    }

    void enqueue_opus_send_samples(const float* samples, unsigned long frame_count,
                                   uint32_t sample_rate,
                                   std::chrono::steady_clock::time_point capture_time) {
        if (frame_count == 0 || samples == nullptr) {
            return;
        }

        if (opus_tx_accumulator_reset_requested_.exchange(false, std::memory_order_acq_rel)) {
            opus_tx_accumulated_frames_ = 0;
            opus_tx_accumulator_capture_time_ = {};
        }

        const uint16_t target_frame_count = preferred_opus_tx_frame_count(
            sample_rate, get_opus_network_frame_count());
        if (target_frame_count == 0 || target_frame_count > opus_tx_accumulator_.size()) {
            opus_send_drops_.fetch_add(1, std::memory_order_relaxed);
            opus_tx_accumulated_frames_ = 0;
            opus_tx_accumulator_capture_time_ = {};
            return;
        }

        if (frame_count <= opus_tx_accumulator_.size() &&
            opus_network_clock::can_send_callback_direct(
                static_cast<size_t>(frame_count), opus_tx_accumulated_frames_,
                target_frame_count)) {
            enqueue_opus_send_frame(samples, static_cast<uint16_t>(frame_count), sample_rate,
                                    capture_time);
            return;
        }

        size_t offset = 0;
        while (offset < frame_count) {
            if (opus_tx_accumulated_frames_ == 0) {
                opus_tx_accumulator_capture_time_ = capture_time;
            }
            const size_t room =
                static_cast<size_t>(target_frame_count) - opus_tx_accumulated_frames_;
            const size_t samples_to_copy =
                std::min(room, static_cast<size_t>(frame_count) - offset);
            auto accumulator_out = opus_tx_accumulator_.begin() +
                                   static_cast<std::ptrdiff_t>(opus_tx_accumulated_frames_);
            std::copy_n(samples + offset, samples_to_copy, accumulator_out);

            opus_tx_accumulated_frames_ += samples_to_copy;
            offset += samples_to_copy;

            if (opus_tx_accumulated_frames_ == target_frame_count) {
                enqueue_opus_send_frame(opus_tx_accumulator_.data(), target_frame_count,
                                        sample_rate, opus_tx_accumulator_capture_time_);
                opus_tx_accumulated_frames_ = 0;
                opus_tx_accumulator_capture_time_ = {};
            }
        }
    }

    void wake_audio_sender_thread() {
        audio_sender_wake_.store(true, std::memory_order_release);
        if constexpr (AUDIO_CALLBACK_NOTIFY_ENABLED) {
            audio_sender_cv_.notify_one();
        }
    }

    static size_t consume_opus_pcm_buffer(ParticipantData& participant, size_t frame_count) {
        const size_t consumed = std::min(frame_count, participant.opus_pcm_buffered_frames);
        if (participant.opus_pcm_buffered_frames <= frame_count) {
            participant.opus_pcm_buffered_frames = 0;
            participant.opus_resample_phase = 0.0;
            return consumed;
        }

        const size_t remaining = participant.opus_pcm_buffered_frames - frame_count;
        std::move(participant.opus_pcm_buffer.begin() + static_cast<std::ptrdiff_t>(frame_count),
                  participant.opus_pcm_buffer.begin() +
                      static_cast<std::ptrdiff_t>(participant.opus_pcm_buffered_frames),
                  participant.opus_pcm_buffer.begin());
        participant.opus_pcm_buffered_frames = remaining;
        return consumed;
    }

    static double opus_playout_rate_ratio(ParticipantData& participant) {
        const size_t packet_frames =
            participant.last_packet_frame_count.load(std::memory_order_relaxed);
        const double decoded_packets =
            packet_frames > 0 ? static_cast<double>(participant.opus_pcm_buffered_frames) /
                                    static_cast<double>(packet_frames)
                              : 0.0;
        const double queued_packets =
            static_cast<double>(participant.opus_queue.size_approx()) + decoded_packets;
        const double target_packets =
            static_cast<double>(opus_playout_target_queue_packets(participant));
        double ratio = opus_playout_ratio_for_queue_depth(
            queued_packets, target_packets);

        const uint64_t queue_limit_drops =
            participant.opus_queue_limit_drops.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (queue_limit_drops > participant.opus_rate_last_queue_limit_drops) {
            participant.opus_rate_last_queue_limit_drops = queue_limit_drops;
            participant.opus_rate_correction_deadline =
                post_drop_rate_recovery::deadline(now);
        }
        if (post_drop_rate_recovery::active(
                now, participant.opus_rate_correction_deadline,
                queued_packets, target_packets)) {
            ratio = std::max(ratio, OPUS_PLAYOUT_MAX_RATIO);
        }

        participant.opus_playout_rate_ratio_micros.store(
            static_cast<int64_t>(ratio * 1'000'000.0), std::memory_order_relaxed);
        participant.opus_rate_correction_callbacks_observed.store(
            post_drop_rate_recovery::remaining_reference_callbacks(
                now, participant.opus_rate_correction_deadline),
            std::memory_order_relaxed);
        return ratio;
    }

    static size_t opus_resample_required_input_frames(const ParticipantData& participant,
                                                      unsigned long output_frames,
                                                      double ratio) {
        if (output_frames == 0) {
            return 0;
        }
        const double last_source =
            participant.opus_resample_phase +
            (static_cast<double>(output_frames - 1) * ratio);
        return static_cast<size_t>(std::floor(last_source)) + 1;
    }

    static size_t mix_resampled_opus_pcm(ParticipantData& participant, float* output_buffer,
                                         unsigned long output_frames, size_t output_channels,
                                         float gain, float pan, double ratio) {
        if (output_frames == 0 || output_buffer == nullptr) {
            return 0;
        }

        const bool panned = output_channels > 1 && !audio_analysis::is_center_pan(pan);
        audio_analysis::StereoPanGains pan_gains{};
        if (panned) {
            pan_gains = audio_analysis::stereo_pan_gains(pan);
        }
        const double start_phase = participant.opus_resample_phase;
        for (unsigned long i = 0; i < output_frames; ++i) {
            const double source_pos = start_phase + (static_cast<double>(i) * ratio);
            const auto index = static_cast<size_t>(std::floor(source_pos));
            const float frac = static_cast<float>(source_pos - static_cast<double>(index));
            const float a = participant.opus_pcm_buffer[index];
            const float b =
                index + 1 < participant.opus_pcm_buffered_frames
                    ? participant.opus_pcm_buffer[index + 1]
                    : a;
            const float sample = (a + ((b - a) * frac)) * gain;

            if (output_channels == 1) {
                output_buffer[i] += sample;
            } else {
                const size_t base = static_cast<size_t>(i) * output_channels;
                output_buffer[base] += sample * (panned ? pan_gains.left : 1.0F);
                output_buffer[base + 1] += sample * (panned ? pan_gains.right : 1.0F);
            }
        }

        const double consumed_exact =
            start_phase + (static_cast<double>(output_frames) * ratio);
        const auto consumed_frames = static_cast<size_t>(std::floor(consumed_exact));
        participant.opus_resample_phase =
            consumed_exact - static_cast<double>(consumed_frames);
        return consume_opus_pcm_buffer(participant, consumed_frames);
    }

    static size_t mix_available_opus_pcm_with_tail(ParticipantData& participant,
                                                   float* output_buffer,
                                                   unsigned long output_frames,
                                                   size_t output_channels, float gain,
                                                   float pan, double ratio) {
        if (output_frames == 0 || output_buffer == nullptr ||
            participant.opus_pcm_buffered_frames == 0) {
            return 0;
        }

        const bool panned = output_channels > 1 && !audio_analysis::is_center_pan(pan);
        audio_analysis::StereoPanGains pan_gains{};
        if (panned) {
            pan_gains = audio_analysis::stereo_pan_gains(pan);
        }
        const double start_phase = participant.opus_resample_phase;
        const size_t last_index = participant.opus_pcm_buffered_frames - 1;
        unsigned long frames_into_tail = 0;
        for (unsigned long i = 0; i < output_frames; ++i) {
            const double source_pos = start_phase + (static_cast<double>(i) * ratio);
            const auto requested_index = static_cast<size_t>(std::floor(source_pos));
            const size_t index = std::min(requested_index, last_index);
            const float frac = requested_index < last_index
                                   ? static_cast<float>(source_pos -
                                                        static_cast<double>(requested_index))
                                   : 0.0F;
            const float a = participant.opus_pcm_buffer[index];
            const float b = index + 1 < participant.opus_pcm_buffered_frames
                                ? participant.opus_pcm_buffer[index + 1]
                                : a;
            float sample = (a + ((b - a) * frac)) * gain;
            if (requested_index > last_index) {
                ++frames_into_tail;
                sample *= opus_tail_fade_gain(frames_into_tail, output_frames);
            }

            if (output_channels == 1) {
                output_buffer[i] += sample;
            } else {
                const size_t base = static_cast<size_t>(i) * output_channels;
                output_buffer[base] += sample * (panned ? pan_gains.left : 1.0F);
                output_buffer[base + 1] += sample * (panned ? pan_gains.right : 1.0F);
            }
        }

        const double consumed_exact =
            start_phase + (static_cast<double>(output_frames) * ratio);
        const auto consumed_frames = std::min(
            static_cast<size_t>(std::floor(consumed_exact)),
            participant.opus_pcm_buffered_frames);
        return consume_opus_pcm_buffer(participant, consumed_frames);
    }

    static void observe_participant_queue_depth(ParticipantData& participant, size_t depth) {
        size_t previous_max = participant.queue_depth_max.load(std::memory_order_relaxed);
        while (depth > previous_max &&
               !participant.queue_depth_max.compare_exchange_weak(
                   previous_max, depth, std::memory_order_relaxed)) {
        }

        size_t previous_avg = participant.queue_depth_avg.load(std::memory_order_relaxed);
        size_t next_avg = previous_avg == 0 ? depth : ((previous_avg * 31) + depth) / 32;
        participant.queue_depth_avg.store(next_avg, std::memory_order_relaxed);

        const auto target_depth = std::max<size_t>(
            1, participant.jitter_buffer_min_packets.load(std::memory_order_relaxed));
        const auto drift_milli =
            (static_cast<int64_t>(depth) - static_cast<int64_t>(target_depth)) * 1000;
        const auto previous_drift =
            participant.queue_depth_drift_milli.load(std::memory_order_relaxed);
        const auto next_drift =
            previous_drift == 0 ? drift_milli : ((previous_drift * 31) + drift_milli) / 32;
        participant.queue_depth_drift_milli.store(next_drift, std::memory_order_relaxed);
    }

    static void observe_receiver_clock_drift(ParticipantData& participant,
                                             const OpusPacket& packet) {
        if (!packet.sequence_valid || packet.frame_count == 0 || packet.sample_rate == 0) {
            return;
        }

        if (!participant.drift_reference_initialized ||
            packet.frame_count != participant.drift_reference_frame_count ||
            packet.sample_rate != participant.drift_reference_sample_rate) {
            participant.drift_reference_initialized = true;
            participant.drift_reference_sequence = packet.sequence;
            participant.drift_reference_frame_count = packet.frame_count;
            participant.drift_reference_sample_rate = packet.sample_rate;
            participant.drift_reference_time = packet.timestamp;
            return;
        }

        if (!sequence_number_after(packet.sequence, participant.drift_reference_sequence)) {
            return;
        }

        const uint32_t elapsed_packets = packet.sequence - participant.drift_reference_sequence;
        // Arrival timestamps include OS/network scheduling jitter; use a longer
        // window so the diagnostic reflects clock drift instead of one callback hiccup.
        constexpr uint32_t DRIFT_MIN_OBSERVATION_PACKETS = 12'000;
        if (elapsed_packets < DRIFT_MIN_OBSERVATION_PACKETS) {
            return;
        }

        const auto arrival_elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                packet.timestamp - participant.drift_reference_time)
                .count();
        const double expected_elapsed_ns =
            static_cast<double>(elapsed_packets) *
            static_cast<double>(packet.frame_count) * 1'000'000'000.0 /
            static_cast<double>(packet.sample_rate);
        if (expected_elapsed_ns <= 0.0) {
            return;
        }

        const auto drift_ppm_milli = static_cast<int64_t>(
            ((static_cast<double>(arrival_elapsed_ns) - expected_elapsed_ns) /
             expected_elapsed_ns) *
            1'000'000'000.0);
        constexpr int64_t DRIFT_ARRIVAL_OUTLIER_PPM_MILLI = 1'000'000;
        const int64_t abs_sample_drift =
            drift_ppm_milli < 0 ? -drift_ppm_milli : drift_ppm_milli;
        if (abs_sample_drift > DRIFT_ARRIVAL_OUTLIER_PPM_MILLI) {
            participant.drift_reference_sequence = packet.sequence;
            participant.drift_reference_time = packet.timestamp;
            return;
        }
        participant.receiver_drift_ppm_last_milli.store(drift_ppm_milli,
                                                        std::memory_order_relaxed);

        const int64_t previous_avg =
            participant.receiver_drift_ppm_avg_milli.load(std::memory_order_relaxed);
        const int64_t next_avg =
            previous_avg == 0 ? drift_ppm_milli : ((previous_avg * 63) + drift_ppm_milli) / 64;
        participant.receiver_drift_ppm_avg_milli.store(next_avg, std::memory_order_relaxed);

        constexpr uint64_t DRIFT_MAX_WARMUP_OBSERVATIONS = 16;
        const uint64_t observations =
            participant.receiver_drift_observations.fetch_add(1, std::memory_order_relaxed) + 1;
        if (observations > DRIFT_MAX_WARMUP_OBSERVATIONS) {
            int64_t previous_abs_max =
                participant.receiver_drift_ppm_abs_max_milli.load(std::memory_order_relaxed);
            while (abs_sample_drift > previous_abs_max &&
                   !participant.receiver_drift_ppm_abs_max_milli.compare_exchange_weak(
                       previous_abs_max, abs_sample_drift, std::memory_order_relaxed)) {
            }
        }

        participant.drift_reference_sequence = packet.sequence;
        participant.drift_reference_time = packet.timestamp;
    }

    static void observe_opus_pcm_depth(ParticipantData& participant) {
        participant.opus_pcm_buffered_frames_observed.store(
            participant.opus_pcm_buffered_frames, std::memory_order_relaxed);
    }

    static size_t opus_playout_target_queue_packets(const ParticipantData& participant) {
        const size_t jitter_floor = std::max<size_t>(
            1, participant.jitter_buffer_min_packets.load(std::memory_order_relaxed));
        return std::min<size_t>(MAX_OPUS_JITTER_PACKETS, jitter_floor);
    }

    static size_t opus_gap_wait_dequeue_attempts(const ParticipantData& participant) {
        const size_t packet_frames =
            participant.last_packet_frame_count.load(std::memory_order_relaxed);
        const size_t callback_frames =
            participant.last_callback_frame_count.load(std::memory_order_relaxed);
        if (packet_frames == 0 || callback_frames == 0) {
            return 1;
        }
        return std::max<size_t>(1, (packet_frames + callback_frames - 1) / callback_frames);
    }

    static size_t ready_threshold_packets(const ParticipantData& participant) {
        if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus) {
            return std::max<size_t>(
                1, participant.jitter_buffer_min_packets.load(std::memory_order_relaxed));
        }
        return participant.jitter_buffer_min_packets.load(std::memory_order_relaxed);
    }

    static size_t opus_rebuffer_empty_callback_threshold(const ParticipantData& participant) {
        const size_t packet_frames =
            participant.last_packet_frame_count.load(std::memory_order_relaxed);
        const size_t callback_frames =
            participant.last_callback_frame_count.load(std::memory_order_relaxed);
        const size_t target_packets = opus_playout_target_queue_packets(participant);
        if (packet_frames > 0 && callback_frames > 0 && callback_frames < packet_frames) {
            const size_t callbacks_per_packet =
                (packet_frames + callback_frames - 1) / callback_frames;
            return std::max<size_t>(3, target_packets * callbacks_per_packet);
        }
        return std::max<size_t>(3, target_packets);
    }

    static void observe_opus_age_limit_drop(ParticipantData& participant) {
        participant.jitter_age_drops.fetch_add(1, std::memory_order_relaxed);
        participant.opus_age_limit_drops.fetch_add(1, std::memory_order_relaxed);
        participant.receiver_delivery_drops.fetch_add(1, std::memory_order_relaxed);
    }

    static bool auto_jitter_is_active(const ParticipantData& participant) {
        return participant.opus_jitter_auto_enabled.load(std::memory_order_relaxed) &&
               !participant.opus_jitter_manual_override.load(std::memory_order_relaxed);
    }

    static void complete_auto_jitter_control_window(ParticipantData& participant,
                                                    int age_limit_ms) {
        participant.opus_jitter_auto_stable_callbacks.store(0,
                                                            std::memory_order_relaxed);
        const int instability_events =
            participant.opus_jitter_auto_instability_events.exchange(
                0, std::memory_order_relaxed);

        const size_t floor_packets =
            std::clamp(participant.opus_jitter_auto_floor_packets.load(
                           std::memory_order_relaxed),
                       MIN_OPUS_JITTER_PACKETS, MAX_OPUS_JITTER_PACKETS);
        size_t current_target =
            participant.jitter_buffer_min_packets.load(std::memory_order_relaxed);

        size_t max_target = MAX_OPUS_JITTER_PACKETS;
        const auto frame_count = static_cast<uint16_t>(
            participant.last_packet_frame_count.load(std::memory_order_relaxed));
        if (jitter_packet_age_limit_enabled(age_limit_ms) && frame_count > 0) {
            max_target = std::max(
                MIN_OPUS_JITTER_PACKETS,
                opus_jitter_packets_within_ms(age_limit_ms,
                                              opus_network_clock::SAMPLE_RATE,
                                              frame_count));
        }

        if (current_target > max_target) {
            participant.jitter_buffer_min_packets.store(max_target,
                                                        std::memory_order_relaxed);
            participant.jitter_buffer_floor_packets.store(max_target,
                                                          std::memory_order_relaxed);
            current_target = max_target;
        }

        if (instability_events >= OPUS_AUTO_JITTER_EVENTS_BEFORE_INCREASE) {
            if (current_target < max_target) {
                const size_t next_target =
                    std::min(max_target,
                             std::max<size_t>(3, current_target + 1));
                if (next_target > current_target) {
                    participant.jitter_buffer_min_packets.store(
                        next_target, std::memory_order_relaxed);
                    participant.jitter_buffer_floor_packets.store(
                        next_target, std::memory_order_relaxed);
                    const size_t queue_limit =
                        participant.opus_queue_limit_packets.load(
                            std::memory_order_relaxed);
                    participant.opus_queue_limit_packets.store(
                        std::max(queue_limit, next_target + 3),
                        std::memory_order_relaxed);
                    participant.opus_jitter_auto_increases.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            return;
        }

        if (instability_events == 0 && current_target > floor_packets) {
            const size_t next_target = current_target - 1;
            participant.jitter_buffer_min_packets.store(next_target,
                                                        std::memory_order_relaxed);
            participant.jitter_buffer_floor_packets.store(next_target,
                                                          std::memory_order_relaxed);
            participant.opus_jitter_auto_decreases.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    static void observe_auto_jitter_window_callback(ParticipantData& participant,
                                                    int age_limit_ms) {
        const int observed_callbacks =
            participant.opus_jitter_auto_stable_callbacks.fetch_add(
                1, std::memory_order_relaxed) +
            1;
        if (observed_callbacks >= OPUS_AUTO_JITTER_CONTROL_WINDOW_CALLBACKS) {
            complete_auto_jitter_control_window(participant, age_limit_ms);
        }
    }

    static void observe_auto_jitter_instability(ParticipantData& participant,
                                                int age_limit_ms) {
        if (!auto_jitter_is_active(participant)) {
            return;
        }

        participant.opus_jitter_auto_instability_events.fetch_add(
            1, std::memory_order_relaxed);
        observe_auto_jitter_window_callback(participant, age_limit_ms);
    }

    static void observe_auto_jitter_stable(ParticipantData& participant,
                                           int age_limit_ms) {
        if (!auto_jitter_is_active(participant)) {
            return;
        }

        observe_auto_jitter_window_callback(participant, age_limit_ms);
    }

    static size_t max_receive_queue_packets(const OpusPacket& packet, size_t opus_queue_limit) {
        size_t base_limit = TARGET_OPUS_QUEUE_SIZE + 1;
        if (packet.frame_count <= 128) {
            base_limit = 8;
        }
        base_limit = std::max(base_limit, opus_queue_limit);
        return std::min(base_limit, MAX_OPUS_QUEUE_SIZE);
    }

    size_t jitter_floor_for_packet(const OpusPacket& packet) const {
        size_t packets = jitter_floor_packets_for_audio(
            packet.frame_count, get_opus_jitter_buffer_ms(),
            opus_network_clock::SAMPLE_RATE);
        const int age_limit_ms = get_jitter_packet_age_limit_ms();
        if (age_limit_ms > 0) {
            packets = std::min(
                packets,
                opus_jitter_packets_within_ms(
                    age_limit_ms, opus_network_clock::SAMPLE_RATE,
                    packet.frame_count));
        }
        return packets;
    }

    static size_t opus_latency_trim_threshold_packets(const ParticipantData& participant) {
        const size_t target_packets = opus_playout_target_queue_packets(participant);
        const size_t packet_frames =
            participant.last_packet_frame_count.load(std::memory_order_relaxed);
        const size_t callback_frames =
            participant.last_callback_frame_count.load(std::memory_order_relaxed);
        size_t burst_headroom = 3;
        if (packet_frames > 0 && callback_frames > packet_frames) {
            burst_headroom = std::max(
                burst_headroom,
                (callback_frames + packet_frames - 1) / packet_frames);
        }
        return std::min<size_t>(MAX_OPUS_QUEUE_SIZE, target_packets + burst_headroom);
    }

    static size_t trim_opus_queue_to_latency_target(ParticipantData& participant) {
        if (participant.last_codec.load(std::memory_order_relaxed) != AudioCodec::Opus) {
            return participant.opus_queue.size_approx();
        }

        size_t current_queue_size = participant.opus_queue.size_approx();
        const size_t target_packets = opus_playout_target_queue_packets(participant);
        const size_t trim_threshold = opus_latency_trim_threshold_packets(participant);
        const size_t trim_goal = opus_latency_trim_goal_packets(
            current_queue_size, target_packets, trim_threshold);
        while (current_queue_size > trim_goal) {
            bool receiver_drop_ready = false;
            if (!participant.opus_queue.discard_oldest_actual_packet_for_latency_trim(
                    &receiver_drop_ready)) {
                break;
            }
            --current_queue_size;
            participant.opus_target_trim_drops.fetch_add(1, std::memory_order_relaxed);
            if (receiver_drop_ready) {
                participant.receiver_delivery_drops.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        return current_queue_size;
    }

    static size_t flush_stalled_delivery_backlog(ParticipantData& participant) {
        if (!participant.delivery_stall_flush_requested.exchange(
                false, std::memory_order_acq_rel)) {
            return participant.opus_queue.size_approx();
        }

        size_t current_queue_size = participant.opus_queue.size_approx();
        const size_t target_packets = opus_playout_target_queue_packets(participant);
        while (current_queue_size > target_packets) {
            bool receiver_drop_ready = false;
            if (!participant.opus_queue.discard_oldest_actual_packet_for_latency_trim(
                    &receiver_drop_ready)) {
                break;
            }
            --current_queue_size;
            participant.opus_target_trim_drops.fetch_add(1, std::memory_order_relaxed);
            if (receiver_drop_ready) {
                participant.receiver_delivery_drops.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        return current_queue_size;
    }

    void update_jitter_floor(ParticipantData& participant, const OpusPacket& packet) {
        const bool manual_override =
            packet.codec == AudioCodec::Opus &&
            participant.opus_jitter_manual_override.load(std::memory_order_relaxed);
        size_t floor_packets = manual_override
                                   ? jitter_floor_packets_for_audio(
                                         packet.frame_count,
                                         participant.opus_jitter_override_ms.load(
                                             std::memory_order_relaxed),
                                         opus_network_clock::SAMPLE_RATE)
                                   : jitter_floor_for_packet(packet);
        const int age_limit_ms = get_jitter_packet_age_limit_ms();
        if (manual_override && age_limit_ms > 0) {
            floor_packets = std::min(
                floor_packets,
                opus_jitter_packets_within_ms(
                    age_limit_ms, opus_network_clock::SAMPLE_RATE,
                    packet.frame_count));
        }

        const size_t previous_frame_count =
            participant.last_packet_frame_count.load(std::memory_order_relaxed);
        if (manual_override) {
            const size_t current_target =
                participant.jitter_buffer_min_packets.load(std::memory_order_relaxed);
            participant.jitter_buffer_floor_packets.store(floor_packets,
                                                          std::memory_order_relaxed);
            participant.jitter_buffer_min_packets.store(floor_packets,
                                                        std::memory_order_relaxed);
            if (current_target != floor_packets &&
                participant.opus_queue.size_approx() < std::max<size_t>(1, floor_packets)) {
                participant.buffer_ready.store(false, std::memory_order_relaxed);
            }
            return;
        }

        participant.opus_jitter_auto_floor_packets.store(floor_packets,
                                                         std::memory_order_relaxed);
        if (previous_frame_count == 0 || previous_frame_count != packet.frame_count) {
            const size_t auto_start_packets = opus_auto_start_jitter_packets_for_audio(
                floor_packets, opus_network_clock::SAMPLE_RATE, packet.frame_count);
            apply_opus_jitter_policy_to_participant(
                participant, floor_packets, auto_start_packets,
                participant.opus_jitter_auto_enabled.load(std::memory_order_relaxed),
                true);
            return;
        }
        participant.jitter_buffer_floor_packets.store(floor_packets,
                                                      std::memory_order_relaxed);
        const size_t current_target =
            participant.jitter_buffer_min_packets.load(std::memory_order_relaxed);
        if (jitter_target_should_snap_to_floor(
                participant.opus_jitter_manual_override.load(std::memory_order_relaxed),
                participant.opus_jitter_auto_enabled.load(std::memory_order_relaxed),
                participant.buffer_ready.load(std::memory_order_relaxed), current_target,
                floor_packets)) {
            participant.jitter_buffer_min_packets.store(floor_packets,
                                                        std::memory_order_relaxed);
        }
    }

    void observe_opus_send_queue_age(std::chrono::steady_clock::time_point capture_time) {
        if (capture_time.time_since_epoch().count() == 0) {
            return;
        }

        const auto age_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - capture_time)
                                .count();
        observe_latency_sample(opus_send_queue_age_last_ns_, opus_send_queue_age_avg_ns_,
                               opus_send_queue_age_max_ns_, age_ns);
        opus_send_queue_age_window_.observe(age_ns);
        opus_send_queue_age_p99_ns_.store(opus_send_queue_age_window_.percentile_99_ns(),
                                          std::memory_order_relaxed);
    }

    void observe_tx_encode_time(std::chrono::steady_clock::duration elapsed) {
        observe_latency_sample(
            tx_encode_last_ns_, tx_encode_avg_ns_, tx_encode_max_ns_,
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    void observe_rx_decode_time(std::chrono::steady_clock::duration elapsed) {
        observe_latency_sample(
            rx_decode_last_ns_, rx_decode_avg_ns_, rx_decode_max_ns_,
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    void observe_rx_playout_time(std::chrono::steady_clock::duration elapsed) {
        observe_latency_sample(
            rx_playout_last_ns_, rx_playout_avg_ns_, rx_playout_max_ns_,
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    void observe_audio_packet_send_pacing() {
        const auto now = std::chrono::steady_clock::now();
        if (last_audio_packet_send_time_.time_since_epoch().count() != 0) {
            observe_latency_sample(
                tx_send_pace_last_ns_, tx_send_pace_avg_ns_, tx_send_pace_max_ns_,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - last_audio_packet_send_time_)
                    .count());
        }
        last_audio_packet_send_time_ = now;
    }

    static void observe_latency_sample(std::atomic<int64_t>& last_ns,
                                       std::atomic<int64_t>& avg_ns,
                                       std::atomic<int64_t>& max_ns,
                                       int64_t sample_ns) {
        last_ns.store(sample_ns, std::memory_order_relaxed);

        int64_t previous_max = max_ns.load(std::memory_order_relaxed);
        while (sample_ns > previous_max &&
               !max_ns.compare_exchange_weak(previous_max, sample_ns,
                                             std::memory_order_relaxed)) {
        }

        const int64_t previous_avg = avg_ns.load(std::memory_order_relaxed);
        const int64_t next_avg =
            previous_avg == 0 ? sample_ns : ((previous_avg * 31) + sample_ns) / 32;
        avg_ns.store(next_avg, std::memory_order_relaxed);
    }

    static void append_opus_capture_chunk(ParticipantData& participant, size_t frames,
                                          int64_t capture_server_time_ns, bool valid,
                                          bool received_media) {
        if (!participant.opus_pcm_outcomes.append(
                frames, capture_server_time_ns, valid, received_media)) {
            const uint64_t lost =
                participant.opus_pcm_outcomes.discard_overflow_as_lost(
                    received_media);
            participant.opus_pcm_buffered_frames = 0;
            participant.receiver_delivery_drops.fetch_add(
                lost, std::memory_order_relaxed);
        }
    }

    static uint64_t discard_received_opus_capture_chunks(
        ParticipantData& participant) {
        return participant.opus_pcm_outcomes.discard_all_as_lost();
    }

    static void clear_opus_capture_chunks(ParticipantData& participant) {
        participant.opus_pcm_outcomes.clear();
    }

    static void observe_and_consume_opus_capture_chunks(ParticipantData& participant,
                                                        size_t consumed_frames,
                                                        std::optional<int64_t>
                                                            playout_server_time_ns) {
        FrameWeightedLatencyAccumulator latency;
        const uint64_t delivered = participant.opus_pcm_outcomes.consume(
            consumed_frames, [&](const OpusPcmCaptureChunk& chunk,
                                 size_t consumed_from_chunk) {
                if (chunk.received_media && chunk.capture_timestamp_valid &&
                    chunk.capture_server_time_ns > 0 &&
                    playout_server_time_ns.has_value() &&
                    *playout_server_time_ns > chunk.capture_server_time_ns) {
                    const int64_t sample_ns =
                        *playout_server_time_ns - chunk.capture_server_time_ns;
                    latency.observe(sample_ns, consumed_from_chunk,
                                    consumed_from_chunk == chunk.frames);
                }
            });
        if (!latency.empty()) {
            observe_latency_sample(participant.capture_to_playout_latency_last_ns,
                                   participant.capture_to_playout_latency_avg_ns,
                                   participant.capture_to_playout_latency_max_ns,
                                   latency.average_ns());
            int64_t previous_max =
                participant.capture_to_playout_latency_max_ns.load(
                    std::memory_order_relaxed);
            while (latency.max_latency_ns > previous_max &&
                   !participant.capture_to_playout_latency_max_ns.compare_exchange_weak(
                       previous_max, latency.max_latency_ns,
                       std::memory_order_relaxed)) {
            }
            participant.capture_to_playout_latency_samples.fetch_add(
                latency.completed_samples, std::memory_order_relaxed);
        }
        participant.receiver_delivery_delivered.fetch_add(
            delivered, std::memory_order_relaxed);
    }

    void ping_timer_callback() {
        if (!outbound_enabled_.load(std::memory_order_acquire) ||
            current_server_endpoint().port() == 0 ||
            !join_session_.is_join_confirmed()) {
            return;
        }
        SyncHdr         shdr{};
        shdr.magic = PING_MAGIC;
        shdr.seq   = ping_tx_sequence_.fetch_add(1, std::memory_order_relaxed);
        auto now   = std::chrono::steady_clock::now();
        shdr.t1_client_send =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(SyncHdr));
        std::memcpy(buf->data(), &shdr, sizeof(SyncHdr));
        send(buf->data(), buf->size(), buf);
        observe_ping_path_timeout(shdr.seq);
    }

    void alive_timer_callback() {
        if (!outbound_enabled_.load(std::memory_order_acquire) ||
            current_server_endpoint().port() == 0) {
            return;
        }
        if (!join_session_.is_join_confirmed()) {
            send_join();
            log_audio_diagnostics();
            return;
        }

        CtrlHdr chdr{};
        chdr.magic = CTRL_MAGIC;
        chdr.type  = CtrlHdr::Cmd::ALIVE;
        auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(CtrlHdr));
        std::memcpy(buf->data(), &chdr, sizeof(CtrlHdr));
        send(buf->data(), buf->size(), buf);
        log_audio_diagnostics();
    }

    void join_retry_timer_callback() {
        if (!outbound_enabled_.load(std::memory_order_acquire) ||
            current_server_endpoint().port() == 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (join_session_.should_send_join(now)) {
            send_join();
        }
        retry_pending_key_handoffs_on_io();
    }

    void chat_timer_callback() {
        if (!chat_available_on_io_snapshot()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        bool request_history = false;
        uint64_t after_sequence = 0;
        {
            std::lock_guard<std::mutex> lock(chat_mutex_);
            for (auto& [_, pending]: pending_chat_messages_) {
                if (!pending.history_requested &&
                    now - pending.sent_at > std::chrono::seconds(2)) {
                    pending.history_requested = true;
                    request_history = true;
                    after_sequence = last_seen_chat_sequence_;
                }
            }
        }
        if (request_history) {
            request_chat_history_on_io(after_sequence);
        }
    }

    void log_audio_diagnostics() {
        struct DropRate {
            double opus_send_per_sec;
            double jitter_depth_per_sec;
            double jitter_age_per_sec;
        };

        auto calculate_rate = [](uint64_t current, uint64_t previous, double elapsed_sec) {
            if (elapsed_sec <= 0.0 || current < previous) {
                return 0.0;
            }
            return static_cast<double>(current - previous) / elapsed_sec;
        };

        const auto now = std::chrono::steady_clock::now();
        double elapsed_sec = 0.0;
        if (last_audio_health_log_time_.time_since_epoch().count() != 0) {
            elapsed_sec = std::chrono::duration<double>(now - last_audio_health_log_time_).count();
        }
        last_audio_health_log_time_ = now;

        const uint64_t opus_send_drops = opus_send_drops_.load(std::memory_order_relaxed);
        const uint64_t outbound_malformed_audio_drops =
            outbound_malformed_audio_drops_.load(std::memory_order_relaxed);
        const double opus_send_drop_rate =
            calculate_rate(opus_send_drops, last_opus_send_drops_, elapsed_sec);
        const double outbound_malformed_audio_drop_rate =
            calculate_rate(outbound_malformed_audio_drops,
                           last_outbound_malformed_audio_drops_, elapsed_sec);
        last_opus_send_drops_ = opus_send_drops;
        last_outbound_malformed_audio_drops_ = outbound_malformed_audio_drops;

        const auto participants = participant_manager_.get_all_info();
        const auto ns_to_ms = [](int64_t ns) {
            return static_cast<double>(ns) / 1'000'000.0;
        };

        spdlog::info(
            "Audio diag: frames={} tx_packets={} tx_drops opus={} "
            "tx_socket_would_block={} tx_malformed={} ({:.1f}/s) "
            "sendq_age_ms opus_last/avg/max/p99={:.2f}/{:.2f}/{:.2f}/{:.2f} "
            "rx_bytes={} tx_bytes={}",
                  current_audio_frames_per_buffer(),
                  audio_tx_sequence_.load(std::memory_order_relaxed),
                  opus_send_drops,
                  tx_socket_would_block_drops_.load(std::memory_order_relaxed),
                  outbound_malformed_audio_drops,
                  outbound_malformed_audio_drop_rate,
                  ns_to_ms(opus_send_queue_age_last_ns_.load(std::memory_order_relaxed)),
                  ns_to_ms(opus_send_queue_age_avg_ns_.load(std::memory_order_relaxed)),
                  ns_to_ms(opus_send_queue_age_max_ns_.load(std::memory_order_relaxed)),
                  ns_to_ms(opus_send_queue_age_p99_ns_.load(std::memory_order_relaxed)),
                  total_bytes_rx_.load(std::memory_order_relaxed),
                  total_bytes_tx_.load(std::memory_order_relaxed));

        spdlog::info(
            "Latency diag: callback_ms last/avg/max/deadline={:.3f}/{:.3f}/{:.3f}/{:.3f} "
            "over={} txq_ms opus={:.3f}/{:.3f}/{:.3f} opus_p99={:.3f} "
            "encode_ms={:.3f}/{:.3f}/{:.3f} send_pace_ms={:.3f}/{:.3f}/{:.3f} "
            "rx_decode_ms={:.3f}/{:.3f}/{:.3f} rx_playout_ms={:.3f}/{:.3f}/{:.3f}",
            ns_to_ms(callback_last_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(callback_avg_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(callback_max_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(callback_deadline_ns_.load(std::memory_order_relaxed)),
            callback_over_deadline_count_.load(std::memory_order_relaxed),
            ns_to_ms(opus_send_queue_age_last_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(opus_send_queue_age_avg_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(opus_send_queue_age_max_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(opus_send_queue_age_p99_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(tx_encode_last_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(tx_encode_avg_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(tx_encode_max_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(tx_send_pace_last_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(tx_send_pace_avg_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(tx_send_pace_max_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(rx_decode_last_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(rx_decode_avg_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(rx_decode_max_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(rx_playout_last_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(rx_playout_avg_ns_.load(std::memory_order_relaxed)),
            ns_to_ms(rx_playout_max_ns_.load(std::memory_order_relaxed)));

        for (const auto& p: participants) {
            auto& previous = participant_drop_snapshots_[p.id];
            DropRate drop_rate{
                opus_send_drop_rate,
                calculate_rate(p.jitter_depth_drops, previous.jitter_depth_drops, elapsed_sec),
                calculate_rate(p.jitter_age_drops, previous.jitter_age_drops, elapsed_sec),
            };
            const auto decoded_packet_rate = calculate_rate(
                p.opus_packets_decoded_in_callback,
                previous.opus_packets_decoded_in_callback, elapsed_sec);
            const auto queue_limit_drop_rate = calculate_rate(
                p.opus_queue_limit_drops, previous.opus_queue_limit_drops, elapsed_sec);
            const auto age_limit_drop_rate = calculate_rate(
                p.opus_age_limit_drops, previous.opus_age_limit_drops, elapsed_sec);
            const auto decode_overflow_drop_rate = calculate_rate(
                p.opus_decode_buffer_overflow_drops,
                previous.opus_decode_buffer_overflow_drops, elapsed_sec);
            const auto target_trim_rate = calculate_rate(
                p.opus_target_trim_drops, previous.opus_target_trim_drops, elapsed_sec);
            previous.jitter_depth_drops = p.jitter_depth_drops;
            previous.jitter_age_drops = p.jitter_age_drops;
            previous.opus_packets_decoded_in_callback = p.opus_packets_decoded_in_callback;
            previous.opus_queue_limit_drops = p.opus_queue_limit_drops;
            previous.opus_age_limit_drops = p.opus_age_limit_drops;
            previous.opus_decode_buffer_overflow_drops =
                p.opus_decode_buffer_overflow_drops;
            previous.opus_target_trim_drops = p.opus_target_trim_drops;

            spdlog::info(
                "Participant diag {}: ready={} q={} q_avg={} q_max={} q_drift={:.2f} "
                "jitter_buffer={} queue_limit={} frames pkt/cb={}/{} decoded_frames={} decoded_packets={} age_avg_ms={:.1f} e2e_avg_ms={:.1f} e2e_max_ms={:.1f} e2e_samples={} drift_ppm last/avg/max={:.1f}/{:.1f}/{:.1f} underruns={} plc={} drops q/age={}/{} drop_detail limit/age/overflow={}/{}/{} seq gap/recovered/unresolved/late={}/{}/{}/{} "
                "target_trim={} drop_rate opus/q/age={:.1f}/{:.1f}/{:.1f}/s",
                p.id, p.buffer_ready, p.queue_size, p.queue_size_avg, p.queue_size_max,
                p.queue_drift_packets, p.jitter_buffer_min_packets,
                p.opus_queue_limit_packets, p.last_packet_frame_count,
                p.last_callback_frame_count, p.opus_pcm_buffered_frames,
                p.opus_packets_decoded_in_callback, p.packet_age_avg_ms,
                p.capture_to_playout_latency_avg_ms,
                p.capture_to_playout_latency_max_ms,
                p.capture_to_playout_latency_samples,
                p.receiver_drift_ppm_last, p.receiver_drift_ppm_avg,
                p.receiver_drift_ppm_abs_max,
                p.underrun_count, p.plc_count, p.jitter_depth_drops, p.jitter_age_drops,
                p.opus_queue_limit_drops,
                p.opus_age_limit_drops, p.opus_decode_buffer_overflow_drops,
                p.sequence_gaps, p.sequence_gap_recoveries, p.sequence_unresolved_gaps,
                p.sequence_late_or_reordered, p.opus_target_trim_drops,
                drop_rate.opus_send_per_sec, drop_rate.jitter_depth_per_sec,
                drop_rate.jitter_age_per_sec);
            spdlog::info(
                "Participant playout rates {}: decoded_packets={:.1f}/s ratio={:.4f} correction_callbacks={} drops limit/age/overflow/target={:.1f}/{:.1f}/{:.1f}/{:.1f}/s",
                p.id, decoded_packet_rate, p.opus_playout_rate_ratio,
                p.opus_rate_correction_callbacks, queue_limit_drop_rate, age_limit_drop_rate,
                decode_overflow_drop_rate, target_trim_rate);

            if (elapsed_sec > 0.0 &&
                (drop_rate.opus_send_per_sec > 5.0 ||
                 drop_rate.jitter_depth_per_sec > 100.0 ||
                 drop_rate.jitter_age_per_sec > 5.0)) {
                spdlog::warn(
                    "Audio health warning for participant {}: likely corrupt/robotic risk "
                    "(opus_drop_rate={:.1f}/s queue_drop_rate={:.1f}/s "
                    "age_drop_rate={:.1f}/s)",
                    p.id, drop_rate.opus_send_per_sec,
                    drop_rate.jitter_depth_per_sec, drop_rate.jitter_age_per_sec);
            }
        }
    }

private:
    void handle_ctrl_message(std::size_t bytes, const char* recv_data) {
        // Add to total bytes received
        total_bytes_rx_.fetch_add(bytes, std::memory_order_relaxed);

        if (bytes < sizeof(CtrlHdr)) {
            return;
        }

        CtrlHdr chdr{};
        std::memcpy(&chdr, recv_data, sizeof(CtrlHdr));

        switch (chdr.type) {
            case CtrlHdr::Cmd::PARTICIPANT_LEAVE: {
                uint32_t participant_id = chdr.participant_id;
                remove_participant(participant_id);
                spdlog::info("Participant {} left (server notification)", participant_id);
                break;
            }
            case CtrlHdr::Cmd::PARTICIPANT_INFO: {
                if (bytes < sizeof(ParticipantInfoHdr)) {
                    break;
                }
                ParticipantInfoHdr info{};
                std::memcpy(&info, recv_data, sizeof(ParticipantInfoHdr));
                uint32_t participant_capabilities = 0;
                Bytes<E2E_PUBLIC_KEY_BYTES> key_public{};
                bool has_key_public = false;
                if (bytes >= sizeof(ParticipantInfoCapsHdr)) {
                    ParticipantInfoCapsHdr caps_info{};
                    std::memcpy(&caps_info, recv_data, sizeof(ParticipantInfoCapsHdr));
                    participant_capabilities =
                        caps_info.capabilities & AUDIO_SUPPORTED_CAPABILITIES;
                    key_public = caps_info.key_public;
                    has_key_public = std::any_of(key_public.begin(), key_public.end(),
                                                 [](char value) { return value != 0; });
                }
                const auto profile_id = fixed_string(info.profile_id);
                const auto display_name = fixed_string(info.display_name);
                participant_manager_.set_participant_metadata(info.participant_id, profile_id,
                                                              display_name, key_public,
                                                              has_key_public);
                media_state_.set_participant_metadata(info.participant_id, profile_id,
                                                      display_name);
                remember_participant_key(info.participant_id, profile_id, display_name,
                                         key_public, has_key_public);
                maybe_auto_share_media_key(info.participant_id);
                spdlog::info("Participant {} metadata: user='{}' display='{}' capabilities=0x{:08x}",
                          info.participant_id, profile_id, display_name,
                          participant_capabilities);
                break;
            }
            case CtrlHdr::Cmd::JOIN_ACK: {
                const bool was_confirmed = join_session_.is_join_confirmed();
                uint32_t server_capabilities = 0;
                if (bytes >= sizeof(JoinAckHdr)) {
                    JoinAckHdr ack{};
                    std::memcpy(&ack, recv_data, sizeof(JoinAckHdr));
                    server_capabilities = ack.capabilities;
                }
                join_session_.mark_join_ack(chdr.participant_id, server_capabilities);
                if (!was_confirmed) {
                    reset_ping_path_feedback_to_current_sequence();
                }
                spdlog::info("JOIN acknowledged by server (participant ID: {}, capabilities=0x{:08x})",
                          chdr.participant_id, server_capabilities);
                break;
            }
            case CtrlHdr::Cmd::JOIN_DENIED: {
                if (bytes < sizeof(JoinDeniedHdr)) {
                    break;
                }
                JoinDeniedHdr denied{};
                std::memcpy(&denied, recv_data, sizeof(JoinDeniedHdr));
                if (denied.reason == 1) {
                    spdlog::warn("JOIN denied: room is full ({} participants max)",
                                 MAX_ROOM_PARTICIPANTS);
                    join_denied_room_full_.store(true, std::memory_order_release);
                    disconnect_from_server(false);
                }
                break;
            }
            case CtrlHdr::Cmd::JOIN_REQUIRED: {
                join_session_.mark_join_required();
                OpusSendFrame discarded_opus;
                while (opus_send_queue_.try_dequeue(discarded_opus)) {
                }
                request_recent_opus_audio_packets_reset();
                spdlog::warn("Server requested JOIN refresh; resending JOIN");
                send_join();
                break;
            }
            case CtrlHdr::Cmd::ROOM_REMOVED: {
                spdlog::warn("Server removed this client from room '{}'",
                             join_session_.room_id());
                room_removed_by_server_.store(true, std::memory_order_release);
                disconnect_from_server(false);
                break;
            }
            case CtrlHdr::Cmd::AUDIO_PATH_STATS: {
                handle_audio_path_stats_message(bytes, recv_data);
                break;
            }
            case CtrlHdr::Cmd::METRONOME_SYNC: {
                if (bytes < sizeof(MetronomeSyncHdr)) {
                    break;
                }
                MetronomeSyncHdr sync{};
                std::memcpy(&sync, recv_data, sizeof(MetronomeSyncHdr));
                schedule_metronome_sync(sync);
                metronome_.mark_sync_received();
                spdlog::info("Metronome sync: bpm={:.1f} running={} beat={} seq={} effective_ns={}",
                          static_cast<double>(sync.bpm_milli) / 1000.0,
                          (sync.flags & METRONOME_FLAG_RUNNING) != 0, sync.beat_number,
                          sync.sequence, sync.effective_server_time_ns);
                break;
            }
            case CtrlHdr::Cmd::ROOM_CHAT_EVENT: {
                handle_chat_event_message(bytes, recv_data);
                break;
            }
            case CtrlHdr::Cmd::ROOM_CHAT_SEND_REJECTED: {
                handle_chat_rejected_message(bytes, recv_data);
                break;
            }
            case CtrlHdr::Cmd::ROOM_CHAT_HISTORY_RESPONSE: {
                handle_chat_history_response_message(bytes, recv_data);
                break;
            }
            case CtrlHdr::Cmd::ROOM_KEY_HANDOFF_ACK: {
                if (bytes < sizeof(RoomKeyHandoffAckHdr)) {
                    break;
                }
                RoomKeyHandoffAckHdr ack{};
                std::memcpy(&ack, recv_data, sizeof(ack));
                const std::string media_key_commitment(
                    ack.media_key_commitment.begin(),
                    ack.media_key_commitment.end());
                if (ack.target_id == join_session_.participant_id() &&
                    ack.participant_id != 0 &&
                    ack.access_epoch == join_session_.access_epoch() &&
                    media_key_commitment == join_session_.media_key_commitment()) {
                    std::lock_guard<std::mutex> lock(access_request_mutex_);
                    pending_key_handoff_acks_.erase(ack.participant_id);
                    spdlog::info(
                        "Participant {} acknowledged encrypted room key",
                        ack.participant_id);
                }
                break;
            }
            default:
                // Other CTRL messages (JOIN, LEAVE, ALIVE) are not handled by clients
                break;
        }
    }

    void remove_participant(uint32_t participant_id) {
        participant_manager_.remove_participant(participant_id);
        delivery_report_accumulators_.erase(participant_id);
        downstream_delivery_by_reporter_.erase(participant_id);
        downstream_delivery_last_activity_.erase(participant_id);
        downstream_delivery_tombstone_since_.erase(participant_id);
        downstream_feedback_by_reporter_.erase(participant_id);
        secure_control_replay_windows_.erase(participant_id);
        std::lock_guard<std::mutex> lock(access_request_mutex_);
        participant_key_public_.erase(participant_id);
        pending_access_requests_.erase(participant_id);
        pending_key_handoff_acks_.erase(participant_id);
    }

    void log_rt_callback_diagnostics() {
        const uint64_t mix = rt_diag_mix_size_mismatches_.load(std::memory_order_relaxed);
        const uint64_t decode = rt_diag_decode_failures_.load(std::memory_order_relaxed);
        const uint64_t budget =
            rt_diag_decode_budget_exhaustions_.load(std::memory_order_relaxed);
        if (mix == rt_diag_logged_mix_size_mismatches_ &&
            decode == rt_diag_logged_decode_failures_ &&
            budget == rt_diag_logged_decode_budget_exhaustions_) {
            return;
        }
        spdlog::warn(
            "Audio callback diagnostics: mix_size_mismatches={} decode_failures={} "
            "decode_budget_exhaustions={}",
            mix, decode, budget);
        rt_diag_logged_mix_size_mismatches_ = mix;
        rt_diag_logged_decode_failures_ = decode;
        rt_diag_logged_decode_budget_exhaustions_ = budget;
    }

    void snapshot_delivery_outcomes() {
        participant_manager_.for_each(
            [&](uint32_t participant_id, ParticipantData& participant) {
                const auto found = delivery_report_accumulators_.find(participant_id);
                if (found == delivery_report_accumulators_.end()) {
                    return;
                }
                auto& report = found->second;
                report.delivered.observe(
                    participant.receiver_delivery_delivered.load(
                        std::memory_order_relaxed));
                report.lost.observe(
                    participant.sequence_gaps_declared_lost.load(
                        std::memory_order_relaxed) +
                    participant.receiver_delivery_drops.load(
                        std::memory_order_relaxed));
            });
    }

    void cleanup_timer_callback() {
        // Remove participants who haven't sent packets in a while (backup cleanup)
        auto           now                 = std::chrono::steady_clock::now();
        constexpr auto PARTICIPANT_TIMEOUT = 20s;

        snapshot_delivery_outcomes();
        auto removed_ids =
            participant_manager_.remove_timed_out_participants(now, PARTICIPANT_TIMEOUT);

        for (uint32_t id: removed_ids) {
            if (auto report = delivery_report_accumulators_.find(id);
                report != delivery_report_accumulators_.end()) {
                report->second.delivered.end_media_lifetime();
                report->second.lost.end_media_lifetime();
                report->second.inactive_since = now;
            }
            spdlog::info(
                "Removed stale participant {} (no packets for {}s)", id,
                std::chrono::duration_cast<std::chrono::seconds>(PARTICIPANT_TIMEOUT).count());
        }

        for (auto it = delivery_report_accumulators_.begin();
             it != delivery_report_accumulators_.end();) {
            if (it->second.inactive_since !=
                    std::chrono::steady_clock::time_point{} &&
                now - it->second.inactive_since >=
                    DELIVERY_REPORT_STATE_RETENTION) {
                it = delivery_report_accumulators_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = downstream_delivery_last_activity_.begin();
             it != downstream_delivery_last_activity_.end();) {
            if (now - it->second >= DELIVERY_REPORT_STATE_RETENTION) {
                downstream_delivery_tombstone_since_[it->first] = now;
                it = downstream_delivery_last_activity_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = downstream_delivery_tombstone_since_.begin();
             it != downstream_delivery_tombstone_since_.end();) {
            if (now - it->second >= DELIVERY_REPORT_STATE_RETENTION) {
                downstream_delivery_by_reporter_.erase(it->first);
                it = downstream_delivery_tombstone_since_.erase(it);
            } else {
                ++it;
            }
        }

        log_rt_callback_diagnostics();
        participant_manager_.reap_retired_participants();
        media_state_.reap_retired_wavs();
        apply_downstream_delivery_feedback();
        send_audio_delivery_reports();
        recover_audio_device_if_needed();
    }

    void recover_audio_device_if_needed() {
        if (!audio_stream_requested_.load(std::memory_order_acquire) ||
            audio_.is_stream_active() ||
            audio_recovery_in_flight_.load(std::memory_order_acquire)) {
            return;
        }

        const int64_t now_ns = steady_now_ns();
        if (now_ns < next_audio_recovery_ns_.load(std::memory_order_acquire)) {
            return;
        }

        const auto input = get_selected_input_device();
        const auto output = get_selected_output_device();
        if (input == AudioStream::NO_DEVICE || output == AudioStream::NO_DEVICE) {
            audio_stream_requested_.store(false, std::memory_order_release);
            return;
        }

        const uint32_t attempt =
            audio_recovery_attempts_.fetch_add(1, std::memory_order_acq_rel) + 1;
        bool expected = false;
        if (!audio_recovery_in_flight_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        const uint64_t generation =
            audio_device_generation_.load(std::memory_order_acquire);
        const auto config = get_audio_config();
        const std::weak_ptr<int> lifetime = lifetime_token_;
        spdlog::warn("Audio device stopped unexpectedly; reopen attempt {}", attempt);
        asio::post(audio_device_worker_,
                   [this, lifetime, input, output, config, generation, attempt]() {
            bool success = false;
            {
                std::lock_guard<std::mutex> lock(audio_lifecycle_mutex_);
                if (!lifetime.expired() &&
                    audio_stream_requested_.load(std::memory_order_acquire) &&
                    audio_device_generation_.load(std::memory_order_acquire) == generation) {
                    success = start_audio_stream_locked(input, output, config);
                }
            }
            asio::post(io_context_, [this, lifetime, success, generation, attempt]() {
                if (lifetime.expired()) {
                    return;
                }
                if (audio_device_generation_.load(std::memory_order_acquire) != generation ||
                    !audio_stream_requested_.load(std::memory_order_acquire)) {
                    audio_recovery_in_flight_.store(false, std::memory_order_release);
                    return;
                }
                audio_recovery_in_flight_.store(false, std::memory_order_release);
                if (success) {
                    audio_recovery_attempts_.store(0, std::memory_order_release);
                    next_audio_recovery_ns_.store(0, std::memory_order_release);
                    spdlog::info("Audio device reopened automatically");
                    return;
                }

                const int backoff_seconds =
                    std::min<int>(4, 1 << std::min<uint32_t>(attempt - 1, 2));
                next_audio_recovery_ns_.store(
                    steady_now_ns() +
                        static_cast<int64_t>(backoff_seconds) * 1'000'000'000LL,
                    std::memory_order_release);
            });
        });
    }

    void apply_downstream_delivery_feedback() {
        if (!downstream_feedback_pending_) {
            return;
        }
        downstream_feedback_pending_ = false;
        std::vector<delivery_report_feedback::ResolvedOutcomes> reporter_outcomes;
        reporter_outcomes.reserve(downstream_feedback_by_reporter_.size());
        const auto now = std::chrono::steady_clock::now();
        for (auto it = downstream_feedback_by_reporter_.begin();
             it != downstream_feedback_by_reporter_.end();) {
            if (now - it->second.updated_at > DOWNSTREAM_FEEDBACK_RETENTION) {
                it = downstream_feedback_by_reporter_.erase(it);
                continue;
            }
            reporter_outcomes.push_back(it->second.outcomes);
            ++it;
        }
        const auto loss_rate =
            delivery_report_feedback::robust_loss_rate(reporter_outcomes);
        if (!loss_rate.has_value()) {
            return;
        }
        apply_congestion_feedback(
            *loss_rate,
            rtt_ms_.load(std::memory_order_relaxed),
            congestion_policy::FeedbackPath::downstream);
    }

    void send_audio_delivery_reports() {
        if (!join_session_.is_join_confirmed() ||
            !join_session_.server_supports(AUDIO_CAP_DELIVERY_REPORTS) ||
            !join_session_.server_supports(AUDIO_CAP_SECURE_AUDIO)) {
            return;
        }
        const auto key = current_session_key();
        if (key == nullptr) {
            return;
        }
        snapshot_delivery_outcomes();
        for (auto& [sender_id, report]: delivery_report_accumulators_) {
            const bool participant_exists = participant_manager_.exists(sender_id);
            if (!participant_exists ||
                (report.delivered.total == report.last_sent_delivered &&
                 report.lost.total == report.last_sent_lost)) {
                continue;
            }
            AudioDeliveryReportPayload payload{};
            payload.report_epoch = report.report_epoch;
            payload.total_delivered = report.delivered.total;
            payload.total_lost = report.lost.total;

            session_crypto::SecureControlMetadata metadata;
            metadata.sender_id = join_session_.participant_id();
            metadata.target_id = sender_id;
            metadata.sequence =
                secure_control_sequence_.fetch_add(1, std::memory_order_acq_rel);
            metadata.access_epoch = join_session_.access_epoch();
            metadata.plaintext_bytes = sizeof(payload);
            metadata.media_key_commitment =
                join_session_.media_key_commitment();

            std::array<unsigned char, 512> sealed{};
            size_t sealed_bytes = 0;
            if (!session_crypto::seal_control_packet(
                    *key, metadata,
                    reinterpret_cast<const unsigned char*>(&payload), sizeof(payload),
                    sealed.data(), sealed.size(), sealed_bytes)) {
                continue;
            }
            auto packet = std::make_shared<std::vector<unsigned char>>(
                sealed.begin(), sealed.begin() +
                    static_cast<std::ptrdiff_t>(sealed_bytes));
            send(packet->data(), packet->size(), packet);
            report.last_sent_delivered = report.delivered.total;
            report.last_sent_lost = report.lost.total;
        }
    }

    void apply_congestion_feedback(
        double loss_ratio, double rtt_ms,
        congestion_policy::FeedbackPath feedback_path) {
        congestion_policy::State state;
        state.target_bitrate =
            congestion_target_bitrate_.load(std::memory_order_relaxed);
        state.maximum_bitrate =
            congestion_max_bitrate_.load(std::memory_order_relaxed);
        state.redundancy_depth =
            adaptive_redundancy_depth_.load(std::memory_order_relaxed);
        state.healthy_intervals =
            congestion_healthy_intervals_.load(std::memory_order_relaxed);
        const auto next = congestion_policy::update(state, loss_ratio, rtt_ms);
        congestion_target_bitrate_.store(next.target_bitrate, std::memory_order_release);
        congestion_healthy_intervals_.store(next.healthy_intervals,
                                             std::memory_order_release);
        const int previous_depth = adaptive_redundancy_depth_.exchange(
            next.redundancy_depth, std::memory_order_acq_rel);
        if (previous_depth != next.redundancy_depth) {
            request_recent_opus_audio_packets_reset();
        }

        const bool impaired = loss_ratio >= 0.01 || rtt_ms >= 150.0;
        uint16_t desired_frames = opus_network_clock::DEFAULT_FRAME_COUNT;
        {
            std::lock_guard lock(packet_duration_mutex_);
            desired_frames = static_cast<uint16_t>(
                congestion_policy::update_packet_duration(
                    packet_duration_state_, feedback_path, impaired,
                    opus_network_clock::FAST_FRAME_COUNT));
        }
        const uint16_t previous_frames = get_opus_network_frame_count();
        if (desired_frames != previous_frames) {
            apply_opus_network_frame_count(desired_frames);
            if (desired_frames > previous_frames) {
                spdlog::warn(
                    "Congestion controller increased Opus packet duration to {:.1f} ms "
                    "after sustained impairment",
                    opus_network_clock::frame_duration_ms(
                        opus_network_clock::SAMPLE_RATE, desired_frames));
            } else {
                spdlog::info(
                    "Congestion controller restored configured Opus packet duration to "
                    "{:.1f} ms after sustained healthy feedback",
                    opus_network_clock::frame_duration_ms(
                        opus_network_clock::SAMPLE_RATE, desired_frames));
            }
        }
    }

    void handle_audio_path_stats_message(std::size_t bytes, const char* recv_data) {
        if (bytes < sizeof(AudioPathStatsHdr)) {
            return;
        }

        AudioPathStatsHdr stats{};
        std::memcpy(&stats, recv_data, sizeof(AudioPathStatsHdr));
        const uint32_t interval_unrecovered_gaps =
            stats.interval_unrecovered_sequence_gaps;
        audio_path_interval_received_.store(stats.interval_received,
                                            std::memory_order_relaxed);
        audio_path_interval_sequence_gaps_.store(stats.interval_sequence_gaps,
                                                std::memory_order_relaxed);
        audio_path_interval_gaps_.store(interval_unrecovered_gaps,
                                        std::memory_order_relaxed);
        const double gap_rate_percent =
            client_network_path::net_gap_rate(stats.interval_received,
                                              stats.interval_sequence_gaps,
                                              interval_unrecovered_gaps) *
            100.0;
        apply_congestion_feedback(
            gap_rate_percent / 100.0,
            rtt_ms_.load(std::memory_order_relaxed),
            congestion_policy::FeedbackPath::sender_ingress);
        if (stats.interval_sequence_gaps == 0 && interval_unrecovered_gaps == 0) {
            return;
        }

        spdlog::warn(
            "Server reports sender audio ingress loss: received={} seq_gap={} "
            "net_gap={} net_gap_rate={:.1f}% observed_packet={} total_received={} "
            "total_gap={} total_net_gap={}; adaptive_bitrate={} adaptive_redundancy={} "
            "Opus packet={} frames",
            stats.interval_received, stats.interval_sequence_gaps,
            interval_unrecovered_gaps, gap_rate_percent, stats.observed_frame_count,
            stats.total_received, stats.total_sequence_gaps,
            stats.total_unrecovered_sequence_gaps,
            congestion_target_bitrate_.load(std::memory_order_relaxed),
            adaptive_redundancy_depth_.load(std::memory_order_relaxed),
            get_opus_network_frame_count());
        if (client_network_path::should_rebind_after_severe_loss(
                stats.interval_received, interval_unrecovered_gaps)) {
            request_udp_path_rebind("severe sender audio ingress loss");
        }
    }

    void observe_ping_path_timeout(uint32_t sent_sequence) {
        if (!join_session_.is_join_confirmed()) {
            return;
        }

        const uint32_t missing_replies = client_network_path::missing_replies_for_timeout(
            sent_sequence,
            ping_path_watch_start_sequence_.load(std::memory_order_acquire),
            have_ping_reply_sequence_.load(std::memory_order_acquire),
            last_ping_reply_sequence_.load(std::memory_order_acquire));

        ping_path_consecutive_missing_.store(missing_replies,
                                             std::memory_order_relaxed);
        if (missing_replies < client_network_path::PING_TIMEOUT_PROMOTE_REPLIES) {
            return;
        }

        spdlog::warn(
            "Server ping replies are missing for {} consecutive sends; current Opus packet "
            "is {} frames",
            missing_replies, get_opus_network_frame_count());
        request_udp_path_rebind("missing server ping replies");
    }

    void observe_ping_path_feedback(uint32_t reply_sequence, double rtt_ms) {
        if (!client_network_path::ping_reply_is_within_watch_window(
                reply_sequence,
                ping_path_watch_start_sequence_.load(std::memory_order_acquire))) {
            return;
        }

        uint32_t missing_replies = 0;
        if (have_ping_reply_sequence_.exchange(true, std::memory_order_acq_rel)) {
            const uint32_t previous =
                last_ping_reply_sequence_.load(std::memory_order_acquire);
            if (reply_sequence > previous + 1U) {
                missing_replies = reply_sequence - previous - 1U;
            }
        }

        last_ping_reply_sequence_.store(reply_sequence, std::memory_order_release);
        ping_path_total_received_.fetch_add(1, std::memory_order_relaxed);
        ping_path_consecutive_missing_.store(0, std::memory_order_relaxed);
        ping_path_interval_received_.fetch_add(1, std::memory_order_relaxed);
        if (missing_replies > 0) {
            ping_path_total_missing_.fetch_add(missing_replies,
                                               std::memory_order_relaxed);
            ping_path_interval_missing_.fetch_add(missing_replies,
                                                  std::memory_order_relaxed);
        }

        const uint32_t received =
            ping_path_interval_received_.load(std::memory_order_relaxed);
        const uint32_t missing =
            ping_path_interval_missing_.load(std::memory_order_relaxed);
        if (received + missing < client_network_path::PING_FEEDBACK_MIN_REPLIES) {
            return;
        }

        ping_path_interval_received_.store(0, std::memory_order_relaxed);
        ping_path_interval_missing_.store(0, std::memory_order_relaxed);
        const double gap_rate_percent =
            client_network_path::gap_rate(received, missing) * 100.0;
        apply_congestion_feedback(
            gap_rate_percent / 100.0, rtt_ms,
            congestion_policy::FeedbackPath::ping);
        if (missing == 0 && rtt_ms < client_network_path::HIGH_RTT_MS) {
            return;
        }

        spdlog::warn(
            "Server ping path is unstable: replies={} missing={} gap_rate={:.1f}% "
            "rtt_ms={:.1f}; adaptive_bitrate={} adaptive_redundancy={} Opus packet={} frames",
            received, missing, gap_rate_percent, rtt_ms,
            congestion_target_bitrate_.load(std::memory_order_relaxed),
            adaptive_redundancy_depth_.load(std::memory_order_relaxed),
            get_opus_network_frame_count());
        if (client_network_path::should_rebind_after_ping_feedback(
                received, missing, rtt_ms)) {
            request_udp_path_rebind("severe server ping path loss");
        }
    }

    void handle_ping_message(std::size_t bytes, const char* recv_data) {
        // Add to total bytes received
        total_bytes_rx_.fetch_add(bytes, std::memory_order_relaxed);

        SyncHdr hdr{};
        std::memcpy(&hdr, recv_data, sizeof(SyncHdr));

        auto now = std::chrono::steady_clock::now();
        auto current_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        auto rtt = (current_time - hdr.t1_client_send) - (hdr.t3_server_send - hdr.t2_server_recv);
        auto offset =
            ((hdr.t2_server_recv - hdr.t1_client_send) + (hdr.t3_server_send - current_time)) /
            2;

        double rtt_ms = static_cast<double>(rtt) / 1e6;

        // Store RTT for GUI display (thread-safe atomic update)
        rtt_ms_.store(rtt_ms, std::memory_order_relaxed);
        const int64_t rtt_ns = std::max<int64_t>(0, rtt);
        observe_latency_sample(rtt_last_ns_, rtt_avg_ns_, rtt_max_ns_, rtt_ns);
        int64_t previous_min = rtt_min_ns_.load(std::memory_order_relaxed);
        while ((previous_min == 0 || rtt_ns < previous_min) &&
               !rtt_min_ns_.compare_exchange_weak(previous_min, rtt_ns,
                                                  std::memory_order_relaxed)) {
        }
        observe_ping_path_feedback(hdr.seq, rtt_ms);
        clock_offset_estimator::Estimate clock_estimate;
        {
            std::lock_guard lock(server_clock_estimator_mutex_);
            clock_estimate = server_clock_estimator_.observe(rtt_ns, offset);
        }
        if (clock_estimate.accepted) {
            server_clock_offset_ns_.store(clock_estimate.offset_ns,
                                          std::memory_order_release);
            server_clock_uncertainty_ns_.store(clock_estimate.uncertainty_ns,
                                               std::memory_order_release);
            server_clock_ready_.store(clock_estimate.ready, std::memory_order_release);
        }

        // print live stats
        // spdlog::debug("seq {} RTT {:.5f} ms | offset {:.5f} ms", hdr.seq, rtt_ms, offset_ms);
    }

    void handle_secure_control_message(std::size_t bytes, const char* recv_data) {
        const auto key = current_session_key();
        if (key == nullptr) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::array<unsigned char, 512> plaintext{};
        session_crypto::SecureControlMetadata metadata;
        size_t plaintext_bytes = 0;
        if (!session_crypto::open_control_packet(
                *key, reinterpret_cast<const unsigned char*>(recv_data), bytes,
                metadata, plaintext.data(), plaintext.size(), plaintext_bytes)) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn("Dropping secure control with invalid auth tag (drops={})",
                             count);
            }
            return;
        }
        if (!accept_secure_control_sequence(metadata.sender_id, metadata.sequence)) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn("Dropping replayed secure control sender={} seq={} drops={}",
                             metadata.sender_id, metadata.sequence, count);
            }
            return;
        }
        if (plaintext_bytes == 0) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const uint8_t command = plaintext[0];
        if (command == SECURE_CONTROL_AUDIO_DELIVERY_REPORT) {
            if (metadata.target_id != join_session_.participant_id() ||
                metadata.sender_id == metadata.target_id ||
                plaintext_bytes != sizeof(AudioDeliveryReportPayload)) {
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            AudioDeliveryReportPayload report{};
            std::memcpy(&report, plaintext.data(), sizeof(report));
            if (report.reserved[0] != 0 || report.reserved[1] != 0 ||
                report.reserved[2] != 0) {
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (report.total_delivered > UINT64_MAX - report.total_lost) {
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            DownstreamDeliveryReporter candidate;
            if (const auto previous =
                    downstream_delivery_by_reporter_.find(metadata.sender_id);
                previous != downstream_delivery_by_reporter_.end()) {
                candidate = previous->second;
            }
            const bool first_report = !candidate.report.initialized;
            const bool new_epoch = candidate.report.initialized &&
                                   candidate.report.report_epoch != report.report_epoch;
            const uint64_t generated =
                audio_packets_generated_.load(std::memory_order_acquire) -
                delivery_report_generated_baseline_;
            if (first_report &&
                generated > delivery_report_feedback::MAX_INTERVAL_PACKETS) {
                candidate.claimed_outcomes =
                    generated - delivery_report_feedback::MAX_INTERVAL_PACKETS;
            }
            const auto delta = delivery_report_feedback::observe(
                candidate.report, report.report_epoch, metadata.sequence,
                report.total_delivered, report.total_lost);
            if (delta.status == delivery_report_feedback::Delta::Status::Stale) {
                return;
            }
            if (delta.status == delivery_report_feedback::Delta::Status::Implausible) {
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const uint64_t newly_claimed = first_report || new_epoch
                                               ? report.total_delivered + report.total_lost
                                               : delta.delivered + delta.lost;
            if (!delivery_report_feedback::claimed_outcomes_plausible(
                    candidate.claimed_outcomes, newly_claimed, generated)) {
                if (delta.status !=
                    delivery_report_feedback::Delta::Status::Rebaseline) {
                    inbound_malformed_audio_drops_.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }
                candidate.claimed_outcomes = generated;
            } else {
                candidate.claimed_outcomes += newly_claimed;
            }
            downstream_delivery_by_reporter_[metadata.sender_id] = candidate;
            downstream_delivery_last_activity_[metadata.sender_id] =
                std::chrono::steady_clock::now();
            downstream_delivery_tombstone_since_.erase(metadata.sender_id);
            if (delta.status ==
                delivery_report_feedback::Delta::Status::Rebaseline) {
                return;
            }
            downstream_feedback_by_reporter_[metadata.sender_id] = {
                {delta.delivered, delta.lost}, std::chrono::steady_clock::now()};
            downstream_feedback_pending_ = true;
            return;
        }

        if (command != SECURE_CONTROL_ROTATE_MEDIA_KEY || metadata.target_id != 0 ||
            plaintext_bytes != sizeof(MediaKeyRotationPayload)) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        MediaKeyRotationPayload payload{};
        std::memcpy(&payload, plaintext.data(), sizeof(payload));
        if (payload.command != SECURE_CONTROL_ROTATE_MEDIA_KEY ||
            payload.reserved8 != 0 ||
            payload.access_epoch == 0 ||
            payload.access_epoch != metadata.access_epoch ||
            payload.media_secret_bytes == 0 ||
            payload.media_secret_bytes > payload.media_secret.size()) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const std::string media_secret(
            payload.media_secret.begin(),
            payload.media_secret.begin() + payload.media_secret_bytes);
        if (apply_media_secret_rotation(media_secret, payload.access_epoch,
                                        metadata.media_key_commitment)) {
            spdlog::info("Applied encrypted E2E media key rotation from participant {}",
                         metadata.sender_id);
        }
    }

    void handle_e2e_key_envelope(std::size_t bytes, const char* recv_data) {
        if (!e2e_keypair_ready_ || recv_data == nullptr ||
            bytes < sizeof(E2EKeyEnvelopeHdr)) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        E2EKeyEnvelopeHdr envelope{};
        std::memcpy(&envelope, recv_data, sizeof(envelope));
        if (envelope.magic != E2E_KEY_ENVELOPE_MAGIC ||
            envelope.target_id != join_session_.participant_id() ||
            envelope.sender_id == 0 ||
            envelope.access_epoch == 0 ||
            !performer_join_token::valid_sha256_hex(std::string(
                envelope.media_key_commitment.begin(),
                envelope.media_key_commitment.end())) ||
            envelope.encrypted_bytes <= crypto_box_SEALBYTES ||
            envelope.encrypted_bytes > E2E_KEY_ENVELOPE_MAX_BYTES ||
            envelope.reserved != 0) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::array<unsigned char, sizeof(E2EKeyEnvelopePayload)> plaintext{};
        size_t plaintext_bytes = 0;
        if (!session_crypto::open_key_envelope(
                e2e_public_key_, e2e_secret_key_,
                reinterpret_cast<const unsigned char*>(envelope.encrypted.data()),
                envelope.encrypted_bytes, plaintext.data(), plaintext.size(),
                plaintext_bytes) ||
            plaintext_bytes != sizeof(E2EKeyEnvelopePayload)) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn("Dropping E2E key envelope with invalid seal (drops={})",
                             count);
            }
            return;
        }

        E2EKeyEnvelopePayload payload{};
        std::memcpy(&payload, plaintext.data(), sizeof(payload));
        if (payload.media_secret_bytes == 0 ||
            payload.reserved != 0 ||
            payload.access_epoch == 0 ||
            payload.access_epoch != envelope.access_epoch ||
            payload.media_secret_bytes > payload.media_secret.size()) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const std::string media_secret(
            payload.media_secret.begin(),
            payload.media_secret.begin() + payload.media_secret_bytes);
        const std::string media_key_commitment(
            envelope.media_key_commitment.begin(),
            envelope.media_key_commitment.end());
        const bool applied = apply_media_secret_rotation(
            media_secret, payload.access_epoch, media_key_commitment);
        const bool already_applied =
            payload.access_epoch == join_session_.access_epoch() &&
            media_key_commitment == join_session_.media_key_commitment() &&
            media_secret == join_session_.media_secret();
        if (applied) {
            spdlog::info("Received encrypted room key from participant {}",
                         envelope.sender_id);
        }
        if (applied || already_applied) {
            send_key_handoff_ack_on_io(envelope.sender_id,
                                       payload.access_epoch,
                                       media_key_commitment);
        }
    }

    void handle_secure_audio_message(std::size_t bytes, const char* recv_data) {
        const auto key = current_session_key();
        if (key == nullptr) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::array<unsigned char, 2048> plaintext{};
        session_crypto::SecureAudioMetadata metadata;
        size_t plaintext_bytes = 0;
        if (!session_crypto::open_audio_packet(
                *key, reinterpret_cast<const unsigned char*>(recv_data), bytes,
                metadata, plaintext.data(), plaintext.size(), plaintext_bytes)) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn("Dropping secure audio with invalid auth tag (drops={})", count);
            }
            return;
        }

        bool metadata_matches = false;
        MsgHdr plaintext_magic{};
        if (plaintext_bytes >= sizeof(MsgHdr)) {
            std::memcpy(&plaintext_magic, plaintext.data(), sizeof(plaintext_magic));
        }
        auto metadata_matches_child = [&](const unsigned char* child, size_t child_len) {
            const auto parsed = audio_packet::parse_audio_header(child, child_len);
            return parsed.valid && parsed.sender_id == metadata.sender_id &&
                   parsed.sequence == metadata.sequence &&
                   parsed.sample_rate == metadata.sample_rate &&
                   parsed.frame_count == metadata.frame_count &&
                   parsed.channels == metadata.channels && parsed.codec == metadata.codec &&
                   parsed.capture_server_time_ns == metadata.capture_server_time_ns;
        };
        if (plaintext_magic.magic == AUDIO_V3_MAGIC) {
            metadata_matches = metadata_matches_child(plaintext.data(), plaintext_bytes);
        } else if (plaintext_magic.magic == AUDIO_REDUNDANT_MAGIC) {
            std::string reason;
            audio_packet::for_each_redundant_audio_child(
                plaintext.data(), plaintext_bytes,
                [&](const unsigned char* child, size_t child_len, uint8_t index) {
                    if (index == 0) {
                        metadata_matches = metadata_matches_child(child, child_len);
                    }
                },
                &reason);
        }
        if (!metadata_matches) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn("Dropping secure audio with mismatched metadata (drops={})",
                             count);
            }
            return;
        }

        handle_audio_message(plaintext_bytes,
                             reinterpret_cast<const char*>(plaintext.data()));
    }

    void handle_audio_message(std::size_t bytes, const char* recv_data,
                              bool count_duplicate_late = true) {
        MsgHdr msg_hdr{};
        std::memcpy(&msg_hdr, recv_data, sizeof(MsgHdr));
        if (msg_hdr.magic == AUDIO_REDUNDANT_MAGIC) {
            handle_redundant_audio_message(bytes, recv_data);
            return;
        }

        if (msg_hdr.magic != AUDIO_V3_MAGIC) {
            inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto parsed_audio = audio_packet::parse_audio_header(
            reinterpret_cast<const unsigned char*>(recv_data), bytes);
        const size_t min_packet_size = audio_packet::v3_header_size();

        if (!message_validator::is_valid_audio_packet(bytes, min_packet_size)) {
            return;
        }

        const auto* packet_bytes = reinterpret_cast<const unsigned char*>(recv_data);
        uint32_t sender_id = parsed_audio.sender_id;
        uint16_t payload_bytes = parsed_audio.payload_bytes;

        size_t expected_size = min_packet_size + payload_bytes;
        if (!message_validator::has_complete_payload(bytes, expected_size, 0)) {
            spdlog::error("Incomplete audio packet: got {}, expected {} (payload_bytes={})", bytes,
                       expected_size, payload_bytes);
            return;
        }

        // Additional safety check: ensure encoded_bytes is reasonable
        if (!message_validator::is_encoded_bytes_valid(payload_bytes, AUDIO_BUF_SIZE)) {
            spdlog::error("Invalid audio packet: payload_bytes {} exceeds max {}", payload_bytes,
                       AUDIO_BUF_SIZE);
            return;
        }

        std::string reason;
        if (!audio_packet::validate_audio_packet_bytes(packet_bytes, bytes, &reason)) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn(
                    "Dropping invalid audio: reason={} sender={} seq={} "
                    "sample_rate={} frame_count={} channels={} payload_bytes={} drops={}",
                    reason, sender_id, parsed_audio.sequence, parsed_audio.sample_rate,
                    parsed_audio.frame_count, static_cast<int>(parsed_audio.channels),
                    parsed_audio.payload_bytes, count);
            }
            return;
        }

        // Register participant if not known
        if (!participant_manager_.exists(sender_id)) {
            // Validate runtime audio shape before creating participant decoder state.
            const int decoder_sample_rate = static_cast<int>(parsed_audio.sample_rate);
            const int decoder_channels = static_cast<int>(parsed_audio.channels);
            if (decoder_sample_rate == 0 || current_audio_frames_per_buffer() == 0 ||
                decoder_channels == 0) {
                spdlog::error(
                    "Cannot create decoder for participant {}: audio config not initialized "
                    "(sample_rate={}, frames_per_buffer={}, channels={})",
                    sender_id, decoder_sample_rate, current_audio_frames_per_buffer(),
                    decoder_channels);
                return;
            }

            if (!participant_manager_.register_participant(sender_id, decoder_sample_rate,
                                                           decoder_channels)) {
                return;
            }
            participant_manager_.with_participant(
                sender_id, [this](ParticipantData& participant) {
                    apply_default_opus_jitter_policy(participant);
                });
        }

        // Add to total bytes received
        total_bytes_rx_.fetch_add(bytes, std::memory_order_relaxed);

        const unsigned char* audio_data = packet_builder::audio_payload(packet_bytes, bytes);
        if (audio_data == nullptr) {
            return;
        }

        // CRITICAL: Enqueue audio packet, DON'T decode here
        // Decoding happens in time-driven audio_callback
        participant_manager_.with_participant(sender_id, [&](ParticipantData& participant) {
            OpusPacket packet;
            // Use memcpy for zero-allocation copy (fixed buffer)
            if (payload_bytes <= AUDIO_BUF_SIZE) {
                std::memcpy(packet.data.data(), audio_data, payload_bytes);
                packet.size      = payload_bytes;
                packet.timestamp = std::chrono::steady_clock::now();
                packet.codec       = parsed_audio.codec;
                packet.sequence    = parsed_audio.sequence;
                packet.sequence_valid = true;
                packet.sample_rate = parsed_audio.sample_rate;
                packet.frame_count = parsed_audio.frame_count;
                packet.channels    = parsed_audio.channels;
                packet.capture_timestamp_valid =
                    parsed_audio.capture_timestamp_valid;
                packet.capture_server_time_ns =
                    parsed_audio.capture_server_time_ns;
                const auto sequence_delta =
                    participant.sequence_tracker.record(packet.sequence);
                auto [delivery_it, delivery_inserted] =
                    delivery_report_accumulators_.try_emplace(sender_id);
                auto& delivery_report = delivery_it->second;
                if (delivery_inserted) {
                    delivery_report.report_epoch = next_delivery_report_epoch();
                }
                delivery_report.inactive_since = {};
                if (sequence_delta.gaps_detected > 0) {
                    participant.sequence_gaps.fetch_add(
                        sequence_delta.gaps_detected,
                        std::memory_order_relaxed);
                }
                if (sequence_delta.gaps_recovered > 0) {
                    participant.sequence_gap_recoveries.fetch_add(
                        sequence_delta.gaps_recovered,
                        std::memory_order_relaxed);
                }
                const uint64_t unresolved_gaps =
                    participant.sequence_tracker.unresolved_gaps();
                participant.sequence_unresolved_gaps.store(unresolved_gaps,
                                                           std::memory_order_relaxed);
                if (sequence_delta.late_or_duplicate &&
                    (count_duplicate_late || sequence_delta.gaps_recovered > 0)) {
                    participant.sequence_late_or_reordered.fetch_add(
                        1, std::memory_order_relaxed);
                }
                if (!sequence_arrival_should_enqueue(sequence_delta)) {
                    return;
                }
            } else {
                spdlog::error("Packet too large: {} bytes (max {})", payload_bytes, AUDIO_BUF_SIZE);
                return;
            }

            size_t queue_size = participant.opus_queue.size_approx();
            observe_participant_queue_depth(participant, queue_size);
            const size_t previous_packet_frame_count =
                participant.last_packet_frame_count.load(std::memory_order_relaxed);
            update_jitter_floor(participant, packet);
            observe_receiver_clock_drift(participant, packet);
            participant.last_packet_frame_count.store(packet.frame_count,
                                                       std::memory_order_relaxed);

            if (delivery_stall_policy::record_arrival(
                    participant.delivery_stall_burst, participant.last_packet_time,
                    packet.timestamp, previous_packet_frame_count != 0,
                    opus_playout_target_queue_packets(participant))) {
                participant.delivery_stall_flush_requested.store(
                    true, std::memory_order_release);
            }
            participant.last_packet_time = packet.timestamp;

            // Bounded jitter management: preserve sequenced playout order under overflow.
            const size_t configured_queue_limit =
                std::max(get_opus_queue_limit_packets(),
                         opus_playout_target_queue_packets(participant) + 3);
            const size_t max_queue_packets =
                max_receive_queue_packets(packet, configured_queue_limit);
            participant.opus_queue_limit_packets.store(max_queue_packets,
                                                       std::memory_order_relaxed);
            if (!participant.opus_queue.enqueue_bounded_or_reject_overflow(packet,
                                                                           max_queue_packets)) {
                participant.jitter_depth_drops.fetch_add(1, std::memory_order_relaxed);
                participant.opus_queue_limit_drops.fetch_add(1, std::memory_order_relaxed);
                if (!participant.is_muted.load(std::memory_order_relaxed)) {
                    (void)participant.opus_queue.record_receiver_drop(packet);
                }
                return;
            }

            size_t queue_after_enqueue = participant.opus_queue.size_approx();
            observe_participant_queue_depth(participant, queue_after_enqueue);
            // Mark buffer as ready once we have enough packets
            if (!participant.buffer_ready.load(std::memory_order_relaxed) &&
                queue_after_enqueue >= ready_threshold_packets(participant)) {
                participant.buffer_ready.store(true, std::memory_order_relaxed);
                participant.opus_consecutive_empty_callbacks.store(0,
                                                                   std::memory_order_relaxed);
                spdlog::info("Jitter buffer ready for participant {} ({} packets)", sender_id,
                          queue_after_enqueue);
            }
        });
    }

    void handle_redundant_audio_message(std::size_t bytes, const char* recv_data) {
        const auto* packet_bytes = reinterpret_cast<const unsigned char*>(recv_data);
        std::string reason;
        if (!audio_packet::validate_redundant_audio_packet_bytes(packet_bytes, bytes, &reason)) {
            const uint64_t count =
                inbound_malformed_audio_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 100 == 0) {
                spdlog::warn("Dropping invalid inbound redundant audio: reason={} bytes={} drops={}",
                          reason, bytes, count);
            }
            return;
        }

        audio_packet::for_each_redundant_audio_child_reverse(
            packet_bytes, bytes,
            [&](const unsigned char* child, size_t child_len, uint8_t index) {
                handle_audio_message(child_len, reinterpret_cast<const char*>(child),
                                     index == 0);
            });
    }

    void schedule_metronome_sync(const MetronomeSyncHdr& sync) {
        const bool clock_ready = server_clock_ready_.load(std::memory_order_acquire);
        const int64_t effective_ns =
            sync.effective_server_time_ns > 0 && clock_ready
                ? sync.effective_server_time_ns
                : steady_now_ns() + server_clock_offset_ns_.load(std::memory_order_acquire) +
                      150'000'000LL;
        metronome_.schedule_sync(sync, effective_ns);
    }

    void mix_metronome_click(float* output_buffer, unsigned long frame_count, size_t out_channels) {
        metronome_.mix_click(output_buffer, frame_count, out_channels,
                             current_audio_sample_rate(), steady_now_ns(),
                             server_clock_offset_ns_.load(std::memory_order_acquire));
    }

    void record_mono_block(RecordingWriter::TrackKind kind, uint32_t participant_id,
                           const float* samples, size_t frame_count) {
        media_state_.record_mono_block(kind, participant_id,
                                       static_cast<uint32_t>(current_audio_sample_rate()),
                                       samples, frame_count);
    }

    void record_master_mix(const float* output_buffer, unsigned long frame_count,
                           size_t out_channels) {
        media_state_.record_master_mix(static_cast<uint32_t>(current_audio_sample_rate()),
                                       output_buffer, frame_count, out_channels);
    }

    static int audio_callback(const void* input, void* output, unsigned long frame_count,
                              void* user_data, AudioCallbackWorkBudget& work_budget) {
        const auto* input_buffer  = static_cast<const float*>(input);
        auto*       output_buffer = static_cast<float*>(output);
        auto*       client        = static_cast<Client*>(user_data);
        const int   runtime_sample_rate = client->current_audio_sample_rate();
        const auto callback_start = std::chrono::steady_clock::now();
        struct TimingScope {
            Client* client;
            std::chrono::steady_clock::time_point start;
            unsigned long frame_count;
            int sample_rate;

            ~TimingScope() {
                auto elapsed = std::chrono::steady_clock::now() - start;
                auto elapsed_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
                auto deadline_ns = static_cast<int64_t>(
                    (static_cast<double>(frame_count) * 1e9) /
                    static_cast<double>(std::max(1, sample_rate)));

                client->callback_last_ns_.store(elapsed_ns, std::memory_order_relaxed);
                client->callback_deadline_ns_.store(deadline_ns, std::memory_order_relaxed);
                client->callback_count_.fetch_add(1, std::memory_order_relaxed);

                int64_t previous_max = client->callback_max_ns_.load(std::memory_order_relaxed);
                while (elapsed_ns > previous_max &&
                       !client->callback_max_ns_.compare_exchange_weak(
                           previous_max, elapsed_ns, std::memory_order_relaxed)) {
                }

                int64_t previous_avg = client->callback_avg_ns_.load(std::memory_order_relaxed);
                int64_t next_avg = previous_avg == 0 ? elapsed_ns : ((previous_avg * 31) + elapsed_ns) / 32;
                client->callback_avg_ns_.store(next_avg, std::memory_order_relaxed);

                if (elapsed_ns > deadline_ns) {
                    client->callback_over_deadline_count_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } timing_scope{client, callback_start, frame_count, runtime_sample_rate};

        ParticipantManager::AudioCallbackReadScope participant_snapshot_scope;

#ifdef _WIN32
        struct CallbackThreadPriority {
            HANDLE handle = nullptr;
            CallbackThreadPriority() {
                DWORD task_index = 0;
                handle = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
                if (handle != nullptr) {
                    AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL);
                } else {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                }
            }
            ~CallbackThreadPriority() {
                if (handle != nullptr) {
                    AvRevertMmThreadCharacteristics(handle);
                }
            }
        };
        thread_local CallbackThreadPriority callback_priority;
        (void)callback_priority;
#endif

        if (output_buffer == nullptr) {
            return 0;
        }

        const size_t out_channels  = client->audio_.get_output_channel_count();
        const size_t bytes_to_copy = frame_count * out_channels * sizeof(float);

        // Initialize output buffer to silence
        std::memset(output_buffer, 0, bytes_to_copy);

        // Mix audio from all active participants (thread-safe iteration)
        int active_count = 0;
        // Bound burst work even when every participant arrives as 2.5 ms Opus
        // packets and the device asks for a 20 ms callback.
        const size_t& remaining_decode_budget = work_budget.remaining_decodes;
        const auto playout_start = std::chrono::steady_clock::now();
        client->participant_manager_.for_each([&](uint32_t         participant_id,
                                                  ParticipantData& participant) {
            observe_opus_pcm_depth(participant);
            participant.last_callback_frame_count.store(frame_count, std::memory_order_relaxed);
            if (participant.is_muted.load(std::memory_order_relaxed)) {
                participant.opus_queue.discard_all_for_local_mute();
                participant.opus_pcm_buffered_frames = 0;
                clear_opus_capture_chunks(participant);
                participant.opus_resample_phase = 0.0;
                participant.buffer_ready.store(false, std::memory_order_relaxed);
                participant.opus_consecutive_empty_callbacks.store(
                    0, std::memory_order_relaxed);
                participant.current_level.store(0.0F, std::memory_order_relaxed);
                participant.current_peak.store(0.0F, std::memory_order_relaxed);
                participant.is_speaking.store(false, std::memory_order_relaxed);
                observe_participant_queue_depth(participant,
                                                participant.opus_queue.size_approx());
                observe_opus_pcm_depth(participant);
                return;
            }

            participant.receiver_delivery_drops.fetch_add(
                participant.opus_queue.take_internal_receiver_drops(),
                std::memory_order_relaxed);

            observe_participant_queue_depth(
                participant, flush_stalled_delivery_backlog(participant));

            if (!participant.buffer_ready.load(std::memory_order_relaxed)) {
                const size_t queue_size = participant.opus_queue.size_approx();
                observe_participant_queue_depth(participant, queue_size);
                if (queue_size >= ready_threshold_packets(participant)) {
                    participant.buffer_ready.store(true, std::memory_order_relaxed);
                    participant.opus_consecutive_empty_callbacks.store(0,
                                                                       std::memory_order_relaxed);
                } else {
                    return;
                }
            }

            if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus) {
                observe_participant_queue_depth(
                    participant, trim_opus_queue_to_latency_target(participant));
            }

            const float participant_gain = participant.gain.load(std::memory_order_relaxed);
            const float participant_pan = participant.pan.load(std::memory_order_relaxed);
            double playout_ratio = opus_playout_rate_ratio(participant);
            if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus &&
                participant.opus_pcm_buffered_frames >=
                    opus_resample_required_input_frames(
                        participant, frame_count, playout_ratio)) {
                const auto playout_server_time_ns =
                    client->server_time_for_steady_time_ns_if_ready(playout_start);
                const size_t consumed_frames = mix_resampled_opus_pcm(
                    participant, output_buffer, frame_count, out_channels,
                    participant_gain, participant_pan, playout_ratio);
                observe_and_consume_opus_capture_chunks(participant, consumed_frames,
                                                        playout_server_time_ns);
                observe_opus_pcm_depth(participant);
                observe_auto_jitter_stable(
                    participant, client->get_jitter_packet_age_limit_ms());
                participant.opus_consecutive_empty_callbacks.store(0,
                                                                   std::memory_order_relaxed);
                active_count++;
                observe_participant_queue_depth(participant, participant.opus_queue.size_approx());
                return;
            }

            if (remaining_decode_budget == 0) {
                client->rt_diag_decode_budget_exhaustions_.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }

            OpusPacket opus_packet;
            const size_t playout_gap_wait_packets =
                opus_gap_wait_dequeue_attempts(participant);
            auto dequeue_status =
                participant.opus_queue.dequeue(opus_packet, playout_gap_wait_packets);

            if (dequeue_status == ParticipantOpusDequeueStatus::Packet) {
                if (opus_packet.abandoned_receiver_drop_packets > 0) {
                    participant.receiver_delivery_drops.fetch_add(
                        opus_packet.abandoned_receiver_drop_packets,
                        std::memory_order_relaxed);
                }
                if (opus_packet.abandoned_gap_packets > 0) {
                    participant.sequence_gaps_declared_lost.fetch_add(
                        opus_packet.abandoned_gap_packets,
                        std::memory_order_relaxed);
                }
                auto now = std::chrono::steady_clock::now();
                auto packet_age = now - opus_packet.timestamp;
                auto packet_age_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(packet_age).count();
                while (jitter_packet_age_exceeds_limit(
                    packet_age_ns, client->get_jitter_packet_age_limit_ms())) {
                    observe_opus_age_limit_drop(participant);
                    dequeue_status =
                        participant.opus_queue.dequeue(opus_packet, playout_gap_wait_packets);
                    if (dequeue_status != ParticipantOpusDequeueStatus::Packet) {
                        if (dequeue_status == ParticipantOpusDequeueStatus::Empty) {
                            participant.underrun_count.fetch_add(1,
                                                                  std::memory_order_relaxed);
                        }
                        return;
                    }
                    now = std::chrono::steady_clock::now();
                    packet_age = now - opus_packet.timestamp;
                    packet_age_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(packet_age).count();
                }

                if (!consume_audio_decode_budget(work_budget)) {
                    client->rt_diag_decode_budget_exhaustions_.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }

                participant.packet_age_last_ns.store(packet_age_ns, std::memory_order_relaxed);
                int64_t previous_age_max =
                    participant.packet_age_max_ns.load(std::memory_order_relaxed);
                while (packet_age_ns > previous_age_max &&
                       !participant.packet_age_max_ns.compare_exchange_weak(
                           previous_age_max, packet_age_ns, std::memory_order_relaxed)) {
                }
                int64_t previous_age_avg =
                    participant.packet_age_avg_ns.load(std::memory_order_relaxed);
                int64_t next_age_avg =
                    previous_age_avg == 0 ? packet_age_ns : ((previous_age_avg * 31) + packet_age_ns) / 32;
                participant.packet_age_avg_ns.store(next_age_avg, std::memory_order_relaxed);

                participant.last_codec.store(opus_packet.codec, std::memory_order_relaxed);
                int decoded_samples = 0;
                if (opus_packet.reset_decoder && opus_packet.codec == AudioCodec::Opus) {
                    participant.receiver_delivery_drops.fetch_add(
                        discard_received_opus_capture_chunks(participant),
                        std::memory_order_relaxed);
                    participant.decoder->reset();
                    participant.opus_pcm_buffered_frames = 0;
                    participant.opus_resample_phase = 0.0;
                    observe_auto_jitter_instability(
                        participant, client->get_jitter_packet_age_limit_ms());
                }
                if (opus_packet.loss_concealment) {
                    if (opus_packet.receiver_drop_pending) {
                        participant.receiver_delivery_drops.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        participant.sequence_gaps_declared_lost.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    const int decode_frame_count =
                        opus_packet.frame_count > 0 ? static_cast<int>(opus_packet.frame_count)
                                                    : static_cast<int>(frame_count);
                    const auto decode_start = std::chrono::steady_clock::now();
                    decoded_samples = participant.decoder->decode_plc(
                        participant.pcm_buffer.data(), decode_frame_count);
                    client->observe_rx_decode_time(std::chrono::steady_clock::now() -
                                                   decode_start);
                    if (decoded_samples > 0) {
                        participant.plc_count.fetch_add(1, std::memory_order_relaxed);
                        observe_auto_jitter_instability(
                            participant, client->get_jitter_packet_age_limit_ms());
                    }
                } else {
                    const int decode_frame_count =
                        opus_packet.frame_count > 0 ? static_cast<int>(opus_packet.frame_count)
                                                    : static_cast<int>(frame_count);
                    // Decode into preallocated buffer (zero allocations)
                    const auto decode_start = std::chrono::steady_clock::now();
                    decoded_samples = participant.decoder->decode_into(
                        opus_packet.get_data(), static_cast<int>(opus_packet.get_size()),
                        participant.pcm_buffer.data(), decode_frame_count);
                    client->observe_rx_decode_time(std::chrono::steady_clock::now() -
                                                   decode_start);
                }

                if (decoded_samples <= 0) {
                    // Decode failed - use silence
                    client->rt_diag_decode_failures_.fetch_add(1, std::memory_order_relaxed);
                    if (!opus_packet.loss_concealment) {
                        participant.receiver_delivery_drops.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    observe_auto_jitter_instability(
                        participant, client->get_jitter_packet_age_limit_ms());
                    return;
                }

                if (static_cast<size_t>(decoded_samples) <=
                    RecordingWriter::MAX_FRAMES_PER_BLOCK) {
                    client->record_mono_block(RecordingWriter::TrackKind::Participant,
                                              participant_id, participant.pcm_buffer.data(),
                                              static_cast<size_t>(decoded_samples));
                }

                {
                    const size_t decoded_frames = static_cast<size_t>(decoded_samples);
                    if (participant.opus_pcm_buffered_frames + decoded_frames <=
                        participant.opus_pcm_buffer.size()) {
                        std::copy_n(participant.pcm_buffer.begin(), decoded_frames,
                                    participant.opus_pcm_buffer.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            participant.opus_pcm_buffered_frames));
                        participant.opus_pcm_buffered_frames += decoded_frames;
                        append_opus_capture_chunk(
                            participant, decoded_frames,
                            opus_packet.capture_server_time_ns,
                            opus_packet.capture_timestamp_valid,
                            !opus_packet.loss_concealment);
                        participant.opus_packets_decoded_in_callback.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        const uint64_t dropped_packets =
                            discard_received_opus_capture_chunks(participant) +
                            (opus_packet.loss_concealment ? 0 : 1);
                        participant.opus_pcm_buffered_frames = 0;
                        participant.jitter_depth_drops.fetch_add(1, std::memory_order_relaxed);
                        participant.opus_decode_buffer_overflow_drops.fetch_add(
                            1, std::memory_order_relaxed);
                        participant.receiver_delivery_drops.fetch_add(
                            dropped_packets, std::memory_order_relaxed);
                    }

                    double playout_ratio = opus_playout_rate_ratio(participant);
                    size_t required_input_frames = opus_resample_required_input_frames(
                        participant, frame_count, playout_ratio);
                    while (participant.opus_pcm_buffered_frames < required_input_frames &&
                           remaining_decode_budget > 0) {
                        OpusPacket next_packet;
                        if (!participant.opus_queue.try_dequeue(next_packet,
                                                                playout_gap_wait_packets)) {
                            break;
                        }
                        if (next_packet.abandoned_receiver_drop_packets > 0) {
                            participant.receiver_delivery_drops.fetch_add(
                                next_packet.abandoned_receiver_drop_packets,
                                std::memory_order_relaxed);
                        }
                        if (next_packet.abandoned_gap_packets > 0) {
                            participant.sequence_gaps_declared_lost.fetch_add(
                                next_packet.abandoned_gap_packets,
                                std::memory_order_relaxed);
                        }
                        if (!consume_audio_decode_budget(work_budget)) {
                            client->rt_diag_decode_budget_exhaustions_.fetch_add(
                                1, std::memory_order_relaxed);
                            break;
                        }

                        const int next_decode_frame_count =
                            next_packet.frame_count > 0
                                ? static_cast<int>(next_packet.frame_count)
                                : static_cast<int>(frame_count);
                        const auto next_decode_start = std::chrono::steady_clock::now();
                        int next_decoded_samples = 0;
                        if (next_packet.reset_decoder) {
                            participant.receiver_delivery_drops.fetch_add(
                                discard_received_opus_capture_chunks(participant),
                                std::memory_order_relaxed);
                            participant.decoder->reset();
                            participant.opus_pcm_buffered_frames = 0;
                            participant.opus_resample_phase = 0.0;
                            observe_auto_jitter_instability(
                                participant, client->get_jitter_packet_age_limit_ms());
                        }
                        if (next_packet.loss_concealment) {
                            if (next_packet.receiver_drop_pending) {
                                participant.receiver_delivery_drops.fetch_add(
                                    1, std::memory_order_relaxed);
                            } else {
                                participant.sequence_gaps_declared_lost.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            next_decoded_samples = participant.decoder->decode_plc(
                                participant.pcm_buffer.data(), next_decode_frame_count);
                            if (next_decoded_samples > 0) {
                                participant.plc_count.fetch_add(1, std::memory_order_relaxed);
                                observe_auto_jitter_instability(
                                    participant, client->get_jitter_packet_age_limit_ms());
                            }
                        } else {
                            next_decoded_samples = participant.decoder->decode_into(
                                next_packet.get_data(),
                                static_cast<int>(next_packet.get_size()),
                                participant.pcm_buffer.data(), next_decode_frame_count);
                        }
                        client->observe_rx_decode_time(std::chrono::steady_clock::now() -
                                                       next_decode_start);
                        if (next_decoded_samples <= 0) {
                            if (!next_packet.loss_concealment) {
                                participant.receiver_delivery_drops.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                            break;
                        }

                        const size_t next_decoded_frames =
                            static_cast<size_t>(next_decoded_samples);
                        if (next_decoded_frames <= RecordingWriter::MAX_FRAMES_PER_BLOCK) {
                            client->record_mono_block(RecordingWriter::TrackKind::Participant,
                                                      participant_id,
                                                      participant.pcm_buffer.data(),
                                                      next_decoded_frames);
                        }
                        if (participant.opus_pcm_buffered_frames + next_decoded_frames >
                            participant.opus_pcm_buffer.size()) {
                            const uint64_t dropped_packets =
                                discard_received_opus_capture_chunks(participant) +
                                (next_packet.loss_concealment ? 0 : 1);
                            participant.opus_pcm_buffered_frames = 0;
                            participant.jitter_depth_drops.fetch_add(1, std::memory_order_relaxed);
                            participant.opus_decode_buffer_overflow_drops.fetch_add(
                                1, std::memory_order_relaxed);
                            participant.receiver_delivery_drops.fetch_add(
                                dropped_packets, std::memory_order_relaxed);
                            break;
                        }

                        std::copy_n(participant.pcm_buffer.begin(), next_decoded_frames,
                                    participant.opus_pcm_buffer.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            participant.opus_pcm_buffered_frames));
                        participant.opus_pcm_buffered_frames += next_decoded_frames;
                        append_opus_capture_chunk(
                            participant, next_decoded_frames,
                            next_packet.capture_server_time_ns,
                            next_packet.capture_timestamp_valid,
                            !next_packet.loss_concealment);
                        participant.opus_packets_decoded_in_callback.fetch_add(
                            1, std::memory_order_relaxed);
                        playout_ratio = opus_playout_rate_ratio(participant);
                        required_input_frames = opus_resample_required_input_frames(
                            participant, frame_count, playout_ratio);
                    }

                    const auto decoded_count = static_cast<size_t>(decoded_samples);
                    float rms = audio_analysis::calculate_rms(participant.pcm_buffer.data(),
                                                              decoded_count);
                    float peak = audio_analysis::find_peak(participant.pcm_buffer.data(),
                                                           decoded_count);
                    participant.current_level.store(std::clamp(rms, 0.0F, 1.0F),
                                                    std::memory_order_relaxed);
                    participant.current_peak.store(std::clamp(peak, 0.0F, 1.0F),
                                                   std::memory_order_relaxed);

                    const bool is_speaking = audio_analysis::detect_voice_activity(rms);
                    participant.is_speaking.store(is_speaking, std::memory_order_relaxed);

                    playout_ratio = opus_playout_rate_ratio(participant);
                    required_input_frames = opus_resample_required_input_frames(
                        participant, frame_count, playout_ratio);
                    if (participant.opus_pcm_buffered_frames >= required_input_frames) {
                        const auto playout_server_time_ns =
                            client->server_time_for_steady_time_ns_if_ready(playout_start);
                        const size_t consumed_frames = mix_resampled_opus_pcm(
                            participant, output_buffer, frame_count, out_channels,
                            participant_gain, participant_pan, playout_ratio);
                        observe_and_consume_opus_capture_chunks(participant, consumed_frames,
                                                                playout_server_time_ns);
                        observe_opus_pcm_depth(participant);
                        observe_auto_jitter_stable(
                            participant, client->get_jitter_packet_age_limit_ms());
                        participant.opus_consecutive_empty_callbacks.store(
                            0, std::memory_order_relaxed);
                        active_count++;
                    } else if (participant.opus_pcm_buffered_frames > 0) {
                        const auto playout_server_time_ns =
                            client->server_time_for_steady_time_ns_if_ready(playout_start);
                        const size_t consumed_frames = mix_available_opus_pcm_with_tail(
                            participant, output_buffer, frame_count, out_channels,
                            participant_gain, participant_pan, playout_ratio);
                        observe_and_consume_opus_capture_chunks(participant, consumed_frames,
                                                                playout_server_time_ns);
                        observe_opus_pcm_depth(participant);
                        participant.opus_consecutive_empty_callbacks.store(
                            0, std::memory_order_relaxed);
                        active_count++;
                    }

                    observe_participant_queue_depth(participant,
                                                    participant.opus_queue.size_approx());
                    observe_opus_pcm_depth(participant);
                    return;
                }

            } else {
                // Empty queues use PLC; temporary sequence-gap waits must not advance
                // the Opus decoder because the missing packet may still arrive.
                size_t current_queue_size = participant.opus_queue.size_approx();
                observe_participant_queue_depth(participant, current_queue_size);

                if (dequeue_status == ParticipantOpusDequeueStatus::WaitingForGap) {
                    if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus &&
                        participant.opus_pcm_buffered_frames > 0) {
                        const double playout_ratio = opus_playout_rate_ratio(participant);
                        const auto playout_server_time_ns =
                            client->server_time_for_steady_time_ns_if_ready(playout_start);
                        const size_t consumed_frames = mix_available_opus_pcm_with_tail(
                            participant, output_buffer, frame_count, out_channels,
                            participant_gain, participant_pan, playout_ratio);
                        observe_and_consume_opus_capture_chunks(participant, consumed_frames,
                                                                playout_server_time_ns);
                        observe_opus_pcm_depth(participant);
                        participant.opus_consecutive_empty_callbacks.store(
                            0, std::memory_order_relaxed);
                        active_count++;
                    }
                    return;
                }

                if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus &&
                    participant.opus_pcm_buffered_frames > 0) {
                    const double playout_ratio = opus_playout_rate_ratio(participant);
                    const auto playout_server_time_ns =
                        client->server_time_for_steady_time_ns_if_ready(playout_start);
                    const size_t consumed_frames = mix_available_opus_pcm_with_tail(
                        participant, output_buffer, frame_count, out_channels, participant_gain,
                        participant_pan, playout_ratio);
                    observe_and_consume_opus_capture_chunks(participant, consumed_frames,
                                                            playout_server_time_ns);
                    observe_opus_pcm_depth(participant);
                    participant.opus_consecutive_empty_callbacks.store(0,
                                                                       std::memory_order_relaxed);
                    active_count++;
                    return;
                }

                int plc_samples = 0;
                if (participant.last_codec.load(std::memory_order_relaxed) == AudioCodec::Opus) {
                    if (!consume_audio_decode_budget(work_budget)) {
                        client->rt_diag_decode_budget_exhaustions_.fetch_add(
                            1, std::memory_order_relaxed);
                        return;
                    }
                    const auto plc_start = std::chrono::steady_clock::now();
                    plc_samples = participant.decoder->decode_plc(participant.pcm_buffer.data(),
                                                                  static_cast<int>(frame_count));
                    client->observe_rx_decode_time(std::chrono::steady_clock::now() - plc_start);
                }

                if (plc_samples > 0) {
                    // Mix PLC output (same as normal decode path)
                    size_t expected_samples = frame_count * out_channels;
                    if (static_cast<size_t>(plc_samples) == expected_samples) {
                        audio_analysis::mix_with_gain(output_buffer, participant.pcm_buffer.data(),
                                                      plc_samples, participant_gain);
                    } else if (static_cast<size_t>(plc_samples) == frame_count) {
                        audio_analysis::mix_mono_to_stereo_panned(
                            output_buffer, participant.pcm_buffer.data(), frame_count, out_channels,
                            participant_gain, participant_pan);
                    }
                    participant.plc_count.fetch_add(1, std::memory_order_relaxed);
                    observe_auto_jitter_instability(
                        participant, client->get_jitter_packet_age_limit_ms());
                }

                // Handle Opus rebuffering state. Short empty callbacks are covered by
                // PLC/tail playout above; only a sustained run is a hard underrun.
                if (participant.buffer_ready.load(std::memory_order_relaxed)) {
                    const int empty_callbacks =
                        participant.opus_consecutive_empty_callbacks.fetch_add(
                            1, std::memory_order_relaxed) +
                        1;
                    if (empty_callbacks >=
                        static_cast<int>(opus_rebuffer_empty_callback_threshold(participant))) {
                        participant.underrun_count.fetch_add(1, std::memory_order_relaxed);
                        observe_auto_jitter_instability(
                            participant, client->get_jitter_packet_age_limit_ms());
                        participant.buffer_ready.store(false, std::memory_order_relaxed);
                        participant.opus_consecutive_empty_callbacks.store(
                            0, std::memory_order_relaxed);
                    }
                }
            }
        });
        client->observe_rx_playout_time(std::chrono::steady_clock::now() - playout_start);

        // Mix WAV file audio for local output (if loaded and playing)
        // WAV and mic are completely independent - WAV can work without mic, mic can work without
        // WAV
        std::array<float, 960>
             wav_buffer{};  // Buffer for WAV audio (sized for max possible frame_count)
        int  wav_frames_read = 0;
        bool wav_active      = false;

        if (client->media_state_.wav_loaded_and_playing()) {
            wav_frames_read =
                client->media_state_.read_wav(wav_buffer.data(), static_cast<int>(frame_count),
                                              runtime_sample_rate);
            if (wav_frames_read > 0) {
                wav_active = true;  // Only set active if we actually read frames (handles EOF case)

                // Mix WAV into local output buffer only if not muted locally
                if (!client->media_state_.wav_muted_local()) {
                    float wav_gain = client->media_state_.wav_gain();
                    if (out_channels == 1) {
                        audio_analysis::mix_with_gain(output_buffer, wav_buffer.data(),
                                                      wav_frames_read, wav_gain);
                    } else {
                        // Stereo output - duplicate mono WAV to both channels
                        audio_analysis::mix_mono_to_stereo(output_buffer, wav_buffer.data(),
                                                           wav_frames_read, out_channels, wav_gain);
                    }
                    active_count++;
                }
                // Note: WAV is still sent over network even if muted locally (handled in encoding
                // section)
            }
        }

        const float target_mix_gain = audio_analysis::mix_normalization_target_gain(active_count);
        const float smoothed_mix_gain = audio_analysis::smooth_mix_normalization_gain(
            client->output_mix_gain_, target_mix_gain, frame_count, runtime_sample_rate);
        client->output_mix_gain_ = smoothed_mix_gain;
        audio_analysis::apply_gain_and_hard_limit(output_buffer, frame_count * out_channels,
                                                  smoothed_mix_gain);

        if (input_buffer != nullptr && !client->mic_muted_.load(std::memory_order_acquire)) {
            const float input_gain = client->audio_state_.input_gain();
            const float rms =
                audio_analysis::calculate_rms(input_buffer, frame_count) * input_gain;
            const float peak =
                audio_analysis::find_peak(input_buffer, frame_count) * input_gain;
            client->own_audio_level_.store(std::clamp(rms, 0.0F, 1.0F));
            client->own_audio_peak_.store(std::clamp(peak, 0.0F, 1.0F));
        } else if (!client->join_session_.can_send_audio()) {
            client->own_audio_level_.store(0.0F);
            client->own_audio_peak_.store(0.0F);
        }

        if (client->self_monitor_enabled_.load(std::memory_order_acquire) &&
            input_buffer != nullptr && !client->mic_muted_.load(std::memory_order_acquire)) {
            const float input_gain = client->audio_state_.input_gain();
            audio_analysis::mix_local_monitor(output_buffer, input_buffer, frame_count,
                                              out_channels, input_gain);
            for (unsigned long i = 0; i < frame_count * out_channels; ++i) {
                output_buffer[i] = std::clamp(output_buffer[i], -1.0F, 1.0F);
            }
        }
        client->mix_metronome_click(output_buffer, frame_count, out_channels);
        client->record_master_mix(output_buffer, frame_count, out_channels);

        // Encode and send own audio (always send to maintain timing, even if silence)
        // Mix WAV with microphone input before encoding
        if (client->audio_.is_stream_active() && client->join_session_.can_send_audio()) {
            if (!client->audio_encoder_.is_initialized()) {
                return 0;
            }

            std::array<float, 960> opus_input{};
            if (wav_active && wav_frames_read > 0) {
                float wav_gain = client->media_state_.wav_gain();
                for (int i = 0; i < wav_frames_read; ++i) {
                    opus_input[static_cast<size_t>(i)] = wav_buffer[static_cast<size_t>(i)] * wav_gain;
                }
            }

            if (input_buffer != nullptr && !client->mic_muted_.load(std::memory_order_acquire)) {
                float input_gain = client->audio_state_.input_gain();
                for (unsigned long i = 0; i < frame_count; ++i) {
                    float mic_sample = input_buffer[i] * input_gain;
                    opus_input[i] = wav_active ? (opus_input[i] + mic_sample) * 0.5F : mic_sample;
                }
            } else if (wav_active) {
                for (unsigned long i = 0; i < frame_count; ++i) {
                    opus_input[i] *= 0.5F;
                }
            }

            float rms = audio_analysis::calculate_rms(opus_input.data(), frame_count);
            float peak = audio_analysis::find_peak(opus_input.data(), frame_count);
            client->own_audio_level_.store(std::clamp(rms, 0.0F, 1.0F));
            client->own_audio_peak_.store(std::clamp(peak, 0.0F, 1.0F));
            client->record_mono_block(RecordingWriter::TrackKind::Self, 0, opus_input.data(),
                                      frame_count);
            client->enqueue_opus_send_samples(
                opus_input.data(), frame_count,
                static_cast<uint32_t>(runtime_sample_rate), callback_start);
        }

        return 0;
    }

    asio::io_context& io_context_;
    udp::socket       socket_;
    udp_network::UdpSocketQos socket_qos_;
    mutable std::mutex socket_mutex_;
    mutable std::mutex server_endpoint_mutex_;
    udp::endpoint      server_endpoint_;
    ClientJoinSession join_session_;
    ClientAudioState audio_state_;
    ClientMediaState media_state_;
    ClientMetronome metronome_;
    std::filesystem::path audio_preferences_path_;
    std::shared_ptr<const session_crypto::SessionKey> session_key_;
    std::atomic<uint32_t> secure_control_sequence_{1};
    std::unordered_map<uint32_t, ReplayWindow> secure_control_replay_windows_;
    session_crypto::E2EPublicKey e2e_public_key_{};
    session_crypto::E2ESecretKey e2e_secret_key_{};
    Bytes<E2E_PUBLIC_KEY_BYTES> e2e_key_public_bytes_{};
    bool e2e_keypair_ready_ = false;
    mutable std::mutex access_request_mutex_;
    std::unordered_map<uint32_t, Bytes<E2E_PUBLIC_KEY_BYTES>> participant_key_public_;
    std::unordered_map<uint32_t, ParticipantInfo> pending_access_requests_;
    std::unordered_set<uint32_t> pending_key_handoff_acks_;

    AudioStream              audio_;
    std::mutex               audio_lifecycle_mutex_;
    asio::thread_pool        audio_device_worker_{1};
    std::shared_ptr<int>     lifetime_token_ = std::make_shared<int>(0);
    std::atomic<uint64_t>    audio_device_generation_{0};
    std::atomic<bool>        audio_stream_requested_{false};
    std::atomic<bool>        audio_recovery_in_flight_{false};
    std::atomic<uint32_t>    audio_recovery_attempts_{0};
    std::atomic<int64_t>     next_audio_recovery_ns_{0};
    OpusEncoderWrapper       audio_encoder_;
    std::atomic<uint16_t>    opus_network_frame_count_{
        opus_network_clock::DEFAULT_FRAME_COUNT};
    std::mutex               packet_duration_mutex_;
    congestion_policy::PacketDurationState packet_duration_state_{};
    std::atomic<int>         opus_jitter_buffer_ms_{DEFAULT_OPUS_JITTER_MS};
    std::atomic<size_t>      opus_queue_limit_packets_{DEFAULT_OPUS_QUEUE_LIMIT_PACKETS};
    std::atomic<int>         jitter_packet_age_limit_ms_{DEFAULT_JITTER_PACKET_AGE_MS};
    std::atomic<bool>        opus_auto_jitter_default_{true};
    std::atomic<int>         opus_redundancy_depth_packets_{
        DEFAULT_OPUS_REDUNDANCY_DEPTH_PACKETS};
    std::atomic<int>         congestion_max_bitrate_{congestion_policy::DEFAULT_MAX_BITRATE};
    std::atomic<int>         congestion_target_bitrate_{congestion_policy::DEFAULT_MAX_BITRATE};
    std::atomic<int>         adaptive_redundancy_depth_{-1};
    std::atomic<uint32_t>    congestion_healthy_intervals_{0};
    std::atomic<uint32_t>    audio_tx_sequence_{0};
    std::atomic<uint64_t>    audio_packets_generated_{0};
    float                    output_mix_gain_ = 1.0F;
    // Pre-sized so the audio callback's try_enqueue never allocates
    // (max_send_queue_frames caps useful depth at 8; 64 gives block-pool slack).
    moodycamel::ConcurrentQueue<OpusSendFrame> opus_send_queue_{64};
    std::array<float, 960>                     opus_tx_accumulator_{};
    size_t                                     opus_tx_accumulated_frames_ = 0;
    std::chrono::steady_clock::time_point      opus_tx_accumulator_capture_time_{};
    std::array<TxPacketBuffer, RECENT_OPUS_PACKET_SLOTS> recent_opus_audio_packets_{};
    size_t                                     recent_opus_audio_packet_count_ = 0;
    std::atomic<bool>                         recent_opus_audio_packets_reset_requested_{false};
    std::atomic<bool>                         opus_tx_accumulator_reset_requested_{false};
    std::atomic<bool>                         audio_sender_running_{false};
    std::thread                               audio_sender_thread_;
    std::condition_variable                   audio_sender_cv_;
    std::mutex                                audio_sender_wait_mutex_;
    std::atomic<bool>                         audio_sender_wake_{false};
    std::atomic<uint64_t>                     opus_send_drops_{0};
    std::atomic<uint64_t>                     tx_socket_would_block_drops_{0};
    // Written by the audio callback (relaxed atomics), drained and logged by
    // the io-thread cleanup timer. The callback itself must never log.
    std::atomic<uint64_t> rt_diag_mix_size_mismatches_{0};
    std::atomic<uint64_t> rt_diag_decode_failures_{0};
    std::atomic<uint64_t> rt_diag_decode_budget_exhaustions_{0};
    uint64_t              rt_diag_logged_mix_size_mismatches_  = 0;  // io thread only
    uint64_t              rt_diag_logged_decode_failures_      = 0;  // io thread only
    uint64_t              rt_diag_logged_decode_budget_exhaustions_ = 0;  // io thread only
    std::atomic<uint64_t>                     outbound_malformed_audio_drops_{0};
    std::atomic<uint64_t>                     inbound_malformed_audio_drops_{0};
    std::atomic<uint64_t>                     stray_udp_packets_{0};
    std::unordered_map<uint32_t, DeliveryReportAccumulator>
        delivery_report_accumulators_;
    std::unordered_map<uint32_t, DownstreamDeliveryReporter>
        downstream_delivery_by_reporter_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
        downstream_delivery_last_activity_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
        downstream_delivery_tombstone_since_;
    std::unordered_map<uint32_t, DownstreamFeedbackInterval>
        downstream_feedback_by_reporter_;
    bool downstream_feedback_pending_ = false;
    uint64_t delivery_report_generated_baseline_ = 0;
    uint32_t next_delivery_report_epoch_ = 1;
    std::atomic<bool>                         receiving_enabled_{false};
    std::atomic<uint64_t>                     receive_generation_{0};
    std::array<ReceiveState, RECEIVE_SLOT_COUNT> receive_states_{};
    std::atomic<bool>                         outbound_enabled_{false};
    std::atomic<uint64_t>                     outbound_generation_{0};
    std::atomic<bool>                         join_denied_room_full_{false};
    std::atomic<bool>                         room_removed_by_server_{false};
    std::atomic<int64_t>                      opus_send_queue_age_last_ns_{0};
    std::atomic<int64_t>                      opus_send_queue_age_avg_ns_{0};
    std::atomic<int64_t>                      opus_send_queue_age_max_ns_{0};
    std::atomic<int64_t>                      opus_send_queue_age_p99_ns_{0};
    LatencyPercentileWindow                   opus_send_queue_age_window_;
    std::atomic<int64_t>                      tx_encode_last_ns_{0};
    std::atomic<int64_t>                      tx_encode_avg_ns_{0};
    std::atomic<int64_t>                      tx_encode_max_ns_{0};
    std::atomic<int64_t>                      tx_send_pace_last_ns_{0};
    std::atomic<int64_t>                      tx_send_pace_avg_ns_{0};
    std::atomic<int64_t>                      tx_send_pace_max_ns_{0};
    std::atomic<int64_t>                      rx_decode_last_ns_{0};
    std::atomic<int64_t>                      rx_decode_avg_ns_{0};
    std::atomic<int64_t>                      rx_decode_max_ns_{0};
    std::atomic<int64_t>                      rx_playout_last_ns_{0};
    std::atomic<int64_t>                      rx_playout_avg_ns_{0};
    std::atomic<int64_t>                      rx_playout_max_ns_{0};
    std::chrono::steady_clock::time_point     last_audio_packet_send_time_{};

    ParticipantManager participant_manager_;

    // Microphone mute (thread-safe with atomic)
    std::atomic<bool> mic_muted_{false};  // Mute mic (doesn't send to server)
    std::atomic<bool> self_monitor_enabled_{false};

    // Own audio level tracking (thread-safe with atomic)
    std::atomic<float> own_audio_level_{0.0F};
    std::atomic<float> own_audio_peak_{0.0F};

    // RTT tracking (thread-safe with atomic)
    std::atomic<double> rtt_ms_{0.0};
    std::atomic<int64_t> rtt_last_ns_{0};
    std::atomic<int64_t> rtt_min_ns_{0};
    std::atomic<int64_t> rtt_avg_ns_{0};
    std::atomic<int64_t> rtt_max_ns_{0};
    std::atomic<int64_t> server_clock_offset_ns_{0};
    std::atomic<int64_t> server_clock_uncertainty_ns_{0};
    std::atomic<bool>    server_clock_ready_{false};
    std::mutex server_clock_estimator_mutex_;
    clock_offset_estimator::Estimator server_clock_estimator_;
    std::atomic<uint32_t> ping_tx_sequence_{0};
    std::atomic<uint32_t> last_ping_reply_sequence_{0};
    std::atomic<bool>     have_ping_reply_sequence_{false};
    std::atomic<uint32_t> ping_path_watch_start_sequence_{0};
    std::atomic<uint32_t> ping_path_interval_received_{0};
    std::atomic<uint32_t> ping_path_interval_missing_{0};
    std::atomic<uint32_t> ping_path_total_received_{0};
    std::atomic<uint32_t> ping_path_total_missing_{0};
    std::atomic<uint32_t> ping_path_consecutive_missing_{0};
    std::atomic<uint32_t> audio_path_interval_received_{0};
    std::atomic<uint32_t> audio_path_interval_sequence_gaps_{0};
    std::atomic<uint32_t> audio_path_interval_gaps_{0};
    std::atomic<int64_t>  next_udp_path_rebind_allowed_ns_{0};
    std::atomic<bool>     udp_path_rebind_pending_{false};
    std::atomic<uint32_t> udp_path_rebind_count_{0};

    // Total bytes sent/received (cumulative counters)
    std::atomic<uint64_t> total_bytes_rx_{0};
    std::atomic<uint64_t> total_bytes_tx_{0};

    // Audio callback timing diagnostics
    std::atomic<int64_t>  callback_last_ns_{0};
    std::atomic<int64_t>  callback_max_ns_{0};
    std::atomic<int64_t>  callback_avg_ns_{0};
    std::atomic<int64_t>  callback_deadline_ns_{0};
    std::atomic<uint64_t> callback_count_{0};
    std::atomic<uint64_t> callback_over_deadline_count_{0};
    struct ParticipantDropSnapshot {
        uint64_t jitter_depth_drops = 0;
        uint64_t jitter_age_drops   = 0;
        uint64_t opus_packets_decoded_in_callback = 0;
        uint64_t opus_queue_limit_drops = 0;
        uint64_t opus_age_limit_drops = 0;
        uint64_t opus_decode_buffer_overflow_drops = 0;
        uint64_t opus_target_trim_drops = 0;
    };
    std::chrono::steady_clock::time_point                  last_audio_health_log_time_{};
    uint64_t                                               last_opus_send_drops_ = 0;
    uint64_t                                               last_outbound_malformed_audio_drops_ = 0;
    std::unordered_map<uint32_t, ParticipantDropSnapshot>  participant_drop_snapshots_;

    mutable std::mutex chat_mutex_;
    std::vector<ClientChatMessage> chat_messages_;
    std::unordered_map<std::string, PendingChatMessage> pending_chat_messages_;
    uint64_t last_seen_chat_sequence_ = 0;
    size_t chat_unread_count_ = 0;
    bool chat_dialog_open_ = false;
    bool chat_history_truncated_ = false;
    std::string chat_status_;
    std::string chat_retry_text_;
    std::atomic<uint32_t> chat_request_id_{1};

    PeriodicTimer ping_timer_;
    PeriodicTimer join_retry_timer_;
    PeriodicTimer alive_timer_;
    PeriodicTimer cleanup_timer_;
    PeriodicTimer chat_timer_;
};

class ClientAppAdapter final : public ClientAppFacade {
public:
    explicit ClientAppAdapter(Client& client)
        : client_(client) {
    }

    bool start_audio_stream(AudioStream::DeviceIndex input_device,
                            AudioStream::DeviceIndex output_device,
                            const AudioStream::AudioConfig& config) override {
        return client_.start_audio_stream(input_device, output_device, config);
    }

    void stop_audio_stream() override {
        client_.stop_audio_stream();
    }

    bool is_audio_stream_active() const override {
        return client_.is_audio_stream_active();
    }

    bool swap_audio_devices(AudioStream::DeviceIndex input_device,
                            AudioStream::DeviceIndex output_device) override {
        return client_.swap_audio_devices(input_device, output_device);
    }

    void reset_audio_path() override {
        client_.reset_audio_path();
    }

    void start_connection(const std::string& server_address, uint16_t server_port) override {
        client_.start_connection(server_address, server_port);
    }

    void stop_connection() override {
        client_.stop_connection();
    }

    void join_room(const std::string& server_address, uint16_t server_port,
                   const std::string& room_id, const std::string& profile_id,
                   const std::string& display_name,
                   const std::string& join_token,
                   const std::string& media_secret = {},
                   uint8_t access_mode = ROOM_ACCESS_OPEN) override {
        client_.join_room(server_address, server_port, room_id, profile_id,
                          display_name, join_token, media_secret, access_mode);
    }

    bool rotate_media_key(const std::string& media_secret,
                          uint32_t access_epoch) override {
        return client_.rotate_media_key(media_secret, access_epoch);
    }

    bool has_media_key() const override {
        return client_.has_media_key();
    }

    std::string current_media_secret() const override {
        return client_.current_media_secret();
    }

    void set_room_access_mode(uint8_t access_mode) override {
        client_.set_room_access_mode(access_mode);
    }

    bool approve_waiting_participant(uint32_t participant_id) override {
        return client_.approve_waiting_participant(participant_id);
    }

    std::vector<ParticipantInfo> get_waiting_participant_info() const override {
        return client_.get_waiting_participant_info();
    }

    bool is_join_confirmed() const override {
        return client_.is_join_confirmed();
    }

    bool consume_join_denied_room_full() override {
        return client_.consume_join_denied_room_full();
    }

    bool consume_room_removed_by_server() override {
        return client_.consume_room_removed_by_server();
    }

    std::string get_server_address() const override {
        return client_.get_server_address();
    }

    unsigned short get_server_port() const override {
        return client_.get_server_port();
    }

    std::string get_room_id() const override {
        return client_.get_room_id();
    }

    std::vector<ParticipantInfo> get_participant_info() const override {
        return client_.get_participant_info();
    }

    ChatState get_chat_state() const override {
        return client_.get_chat_state();
    }

    void set_chat_dialog_open(bool open) override {
        client_.set_chat_dialog_open(open);
    }

    bool send_chat_message(const std::string& text) override {
        return client_.send_chat_message(text);
    }

    void request_chat_history() override {
        client_.request_chat_history();
    }

    float get_own_audio_level() const override {
        return client_.get_own_audio_level();
    }

    float get_own_audio_peak() const override {
        return client_.get_own_audio_peak();
    }

    double get_rtt_ms() const override {
        return client_.get_rtt_ms();
    }

    uint64_t get_total_bytes_rx() const override {
        return client_.get_total_bytes_rx();
    }

    uint64_t get_total_bytes_tx() const override {
        return client_.get_total_bytes_tx();
    }

    void set_mic_muted(bool muted) override {
        client_.set_mic_muted(muted);
    }

    bool get_mic_muted() const override {
        return client_.get_mic_muted();
    }

    void set_self_monitor_enabled(bool enabled) override {
        client_.set_self_monitor_enabled(enabled);
    }

    bool get_self_monitor_enabled() const override {
        return client_.get_self_monitor_enabled();
    }

    void set_input_gain(float gain) override {
        client_.set_input_gain(gain);
    }

    float get_input_gain() const override {
        return client_.get_input_gain();
    }

    DeviceInfo get_device_info() const override {
        return client_.get_device_info();
    }

    AudioStream::LatencyInfo get_latency_info() const override {
        return client_.get_latency_info();
    }

    AudioStream::AudioConfig get_audio_config() const override {
        return client_.get_audio_config();
    }

    CallbackTimingInfo get_callback_timing_info() const override {
        return client_.get_callback_timing_info();
    }

    PathDiagnostics get_path_diagnostics() const override {
        return client_.get_path_diagnostics();
    }

    int get_input_channel_index() const override {
        return client_.get_input_channel_index();
    }

    void set_input_channel_index(int channel_index) override {
        client_.set_input_channel_index(channel_index);
    }

    void set_requested_frames_per_buffer(int frames_per_buffer) override {
        client_.set_requested_frames_per_buffer(frames_per_buffer);
    }

    AudioStream::DeviceIndex get_selected_input_device() const override {
        return client_.get_selected_input_device();
    }

    AudioStream::DeviceIndex get_selected_output_device() const override {
        return client_.get_selected_output_device();
    }

    std::string get_audio_api_filter() const override {
        return client_.get_audio_api_filter();
    }

    void set_audio_api_filter(std::string api_filter) override {
        client_.set_audio_api_filter(std::move(api_filter));
    }

    bool save_audio_device_preferences() const override {
        return client_.save_audio_device_preferences();
    }

    bool set_input_device(AudioStream::DeviceIndex device_index) override {
        return client_.set_input_device(device_index);
    }

    bool set_output_device(AudioStream::DeviceIndex device_index) override {
        return client_.set_output_device(device_index);
    }

    uint16_t get_opus_network_frame_count() const override {
        return client_.get_opus_network_frame_count();
    }

    void set_opus_network_frame_count(int frame_count) override {
        client_.set_opus_network_frame_count(frame_count);
    }

    double get_opus_network_packet_ms() const override {
        return client_.get_opus_network_packet_ms();
    }

    int get_opus_jitter_buffer_ms() const override {
        return client_.get_opus_jitter_buffer_ms();
    }

    size_t get_opus_jitter_buffer_packets() const override {
        return client_.get_opus_jitter_buffer_packets();
    }

    void set_opus_jitter_buffer_ms(int target_ms) override {
        client_.set_opus_jitter_buffer_ms(target_ms);
    }

    size_t get_opus_auto_start_jitter_packets() const override {
        return client_.get_opus_auto_start_jitter_packets();
    }

    int get_opus_auto_start_jitter_ms() const override {
        return client_.get_opus_auto_start_jitter_ms();
    }

    size_t get_opus_queue_limit_packets() const override {
        return client_.get_opus_queue_limit_packets();
    }

    void set_opus_queue_limit_packets(size_t packets) override {
        client_.set_opus_queue_limit_packets(packets);
    }

    int get_jitter_packet_age_limit_ms() const override {
        return client_.get_jitter_packet_age_limit_ms();
    }

    void set_jitter_packet_age_limit_ms(int age_ms) override {
        client_.set_jitter_packet_age_limit_ms(age_ms);
    }

    bool get_opus_auto_jitter_default() const override {
        return client_.get_opus_auto_jitter_default();
    }

    void set_opus_auto_jitter_default(bool enabled) override {
        client_.set_opus_auto_jitter_default(enabled);
    }

    int get_opus_redundancy_depth_setting() const override {
        return client_.get_opus_redundancy_depth_setting();
    }

    int get_effective_opus_redundancy_depth() const override {
        return client_.get_effective_opus_redundancy_depth();
    }

    void set_opus_redundancy_depth(int depth) override {
        client_.set_opus_redundancy_depth(depth);
    }

    void set_participant_muted(uint32_t id, bool muted) override {
        client_.set_participant_muted(id, muted);
    }

    void set_participant_gain(uint32_t id, float gain) override {
        client_.set_participant_gain(id, gain);
    }

    void set_participant_pan(uint32_t id, float pan) override {
        client_.set_participant_pan(id, pan);
    }

    void set_participant_opus_jitter_buffer_ms(uint32_t id, int target_ms) override {
        client_.set_participant_opus_jitter_buffer_ms(id, target_ms);
    }

    void reset_participant_opus_jitter_buffer_packets(uint32_t id) override {
        client_.reset_participant_opus_jitter_buffer_packets(id);
    }

    void set_participant_opus_auto_jitter(uint32_t id, bool enabled) override {
        client_.set_participant_opus_auto_jitter(id, enabled);
    }

    MetronomeState get_metronome_state() const override {
        return client_.get_metronome_state();
    }

    void commit_metronome_bpm(float bpm) override {
        client_.commit_metronome_bpm(bpm);
    }

    void start_metronome() override {
        client_.start_metronome();
    }

    void stop_metronome() override {
        client_.stop_metronome();
    }

    void tap_metronome_tempo() override {
        client_.tap_metronome_tempo();
    }

    RecordingState get_recording_state() const override {
        return client_.get_recording_state();
    }

    bool start_recording() override {
        return client_.start_recording();
    }

    void stop_recording() override {
        client_.stop_recording();
    }

    bool load_wav_file(const std::string& path) override {
        return client_.load_wav_file(path);
    }

    void wav_play() override {
        client_.wav_play();
    }

    void wav_pause() override {
        client_.wav_pause();
    }

    void wav_seek(int64_t frame_position) override {
        client_.wav_seek(frame_position);
    }

    void set_wav_gain(float gain) override {
        client_.set_wav_gain(gain);
    }

    void set_wav_muted_local(bool muted) override {
        client_.set_wav_muted_local(muted);
    }

    WavState get_wav_state() const override {
        return client_.get_wav_state();
    }

private:
    Client& client_;
};

class ClientRuntime::Impl {
public:
    Impl(asio::io_context& io_context, PerformerJoinOptions performer_join_options,
         std::filesystem::path audio_preferences_path,
         AudioDevicePreferences audio_preferences)
        : client(io_context, std::move(performer_join_options),
                 std::move(audio_preferences_path), std::move(audio_preferences)),
          adapter(client) {
    }

    Client client;
    ClientAppAdapter adapter;
};

ClientRuntime::ClientRuntime(asio::io_context& io_context,
                             PerformerJoinOptions performer_join_options,
                             std::filesystem::path audio_preferences_path,
                             AudioDevicePreferences audio_preferences)
    : impl_(std::make_unique<Impl>(io_context, std::move(performer_join_options),
                                   std::move(audio_preferences_path),
                                   std::move(audio_preferences))) {
}

ClientRuntime::~ClientRuntime() = default;

ClientAppFacade& ClientRuntime::app_facade() {
    return impl_->adapter;
}

const ClientAppFacade& ClientRuntime::app_facade() const {
    return impl_->adapter;
}

void ClientRuntime::stop_connection() {
    impl_->client.stop_connection();
}

void ClientRuntime::set_opus_jitter_buffer_packets(size_t packets) {
    impl_->client.set_opus_jitter_buffer_packets(packets);
}
