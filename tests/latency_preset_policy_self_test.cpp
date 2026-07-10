#include "latency_preset_policy.h"
#include "client_startup.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void require_bundle(int id, int packet_frames, int jitter_ms, int redundancy_depth,
                    const char* message) {
    const auto* preset = latency_preset_for_id(id);
    require(preset != nullptr, message);
    require(preset->packet_frames == packet_frames, message);
    require(preset->jitter_ms == jitter_ms, message);
    require(preset->redundancy_depth == redundancy_depth, message);
    require(!preset->auto_jitter, "every preset must explicitly disable auto jitter");
    require(latency_preset_id_for_settings(
                preset->packet_frames, preset->jitter_ms, preset->queue_limit_packets,
                preset->age_limit_ms, preset->redundancy_depth, preset->auto_jitter) == id,
            "each preset bundle must reverse-match its own id");
}

}  // namespace

int main() {
    require(LATENCY_PRESETS.size() == 4, "the latency ladder must have four steps");
    require(DEFAULT_LATENCY_PRESET_ID == LATENCY_PRESET_LOW_ID,
            "Low must be the default latency preset");
    require(ClientStartupOptions{}.startup_latency_profile == "low",
            "fresh client launches must select the Low latency preset");
    require_bundle(LATENCY_PRESET_ULTRA_ID, 120, 10, 1,
                   "Ultra must use 120 frames, 10 ms jitter, and redundancy depth 1");
    require_bundle(LATENCY_PRESET_LOW_ID, 240, 15, 2,
                   "Low must use 240 frames, 15 ms jitter, and redundancy depth 2");
    require_bundle(LATENCY_PRESET_BALANCED_ID, 480, 20, 2,
                   "Balanced must use 480 frames, 20 ms jitter, and redundancy depth 2");
    require_bundle(LATENCY_PRESET_STABLE_ID, 960, 80, 3,
                   "Stable must use 960 frames, 80 ms jitter, and redundancy depth 3");

    const auto& low = *latency_preset_for_id(LATENCY_PRESET_LOW_ID);
    require(latency_preset_id_for_settings(
                low.packet_frames, low.jitter_ms, low.queue_limit_packets, low.age_limit_ms,
                low.redundancy_depth, true) == LATENCY_PRESET_CUSTOM_ID,
            "changing auto jitter must produce Custom state");
    require(latency_preset_id_for_settings(
                low.packet_frames, low.jitter_ms, low.queue_limit_packets, low.age_limit_ms,
                1, low.auto_jitter) == LATENCY_PRESET_CUSTOM_ID,
            "changing redundancy must produce Custom state");

    std::cout << "latency preset policy self-test passed\n";
    return 0;
}
