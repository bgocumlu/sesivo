#pragma once

#include "client_app_facade.h"
#include "client_audio_devices.h"
#include "client_join_session.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace asio {
class io_context;
}

class ClientRuntime {
public:
    ClientRuntime(asio::io_context& io_context,
                  PerformerJoinOptions performer_join_options = {},
                  std::filesystem::path audio_preferences_path = {},
                  AudioDevicePreferences audio_preferences = {});
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    ClientAppFacade& app_facade();
    const ClientAppFacade& app_facade() const;

    void stop_connection();
    void set_opus_jitter_buffer_packets(size_t packets);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
