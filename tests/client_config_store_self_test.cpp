#include "client_config_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto test_directory = std::filesystem::temp_directory_path() /
                                ("sesivo-config-store-test-" +
                                 std::to_string(juce::Time::currentTimeMillis()));
    const auto config_path = test_directory / "sesivo-client.json";
    require(!load_client_mixer_ui_state(config_path).advanced_latency_open,
            "Advanced must default to closed when no config exists");
    std::filesystem::create_directories(test_directory);

    {
        std::ofstream config(config_path);
        config << R"({"audio":{"api":"ASIO"},"mixer":{"advancedLatencyOpen":true}})";
    }

    require(load_client_mixer_ui_state(config_path).advanced_latency_open,
            "the saved open Advanced state must load");

    ClientMixerUiState state;
    state.advanced_latency_open = false;
    require(save_client_mixer_ui_state(config_path, state),
            "the closed Advanced state write must be accepted");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool saved_closed_state = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto root = read_client_config_root(config_path);
        const auto* root_object = root.getDynamicObject();
        const auto* mixer = root_object == nullptr
                                ? nullptr
                                : root_object->getProperty("mixer").getDynamicObject();
        const auto saved_value =
            mixer == nullptr ? juce::var{} : mixer->getProperty("advancedLatencyOpen");
        if (saved_value.isBool() && !static_cast<bool>(saved_value)) {
            saved_closed_state = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(saved_closed_state, "the closed Advanced state must persist");

    const auto root = read_client_config_root(config_path);
    const auto* root_object = root.getDynamicObject();
    require(root_object != nullptr, "the saved config root must remain an object");
    const auto* audio = root_object->getProperty("audio").getDynamicObject();
    require(audio != nullptr && audio->getProperty("api").toString() == "ASIO",
            "saving mixer state must preserve unrelated config properties");

    std::filesystem::remove_all(test_directory);
    std::cout << "client config store self-test passed\n";
    return 0;
}
