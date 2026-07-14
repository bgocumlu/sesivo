#include "audio_callback_policy.h"
#include "audio_backend.h"

#include "opus_network_clock.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

void test_continuous_callback(unsigned long frame_count) {
    std::vector<float> input(frame_count);
    std::vector<float> output(frame_count, 0.0F);
    for (unsigned long frame = 0; frame < frame_count; ++frame) {
        input[frame] = static_cast<float>(frame + 1);
    }

    unsigned long processed = 0;
    unsigned long chunks = 0;
    for_each_audio_callback_chunk(
        frame_count, [&](unsigned long offset, unsigned long chunk_frames) {
            require(offset == processed, "callback chunks must be contiguous");
            require(chunk_frames > 0, "callback chunks must not be empty");
            require(chunk_frames <= opus_network_clock::STABLE_FRAME_COUNT,
                    "callback chunk exceeds runtime scratch-buffer capacity");
            std::copy_n(input.data() + offset, chunk_frames, output.data() + offset);
            processed += chunk_frames;
            ++chunks;
        });

    require(processed == frame_count, "the complete device callback must be processed");
    require(output == input, "callback output must have no zero or discontinuous tail");
    const auto expected_chunks =
        (frame_count + opus_network_clock::STABLE_FRAME_COUNT - 1) /
        opus_network_clock::STABLE_FRAME_COUNT;
    require(chunks == expected_chunks, "callback chunk count is incorrect");
}

void test_physical_callback_shares_decode_budget_across_chunks() {
    AudioCallbackWorkBudget work_budget;
    std::size_t decodes = 0;
    for_each_audio_callback_chunk(
        2048, [&](unsigned long, unsigned long) {
            for (std::size_t i = 0; i < 128; ++i) {
                decodes += consume_audio_decode_budget(work_budget) ? 1 : 0;
            }
        });
    require(decodes == 256 && work_budget.remaining_decodes == 0,
            "chunking must share one physical-callback decode budget");
    require(!consume_audio_decode_budget(work_budget),
            "exhausted callback budget must reject PLC and packet decode work");
}

int main() {
    test_continuous_callback(0);
    test_continuous_callback(120);
    test_continuous_callback(960);
    test_continuous_callback(1024);
    test_continuous_callback(2048);
    test_physical_callback_shares_decode_budget_across_chunks();
    std::cout << "audio callback policy self-test passed\n";
    return 0;
}
