#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include "client_app_facade.h"
#include "client_audio_devices.h"
#include "client_runtime.h"
#include "client_startup.h"
#include "juce_app.h"
#include "logging_setup.h"
#include "opus_defines.h"

static bool apply_startup_latency_profile(ClientAppFacade& client,
                                          const ClientStartupOptions& startup_options) {
    const StartupLatencyProfile profile = resolve_startup_latency_profile(startup_options);
    if (!profile.valid) {
        return false;
    }
    if (!profile.enabled) {
        return true;
    }

    if (!startup_options.startup_opus_packet_frames.has_value()) {
        client.set_opus_network_frame_count(profile.opus_packet_frames);
    }
    if (!startup_options.startup_jitter_packets.has_value() &&
        !startup_options.startup_jitter_ms.has_value()) {
        client.set_opus_jitter_buffer_ms(profile.jitter_ms);
    }
    if (!startup_options.startup_queue_limit_packets.has_value()) {
        client.set_opus_queue_limit_packets(profile.queue_limit_packets);
    }
    if (!startup_options.startup_age_limit_ms.has_value()) {
        client.set_jitter_packet_age_limit_ms(profile.age_limit_ms);
    }
    if (!startup_options.startup_auto_jitter &&
        !startup_options.startup_disable_auto_jitter) {
        client.set_opus_auto_jitter_default(profile.auto_jitter);
    }

    spdlog::info("Startup latency profile: {}", profile.name);
    return true;
}

int main(int argc, char** argv) {
    try {
        auto startup_options = parse_startup_options(argc, argv);
        logging::init(true, true, !startup_options.log_file_path.empty(),
                      startup_options.log_file_path, logging::default_level());
        if (!startup_options.log_file_path.empty()) {
            spdlog::info("Logging to {}", startup_options.log_file_path);
        }
        spdlog::info("Runtime: process=client platform={} arch={}", runtime_platform_name(),
                     runtime_arch_name());
        if (startup_options.startup_jitter_packets.has_value() &&
            startup_options.startup_jitter_ms.has_value()) {
            spdlog::error(
                "Cannot combine packet jitter override (--jitter/--opus-jitter) with "
                "millisecond jitter override (--jitter-ms/--opus-jitter-ms)");
            logging::flush();
            return 2;
        }

        if (startup_options.list_audio_devices) {
            print_audio_backend_inventory();
            logging::flush();
            return 0;
        }
        if (startup_options.low_latency_check) {
            const int result = run_low_latency_backend_check(startup_options);
            logging::flush();
            return result;
        }
        asio::io_context io_context;
        const auto audio_preferences_path =
            client_config_path(argv[0], startup_options.config_dir);
        const auto audio_preferences =
            load_audio_device_preferences(audio_preferences_path);

        ClientRuntime client_runtime(io_context, startup_options.server_address,
                                     startup_options.server_port,
                                     startup_options.performer_join,
                                     audio_preferences_path, audio_preferences);
        ClientAppFacade& client_app = client_runtime.app_facade();

        if (startup_options.requested_frames > 0) {
            client_app.set_requested_frames_per_buffer(startup_options.requested_frames);
            spdlog::info("Startup requested buffer override: {} frames",
                         startup_options.requested_frames);
        }
        if (!apply_startup_latency_profile(client_app, startup_options)) {
            client_runtime.stop_connection();
            logging::flush();
            return 2;
        }
        if (startup_options.startup_opus_packet_frames.has_value()) {
            client_app.set_opus_network_frame_count(
                *startup_options.startup_opus_packet_frames);
            spdlog::info("Startup Opus packet override: {} frames",
                         *startup_options.startup_opus_packet_frames);
        }
        if (startup_options.startup_jitter_ms.has_value()) {
            client_app.set_opus_jitter_buffer_ms(
                std::max(*startup_options.startup_jitter_ms, 0));
            spdlog::info("Startup Opus jitter override: {} ms",
                         *startup_options.startup_jitter_ms);
        }
        if (startup_options.startup_jitter_packets.has_value()) {
            client_runtime.set_opus_jitter_buffer_packets(
                static_cast<size_t>(std::max(*startup_options.startup_jitter_packets, 0)));
            spdlog::info("Startup Opus jitter override: {} packets",
                         *startup_options.startup_jitter_packets);
        }
        if (startup_options.startup_queue_limit_packets.has_value()) {
            client_app.set_opus_queue_limit_packets(
                static_cast<size_t>(std::max(*startup_options.startup_queue_limit_packets, 0)));
            spdlog::info("Startup Opus queue limit override: {} packets",
                         *startup_options.startup_queue_limit_packets);
        }
        if (startup_options.startup_redundancy_depth_packets.has_value()) {
            client_app.set_opus_redundancy_depth(
                *startup_options.startup_redundancy_depth_packets);
            const int depth = client_app.get_opus_redundancy_depth_setting();
            spdlog::info("Startup Opus redundancy depth override: {}",
                         depth == OPUS_REDUNDANCY_DEPTH_AUTO ? "auto"
                                                             : std::to_string(depth));
        }
        if (startup_options.startup_age_limit_ms.has_value()) {
            client_app.set_jitter_packet_age_limit_ms(*startup_options.startup_age_limit_ms);
            spdlog::info("Startup packet age limit override: {} ms",
                         *startup_options.startup_age_limit_ms);
        }
        if (startup_options.startup_disable_auto_jitter) {
            client_app.set_opus_auto_jitter_default(false);
            spdlog::info("Startup Opus auto jitter default disabled");
        } else if (startup_options.startup_auto_jitter) {
            client_app.set_opus_auto_jitter_default(true);
            spdlog::info("Startup Opus auto jitter default enabled");
        }

        std::thread io_thread([&io_context]() { io_context.run(); });

        const std::string window_title =
            startup_options.app_version.empty()
                ? "Jam"
                : "Jam " + startup_options.app_version;
        JuceClientStartupAudioOptions startup_audio_options;
        startup_audio_options.audio_preferences = audio_preferences;
        startup_audio_options.required_audio_api = startup_options.required_audio_api;
        startup_audio_options.startup_input_channel_index =
            startup_options.startup_input_channel_index;
        run_juce_client_app(client_app, window_title, std::move(startup_audio_options),
                            [&io_context]() { io_context.stop(); });

        client_app.stop_audio_stream();
        client_runtime.stop_connection();

        io_context.stop();
        if (io_thread.joinable()) {
            io_thread.join();
        }
        logging::flush();
    } catch (std::exception& e) {
        spdlog::error("ERR: {}", e.what());
        logging::flush();
    }
}
