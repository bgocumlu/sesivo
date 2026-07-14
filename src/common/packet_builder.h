#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "audio_packet.h"
#include "protocol.h"

// Helper utilities for building network packets
namespace packet_builder {

// Create a participant leave control packet
inline std::shared_ptr<std::vector<unsigned char>> create_participant_leave_packet(
    uint32_t participant_id) {
    CtrlHdr chdr{};
    chdr.magic          = CTRL_MAGIC;
    chdr.type           = CtrlHdr::Cmd::PARTICIPANT_LEAVE;
    chdr.participant_id = participant_id;

    auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(CtrlHdr));
    std::memcpy(buf->data(), &chdr, sizeof(CtrlHdr));
    return buf;
}

// Embed sender ID into an audio packet
inline void embed_sender_id(unsigned char* packet_data, uint32_t sender_id) {
    std::memcpy(packet_data + sizeof(MsgHdr), &sender_id, sizeof(uint32_t));
}

// Extract sender ID from an audio packet
inline uint32_t extract_sender_id(const unsigned char* packet_data) {
    uint32_t sender_id;
    std::memcpy(&sender_id, packet_data + sizeof(MsgHdr), sizeof(uint32_t));
    return sender_id;
}

template <size_t N>
inline void write_fixed(Bytes<N>& target, const std::string& value) {
    static_assert(N > 0);
    const size_t copy_bytes = std::min(value.size(), target.size() - 1);
    std::memcpy(target.data(), value.data(), copy_bytes);
    target[copy_bytes] = '\0';
}

template <size_t N>
inline bool write_fixed_checked(Bytes<N>& target, const std::string& value) {
    static_assert(N > 0);
    if (value.size() >= target.size()) {
        return false;
    }
    std::memcpy(target.data(), value.data(), value.size());
    target[value.size()] = '\0';
    return true;
}

inline std::shared_ptr<std::vector<unsigned char>> create_participant_info_packet(
    uint32_t participant_id, const std::string& profile_id, const std::string& display_name,
    uint32_t capabilities = 0,
    const Bytes<E2E_PUBLIC_KEY_BYTES>& key_public = {}) {
    ParticipantInfoCapsHdr info{};
    info.magic          = CTRL_MAGIC;
    info.type           = CtrlHdr::Cmd::PARTICIPANT_INFO;
    info.participant_id = participant_id;
    write_fixed(info.profile_id, profile_id);
    write_fixed(info.display_name, display_name);
    info.capabilities = capabilities & AUDIO_SUPPORTED_CAPABILITIES;
    info.key_public = key_public;

    auto buf = std::make_shared<std::vector<unsigned char>>(sizeof(ParticipantInfoCapsHdr));
    std::memcpy(buf->data(), &info, sizeof(ParticipantInfoCapsHdr));
    return buf;
}

inline uint16_t extract_audio_payload_bytes(const unsigned char* packet_data, size_t len) {
    const auto parsed = audio_packet::parse_audio_header(packet_data, len);
    return parsed.valid ? parsed.payload_bytes : 0;
}

inline const unsigned char* audio_payload(const unsigned char* packet_data, size_t len) {
    return audio_packet::audio_payload(packet_data, len);
}

}  // namespace packet_builder
