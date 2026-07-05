#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>

#include <asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include "audio_stream.h"
#include "client_app_facade.h"
#include "client_audio_devices.h"
#include "client_runtime.h"
#include "client_startup.h"
#include "gui.h"
#include "imgui_client_ui.h"
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
        if (!startup_options.required_audio_api.empty() &&
            !required_api_has_duplex_devices(startup_options.required_audio_api)) {
            spdlog::error("Required audio API '{}' does not have both input and output devices",
                          startup_options.required_audio_api);
            print_audio_backend_inventory();
            logging::flush();
            return 2;
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

        if (!startup_options.required_audio_api.empty()) {
            const auto input_dev =
                find_device_for_api(startup_options.required_audio_api, true);
            const auto output_dev =
                find_device_for_api(startup_options.required_audio_api, false);
            client_app.set_input_device(input_dev);
            client_app.set_output_device(output_dev);
            client_app.set_audio_api_filter(startup_options.required_audio_api);
            spdlog::info("Startup required audio API: {}", startup_options.required_audio_api);
        }
        if (startup_options.requested_frames > 0) {
            client_app.set_requested_frames_per_buffer(startup_options.requested_frames);
            spdlog::info("Startup requested buffer override: {} frames",
                         startup_options.requested_frames);
        }
        if (startup_options.startup_input_channel_index.has_value()) {
            client_app.set_input_channel_index(*startup_options.startup_input_channel_index);
            spdlog::info("Startup input channel override: channel {} (index {})",
                         *startup_options.startup_input_channel_index + 1,
                         *startup_options.startup_input_channel_index);
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

        {
            AudioStream::DeviceIndex input_dev = client_app.get_selected_input_device();
            AudioStream::DeviceIndex output_dev = client_app.get_selected_output_device();
            if (input_dev != AudioStream::NO_DEVICE && output_dev != AudioStream::NO_DEVICE) {
                AudioStream::AudioConfig config = client_app.get_audio_config();
                if (client_app.start_audio_stream(input_dev, output_dev, config)) {
                    spdlog::info("Auto-started audio stream with default devices");
                } else {
                    spdlog::warn("Failed to auto-start audio stream");
                }
            }
        }

        std::thread io_thread([&io_context]() { io_context.run(); });

        const std::string window_title =
            startup_options.app_version.empty()
                ? "Jam"
                : "Jam " + startup_options.app_version;
        Gui app(810, 555, window_title.c_str(), false, 60);

        app.set_draw_callback([&client_app]() { draw_client_ui(client_app); });

        app.set_close_callback([&io_context]() {
            io_context.stop();
        });
        app.run();

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
