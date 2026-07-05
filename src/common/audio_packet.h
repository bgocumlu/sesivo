#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "opus_network_clock.h"
#include "protocol.h"

namespace audio_packet {

inline constexpr size_t header_size() {
    return sizeof(AudioHdrV3) - AUDIO_BUF_SIZE;
}

inline constexpr size_t v3_header_size() {
    return header_size();
}

inline constexpr size_t redundant_header_size() {
    return sizeof(AudioRedundantHdr);
}

struct ParsedAudioHeader {
    bool valid = false;
    uint32_t magic = 0;
    uint32_t sender_id = 0;
    uint32_t sequence = 0;
    uint32_t sample_rate = 0;
    uint16_t frame_count = 0;
    uint16_t payload_bytes = 0;
    uint8_t channels = 0;
    AudioCodec codec = AudioCodec::Opus;
    bool capture_timestamp_valid = false;
    int64_t capture_server_time_ns = 0;
    size_t header_size = 0;
};

inline ParsedAudioHeader parse_audio_header(const unsigned char* data, size_t len) {
    ParsedAudioHeader parsed{};
    if (data == nullptr || len < sizeof(MsgHdr)) {
        return parsed;
    }

    MsgHdr msg{};
    std::memcpy(&msg, data, sizeof(MsgHdr));
    if (msg.magic != AUDIO_V3_MAGIC || len < header_size()) {
        return parsed;
    }

    AudioHdrV3 hdr{};
    std::memcpy(&hdr, data, header_size());
    parsed.valid = true;
    parsed.magic = hdr.magic;
    parsed.sender_id = hdr.sender_id;
    parsed.sequence = hdr.sequence;
    parsed.sample_rate = hdr.sample_rate;
    parsed.frame_count = hdr.frame_count;
    parsed.payload_bytes = hdr.payload_bytes;
    parsed.channels = hdr.channels;
    parsed.codec = hdr.codec;
    parsed.capture_timestamp_valid = hdr.capture_server_time_ns > 0;
    parsed.capture_server_time_ns = hdr.capture_server_time_ns;
    parsed.header_size = header_size();
    return parsed;
}

inline bool validate_audio_packet_shape(const ParsedAudioHeader& hdr,
                                        std::string* reason = nullptr) {
    if (hdr.sample_rate != opus_network_clock::SAMPLE_RATE || hdr.channels != 1 ||
        hdr.frame_count == 0) {
        if (reason != nullptr) {
            *reason = "unsupported audio shape";
        }
        return false;
    }

    if (hdr.codec == AudioCodec::Opus) {
        if (!opus_network_clock::is_supported_frame_count(hdr.sample_rate, hdr.frame_count)) {
            if (reason != nullptr) {
                *reason = "unsupported opus frame count";
            }
            return false;
        }
        if (hdr.payload_bytes == 0) {
            if (reason != nullptr) {
                *reason = "empty opus payload";
            }
            return false;
        }
        return true;
    }

    if (hdr.codec == AudioCodec::PcmInt16) {
        const size_t expected_payload =
            static_cast<size_t>(hdr.frame_count) * hdr.channels * sizeof(int16_t);
        if (expected_payload > AUDIO_BUF_SIZE || hdr.payload_bytes != expected_payload) {
            if (reason != nullptr) {
                *reason = "pcm payload shape mismatch";
            }
            return false;
        }
        return true;
    }

    if (reason != nullptr) {
        *reason = "invalid codec";
    }
    return false;
}

inline bool validate_audio_packet_bytes(const unsigned char* data, size_t len,
                                        std::string* reason = nullptr) {
    const auto hdr = parse_audio_header(data, len);
    if (!hdr.valid) {
        if (reason != nullptr) {
            *reason = "short header";
        }
        return false;
    }
    if (hdr.payload_bytes > AUDIO_BUF_SIZE) {
        if (reason != nullptr) {
            *reason = "payload too large";
        }
        return false;
    }

    const size_t expected = hdr.header_size + hdr.payload_bytes;
    if (len != expected) {
        if (reason != nullptr) {
            *reason = "length mismatch";
        }
        return false;
    }
    return validate_audio_packet_shape(hdr, reason);
}

inline const unsigned char* audio_payload(const unsigned char* data, size_t len) {
    const auto hdr = parse_audio_header(data, len);
    return hdr.valid ? data + hdr.header_size : nullptr;
}

inline bool next_redundant_child(const unsigned char* data, size_t len, size_t& offset,
                                 const unsigned char*& child, size_t& child_len,
                                 std::string* reason = nullptr) {
    if (offset + sizeof(MsgHdr) > len) {
        if (reason != nullptr) {
            *reason = "short redundant child header";
        }
        return false;
    }

    child = data + offset;
    MsgHdr msg{};
    std::memcpy(&msg, child, sizeof(MsgHdr));
    if (msg.magic != AUDIO_V3_MAGIC) {
        if (reason != nullptr) {
            *reason = "redundant child wrong magic";
        }
        return false;
    }

    const auto child_hdr = parse_audio_header(child, len - offset);
    if (!child_hdr.valid) {
        if (reason != nullptr) {
            *reason = "short redundant child header";
        }
        return false;
    }
    if (child_hdr.payload_bytes > AUDIO_BUF_SIZE) {
        if (reason != nullptr) {
            *reason = "redundant child payload too large";
        }
        return false;
    }

    child_len = child_hdr.header_size + child_hdr.payload_bytes;
    if (offset + child_len > len) {
        if (reason != nullptr) {
            *reason = "truncated redundant child";
        }
        return false;
    }
    if (!validate_audio_packet_shape(child_hdr, reason)) {
        return false;
    }

    offset += child_len;
    return true;
}

inline bool validate_redundant_audio_packet_bytes(const unsigned char* data, size_t len,
                                                  std::string* reason = nullptr) {
    if (data == nullptr) {
        if (reason != nullptr) {
            *reason = "null packet";
        }
        return false;
    }
    if (len < redundant_header_size()) {
        if (reason != nullptr) {
            *reason = "short redundant header";
        }
        return false;
    }

    AudioRedundantHdr hdr{};
    std::memcpy(&hdr, data, redundant_header_size());
    if (hdr.magic != AUDIO_REDUNDANT_MAGIC) {
        if (reason != nullptr) {
            *reason = "wrong redundant magic";
        }
        return false;
    }
    if (hdr.packet_count == 0 || hdr.packet_count > MAX_AUDIO_REDUNDANT_PACKETS) {
        if (reason != nullptr) {
            *reason = "invalid redundant child count";
        }
        return false;
    }

    size_t offset = redundant_header_size();
    for (uint8_t i = 0; i < hdr.packet_count; ++i) {
        const unsigned char* child = nullptr;
        size_t child_len = 0;
        if (!next_redundant_child(data, len, offset, child, child_len, reason)) {
            return false;
        }
    }
    if (offset != len) {
        if (reason != nullptr) {
            *reason = "redundant length mismatch";
        }
        return false;
    }
    return true;
}

template <typename Func>
inline bool for_each_redundant_audio_child(const unsigned char* data, size_t len, Func&& func,
                                           std::string* reason = nullptr) {
    if (!validate_redundant_audio_packet_bytes(data, len, reason)) {
        return false;
    }

    AudioRedundantHdr hdr{};
    std::memcpy(&hdr, data, redundant_header_size());
    size_t offset = redundant_header_size();
    for (uint8_t i = 0; i < hdr.packet_count; ++i) {
        const unsigned char* child = nullptr;
        size_t child_len = 0;
        if (!next_redundant_child(data, len, offset, child, child_len, reason)) {
            return false;
        }
        func(child, child_len, i);
    }
    return true;
}

template <typename Func>
inline bool for_each_redundant_audio_child_reverse(const unsigned char* data, size_t len,
                                                   Func&& func,
                                                   std::string* reason = nullptr) {
    if (!validate_redundant_audio_packet_bytes(data, len, reason)) {
        return false;
    }

    AudioRedundantHdr hdr{};
    std::memcpy(&hdr, data, redundant_header_size());

    std::array<std::pair<const unsigned char*, size_t>, MAX_AUDIO_REDUNDANT_PACKETS>
        children{};
    size_t offset = redundant_header_size();
    for (uint8_t i = 0; i < hdr.packet_count; ++i) {
        const unsigned char* child = nullptr;
        size_t child_len = 0;
        if (!next_redundant_child(data, len, offset, child, child_len, reason)) {
            return false;
        }
        children[i] = {child, child_len};
    }

    for (uint8_t i = hdr.packet_count; i > 0; --i) {
        const auto& [child, child_len] = children[i - 1];
        func(child, child_len, static_cast<uint8_t>(i - 1));
    }
    return true;
}

template <typename Func>
inline bool for_each_redundant_audio_child(unsigned char* data, size_t len, Func&& func,
                                           std::string* reason = nullptr) {
    return for_each_redundant_audio_child(
        static_cast<const unsigned char*>(data), len,
        [&](const unsigned char* child, size_t child_len, uint8_t index) {
            func(const_cast<unsigned char*>(child), child_len, index);
        },
        reason);
}

inline bool embed_sender_id_in_redundant_audio_packet(unsigned char* data, size_t len,
                                                      uint32_t sender_id,
                                                      std::string* reason = nullptr) {
    return for_each_redundant_audio_child(
        data, len,
        [sender_id](unsigned char* child, size_t, uint8_t) {
            std::memcpy(child + sizeof(MsgHdr), &sender_id, sizeof(sender_id));
        },
        reason);
}

struct AudioPacketView {
    const unsigned char* data = nullptr;
    size_t size = 0;
};

inline bool write_audio_packet_v3(AudioCodec codec, uint32_t sequence, uint32_t sample_rate,
                                  uint16_t frame_count, uint8_t channels,
                                  const unsigned char* payload, uint16_t payload_bytes,
                                  int64_t capture_server_time_ns,
                                  unsigned char* out, size_t out_capacity,
                                  size_t& bytes_written) {
    bytes_written = 0;
    const size_t required = header_size() + payload_bytes;
    if (out == nullptr || out_capacity < required ||
        (payload_bytes > 0 && payload == nullptr)) {
        return false;
    }

    AudioHdrV3 hdr{};
    hdr.magic = AUDIO_V3_MAGIC;
    hdr.sender_id = 0;
    hdr.sequence = sequence;
    hdr.sample_rate = sample_rate;
    hdr.frame_count = frame_count;
    hdr.payload_bytes = payload_bytes;
    hdr.channels = channels;
    hdr.codec = codec;
    hdr.capture_server_time_ns = capture_server_time_ns;

    std::memcpy(out, &hdr, header_size());
    if (payload_bytes > 0) {
        std::memcpy(out + header_size(), payload, payload_bytes);
    }
    bytes_written = required;
    return true;
}

inline bool write_redundant_audio_packet(const AudioPacketView* packets, size_t packet_count,
                                         unsigned char* out, size_t out_capacity,
                                         size_t max_packet_bytes,
                                         size_t& bytes_written) {
    bytes_written = 0;
    if (packets == nullptr || packet_count == 0 ||
        packet_count > MAX_AUDIO_REDUNDANT_PACKETS || out == nullptr ||
        out_capacity < redundant_header_size()) {
        return false;
    }

    size_t total_bytes = redundant_header_size();
    size_t selected_count = 0;
    for (size_t i = 0; i < packet_count; ++i) {
        const auto& packet = packets[i];
        if (packet.data == nullptr ||
            !validate_audio_packet_bytes(packet.data, packet.size)) {
            return false;
        }
        if (max_packet_bytes > 0 && total_bytes + packet.size > max_packet_bytes) {
            if (selected_count == 0) {
                return false;
            }
            break;
        }
        if (total_bytes + packet.size > out_capacity) {
            return false;
        }
        total_bytes += packet.size;
        ++selected_count;
    }

    AudioRedundantHdr hdr{};
    hdr.magic = AUDIO_REDUNDANT_MAGIC;
    hdr.packet_count = static_cast<uint8_t>(selected_count);
    std::memcpy(out, &hdr, redundant_header_size());

    size_t offset = redundant_header_size();
    for (size_t i = 0; i < selected_count; ++i) {
        std::memcpy(out + offset, packets[i].data, packets[i].size);
        offset += packets[i].size;
    }
    bytes_written = total_bytes;
    return true;
}

inline std::shared_ptr<std::vector<unsigned char>> create_redundant_audio_packet(
    const std::vector<const std::vector<unsigned char>*>& packets,
    size_t max_packet_bytes = 0) {
    if (packets.empty() || packets.size() > MAX_AUDIO_REDUNDANT_PACKETS) {
        return nullptr;
    }

    size_t total_bytes = redundant_header_size();
    std::array<AudioPacketView, MAX_AUDIO_REDUNDANT_PACKETS> selected_packets{};
    size_t selected_count = 0;
    for (const auto* packet: packets) {
        if (packet == nullptr ||
            !validate_audio_packet_bytes(packet->data(), packet->size())) {
            return nullptr;
        }
        if (max_packet_bytes > 0 &&
            total_bytes + packet->size() > max_packet_bytes) {
            if (selected_count == 0) {
                return nullptr;
            }
            break;
        }
        selected_packets[selected_count] = {packet->data(), packet->size()};
        ++selected_count;
        total_bytes += packet->size();
    }

    auto redundant = std::make_shared<std::vector<unsigned char>>();
    redundant->resize(total_bytes);
    size_t bytes_written = 0;
    if (!write_redundant_audio_packet(selected_packets.data(), selected_count,
                                      redundant->data(), redundant->size(),
                                      max_packet_bytes, bytes_written)) {
        return nullptr;
    }
    redundant->resize(bytes_written);
    return redundant;
}

inline std::shared_ptr<std::vector<unsigned char>> create_audio_packet_v3(
    AudioCodec codec, uint32_t sequence, uint32_t sample_rate, uint16_t frame_count,
    uint8_t channels, const unsigned char* payload, uint16_t payload_bytes,
    int64_t capture_server_time_ns) {
    auto packet = std::make_shared<std::vector<unsigned char>>();
    packet->resize(header_size() + payload_bytes);
    size_t bytes_written = 0;
    if (!write_audio_packet_v3(codec, sequence, sample_rate, frame_count, channels,
                               payload, payload_bytes, capture_server_time_ns,
                               packet->data(), packet->size(), bytes_written)) {
        return nullptr;
    }
    packet->resize(bytes_written);
    return packet;
}

}  // namespace audio_packet
