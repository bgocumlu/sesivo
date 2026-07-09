#include "wav_file_playback.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.00001F;
}

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool require_near(float actual, float expected, const char* message) {
    if (!near(actual, expected)) {
        std::cerr << "FAIL: " << message << ": expected " << expected << ", got " << actual
                  << '\n';
        return false;
    }
    return true;
}

void write_u16(std::ofstream& file, uint16_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void write_u32(std::ofstream& file, uint32_t value) {
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
    file.put(static_cast<char>((value >> 16) & 0xFF));
    file.put(static_cast<char>((value >> 24) & 0xFF));
}

struct TempWav {
    explicit TempWav(std::string name) {
        path = std::filesystem::temp_directory_path() / std::move(name);
    }

    ~TempWav() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    std::filesystem::path path;
};

std::string unique_name(const char* label) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("sesivo_") + label + "_" + std::to_string(tick) + ".wav";
}

bool write_mono_wav(const std::filesystem::path& path, uint32_t sample_rate,
                    const std::vector<int16_t>& samples) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    constexpr uint16_t channels = 1;
    constexpr uint16_t bits_per_sample = 16;
    constexpr uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));

    file.write("RIFF", 4);
    write_u32(file, 36 + data_bytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    write_u32(file, 16);
    write_u16(file, 1);
    write_u16(file, channels);
    write_u32(file, sample_rate);
    write_u32(file, byte_rate);
    write_u16(file, block_align);
    write_u16(file, bits_per_sample);
    file.write("data", 4);
    write_u32(file, data_bytes);
    file.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return file.good();
}

float pcm_float(int16_t value) {
    return static_cast<float>(value) / 32768.0F;
}

bool resampler_advances_by_source_over_target_ratio() {
    const std::array<float, 3> input{0.0F, 10.0F, 20.0F};
    std::array<float, 4> output{};
    double position = 0.0;

    const auto result = audio_resample::linear(input.data(), output.data(), 0.5F,
                                               static_cast<int>(input.size()),
                                               static_cast<int>(output.size()), position);

    return require_near(output[0], 0.0F, "upsample output 0") &&
           require_near(output[1], 5.0F, "upsample output 1") &&
           require_near(output[2], 10.0F, "upsample output 2") &&
           require_near(output[3], 15.0F, "upsample output 3") &&
           require_near(static_cast<float>(position), 2.0F, "upsample final source position") &&
           require_near(static_cast<float>(result.source_frames_consumed), 2.0F,
                        "upsample consumed source frames") &&
           require(result.output_frames_from_input == 4, "upsample valid output frames");
}

bool resampler_downsamples_by_skipping_source_frames() {
    const std::array<float, 7> input{0.0F, 10.0F, 20.0F, 30.0F,
                                    40.0F, 50.0F, 60.0F};
    std::array<float, 3> output{};
    double position = 0.0;

    const auto result = audio_resample::linear(input.data(), output.data(), 2.0F,
                                               static_cast<int>(input.size()),
                                               static_cast<int>(output.size()), position);

    return require_near(output[0], 0.0F, "downsample output 0") &&
           require_near(output[1], 20.0F, "downsample output 1") &&
           require_near(output[2], 40.0F, "downsample output 2") &&
           require_near(static_cast<float>(position), 6.0F, "downsample final source position") &&
           require_near(static_cast<float>(result.source_frames_consumed), 6.0F,
                        "downsample consumed source frames") &&
           require(result.output_frames_from_input == 3, "downsample valid output frames");
}

bool wav_read_resamples_non_48k_file_at_correct_speed() {
    TempWav wav(unique_name("resample"));
    if (!require(write_mono_wav(wav.path, 96000, {0, 4096, 8192, 12288, 16384, 20480}),
                 "write temp wav")) {
        return false;
    }

    WavFilePlayback playback;
    if (!require(playback.load_file(wav.path.string()), "load 96k wav") ||
        !require(playback.get_sample_rate() == 96000, "metadata sample rate") ||
        !require(playback.get_channels() == 1, "metadata channels") ||
        !require(playback.get_bits_per_sample() == 16, "metadata bits") ||
        !require(playback.get_total_frames() == 6, "metadata total frames")) {
        return false;
    }

    std::array<float, 3> output{};
    playback.play();
    const int frames_read = playback.read(output.data(), static_cast<int>(output.size()), 48000);

    return require(frames_read == 3, "96k read frame count") &&
           require_near(output[0], pcm_float(0), "96k output 0") &&
           require_near(output[1], pcm_float(8192), "96k output 1") &&
           require_near(output[2], pcm_float(16384), "96k output 2") &&
           require(playback.get_position() == 6, "96k final position");
}

bool wav_resampled_read_reports_partial_eof() {
    TempWav wav(unique_name("partial"));
    if (!require(write_mono_wav(wav.path, 96000, {4096, 8192, 12288}),
                 "write partial temp wav")) {
        return false;
    }

    WavFilePlayback playback;
    if (!require(playback.load_file(wav.path.string()), "load partial 96k wav")) {
        return false;
    }

    std::array<float, 3> output{99.0F, 99.0F, 99.0F};
    playback.play();
    const int frames_read = playback.read(output.data(), static_cast<int>(output.size()), 48000);

    return require(frames_read == 2, "partial EOF frame count") &&
           require_near(output[0], pcm_float(4096), "partial output 0") &&
           require_near(output[1], pcm_float(12288), "partial output 1") &&
           require_near(output[2], 0.0F, "partial padded output") &&
           require(!playback.is_playing(), "partial EOF stops playback") &&
           require(playback.get_position() == 3, "partial EOF position");
}
}  // namespace

int main() {
    if (!resampler_advances_by_source_over_target_ratio()) {
        return 1;
    }
    if (!resampler_downsamples_by_skipping_source_frames()) {
        return 1;
    }
    if (!wav_read_resamples_non_48k_file_at_correct_speed()) {
        return 1;
    }
    if (!wav_resampled_read_reports_partial_eof()) {
        return 1;
    }

    std::cout << "wav file playback self-test passed\n";
    return 0;
}
