#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <sodium.h>

#include "opus_network_clock.h"
#include "performer_join_token.h"
#include "protocol.h"

namespace session_crypto {

using SessionKey = std::array<unsigned char, 32>;
using E2EPublicKey = std::array<unsigned char, E2E_PUBLIC_KEY_BYTES>;
using E2ESecretKey = std::array<unsigned char, E2E_SECRET_KEY_BYTES>;

namespace detail {

inline std::vector<unsigned char> bytes_from_string(const std::string& value) {
    return std::vector<unsigned char>(value.begin(), value.end());
}

inline void append_string(std::vector<unsigned char>& out, const std::string& value) {
    out.insert(out.end(), value.begin(), value.end());
}

inline std::vector<unsigned char> hmac_sha256_bytes(
    const std::vector<unsigned char>& key_input,
    const std::vector<unsigned char>& message) {
    constexpr size_t block_size = 64;
    std::vector<unsigned char> key = key_input;
    if (key.size() > block_size) {
        key = performer_join_token::sha256(key);
    }
    key.resize(block_size, 0);

    std::vector<unsigned char> outer_key_pad(block_size);
    std::vector<unsigned char> inner_key_pad(block_size);
    for (size_t i = 0; i < block_size; ++i) {
        outer_key_pad[i] = key[i] ^ 0x5c;
        inner_key_pad[i] = key[i] ^ 0x36;
    }

    std::vector<unsigned char> inner(inner_key_pad);
    inner.insert(inner.end(), message.begin(), message.end());
    const auto inner_hash = performer_join_token::sha256(inner);

    std::vector<unsigned char> outer(outer_key_pad);
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return performer_join_token::sha256(outer);
}

inline SessionKey session_key_from_digest(const std::vector<unsigned char>& digest) {
    SessionKey key{};
    const size_t copy_bytes = std::min(key.size(), digest.size());
    std::copy_n(digest.begin(), copy_bytes, key.begin());
    return key;
}

inline bool ensure_sodium_initialized() {
    return sodium_init() >= 0;
}

}  // namespace detail

struct SecureAudioMetadata {
    uint32_t sender_id = 0;
    uint32_t sequence = 0;
    uint32_t sample_rate = 0;
    uint16_t frame_count = 0;
    uint16_t plaintext_bytes = 0;
    uint8_t channels = 0;
    AudioCodec codec = AudioCodec::Opus;
    int64_t capture_server_time_ns = 0;
};

struct SecureControlMetadata {
    uint32_t sender_id = 0;
    uint32_t sequence = 0;
    uint16_t plaintext_bytes = 0;
};

inline bool parse_secure_audio_header(const unsigned char* packet, size_t packet_len,
                                      SecureAudioMetadata& metadata,
                                      uint16_t& encrypted_bytes,
                                      std::string* reason = nullptr) {
    metadata = {};
    encrypted_bytes = 0;
    if (packet == nullptr || packet_len < sizeof(SecureAudioHdr)) {
        if (reason != nullptr) {
            *reason = "short secure audio header";
        }
        return false;
    }

    SecureAudioHdr hdr{};
    std::memcpy(&hdr, packet, sizeof(hdr));
    if (hdr.magic != SECURE_AUDIO_MAGIC) {
        if (reason != nullptr) {
            *reason = "wrong secure audio magic";
        }
        return false;
    }
    if (hdr.reserved != 0) {
        if (reason != nullptr) {
            *reason = "nonzero secure audio reserved field";
        }
        return false;
    }
    if (hdr.sender_id == 0 || hdr.plaintext_bytes == 0 ||
        hdr.encrypted_bytes < SECURE_PACKET_TAG_BYTES ||
        hdr.encrypted_bytes != hdr.plaintext_bytes + SECURE_PACKET_TAG_BYTES ||
        packet_len != sizeof(SecureAudioHdr) + hdr.encrypted_bytes) {
        if (reason != nullptr) {
            *reason = "invalid secure audio lengths";
        }
        return false;
    }
    if (hdr.sample_rate != opus_network_clock::SAMPLE_RATE || hdr.channels != 1 ||
        hdr.codec != AudioCodec::Opus ||
        !opus_network_clock::is_supported_frame_count(hdr.sample_rate, hdr.frame_count)) {
        if (reason != nullptr) {
            *reason = "unsupported secure audio shape";
        }
        return false;
    }

    metadata.sender_id = hdr.sender_id;
    metadata.sequence = hdr.sequence;
    metadata.sample_rate = hdr.sample_rate;
    metadata.frame_count = hdr.frame_count;
    metadata.plaintext_bytes = hdr.plaintext_bytes;
    metadata.channels = hdr.channels;
    metadata.codec = hdr.codec;
    metadata.capture_server_time_ns = hdr.capture_server_time_ns;
    encrypted_bytes = hdr.encrypted_bytes;
    return true;
}

inline bool parse_secure_control_header(const unsigned char* packet, size_t packet_len,
                                        SecureControlMetadata& metadata,
                                        uint16_t& encrypted_bytes,
                                        std::string* reason = nullptr) {
    metadata = {};
    encrypted_bytes = 0;
    if (packet == nullptr || packet_len < sizeof(SecureControlHdr)) {
        if (reason != nullptr) {
            *reason = "short secure control header";
        }
        return false;
    }

    SecureControlHdr hdr{};
    std::memcpy(&hdr, packet, sizeof(hdr));
    if (hdr.magic != SECURE_CONTROL_MAGIC) {
        if (reason != nullptr) {
            *reason = "wrong secure control magic";
        }
        return false;
    }
    if (hdr.reserved != 0) {
        if (reason != nullptr) {
            *reason = "nonzero secure control reserved field";
        }
        return false;
    }
    if (hdr.sender_id == 0 || hdr.plaintext_bytes == 0 ||
        hdr.encrypted_bytes < SECURE_PACKET_TAG_BYTES ||
        hdr.encrypted_bytes != hdr.plaintext_bytes + SECURE_PACKET_TAG_BYTES ||
        packet_len != sizeof(SecureControlHdr) + hdr.encrypted_bytes) {
        if (reason != nullptr) {
            *reason = "invalid secure control lengths";
        }
        return false;
    }

    metadata.sender_id = hdr.sender_id;
    metadata.sequence = hdr.sequence;
    metadata.plaintext_bytes = hdr.plaintext_bytes;
    encrypted_bytes = hdr.encrypted_bytes;
    return true;
}

inline std::string nonce_replay_key(const performer_join_token::Claims& claims) {
    return std::to_string(claims.server_id.size()) + ":" + claims.server_id + "|" +
           std::to_string(claims.room_id.size()) + ":" + claims.room_id + "|" +
           std::to_string(claims.room_instance_id.size()) + ":" +
           claims.room_instance_id + "|" + std::to_string(claims.access_epoch) + "|" +
           std::to_string(claims.profile_id.size()) + ":" + claims.profile_id + "|" +
           std::to_string(claims.nonce.size()) + ":" + claims.nonce;
}

inline SessionKey derive_media_key_from_secret(const std::string& room_id,
                                               const std::string& room_instance_id,
                                               const std::string& media_secret) {
    std::vector<unsigned char> message;
    detail::append_string(message, "sesivo-e2e-media-v3|");
    detail::append_string(message, room_id);
    detail::append_string(message, "|");
    detail::append_string(message, room_instance_id);
    const auto digest = detail::hmac_sha256_bytes(
        detail::bytes_from_string(media_secret), message);
    return detail::session_key_from_digest(digest);
}

inline bool make_e2e_keypair(E2EPublicKey& public_key, E2ESecretKey& secret_key) {
    static_assert(E2E_PUBLIC_KEY_BYTES == crypto_box_PUBLICKEYBYTES);
    static_assert(E2E_SECRET_KEY_BYTES == crypto_box_SECRETKEYBYTES);
    if (!detail::ensure_sodium_initialized()) {
        public_key.fill(0);
        secret_key.fill(0);
        return false;
    }
    return crypto_box_keypair(public_key.data(), secret_key.data()) == 0;
}

inline bool seal_key_envelope(const E2EPublicKey& recipient_public_key,
                              const unsigned char* plaintext,
                              size_t plaintext_len,
                              unsigned char* encrypted_out,
                              size_t encrypted_capacity,
                              size_t& encrypted_len) {
    encrypted_len = 0;
    if (plaintext == nullptr || encrypted_out == nullptr || plaintext_len == 0 ||
        plaintext_len > E2E_KEY_ENVELOPE_MAX_BYTES - crypto_box_SEALBYTES ||
        encrypted_capacity < plaintext_len + crypto_box_SEALBYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    if (crypto_box_seal(encrypted_out, plaintext,
                        static_cast<unsigned long long>(plaintext_len),
                        recipient_public_key.data()) != 0) {
        return false;
    }
    encrypted_len = plaintext_len + crypto_box_SEALBYTES;
    return true;
}

inline bool open_key_envelope(const E2EPublicKey& recipient_public_key,
                              const E2ESecretKey& recipient_secret_key,
                              const unsigned char* encrypted,
                              size_t encrypted_len,
                              unsigned char* plaintext_out,
                              size_t plaintext_capacity,
                              size_t& plaintext_len) {
    plaintext_len = 0;
    if (encrypted == nullptr || plaintext_out == nullptr ||
        encrypted_len <= crypto_box_SEALBYTES ||
        encrypted_len > E2E_KEY_ENVELOPE_MAX_BYTES ||
        plaintext_capacity < encrypted_len - crypto_box_SEALBYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    const size_t expected_plaintext_len = encrypted_len - crypto_box_SEALBYTES;
    if (crypto_box_seal_open(plaintext_out, encrypted,
                             static_cast<unsigned long long>(encrypted_len),
                             recipient_public_key.data(),
                             recipient_secret_key.data()) != 0) {
        return false;
    }
    plaintext_len = expected_plaintext_len;
    return true;
}

inline bool seal_audio_packet(const SessionKey& key,
                              const SecureAudioMetadata& metadata,
                              const unsigned char* plaintext, size_t plaintext_len,
                              unsigned char* out, size_t out_capacity,
                              size_t& bytes_written) {
    static_assert(SECURE_PACKET_NONCE_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    static_assert(SECURE_PACKET_TAG_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_ABYTES);
    static_assert(SessionKey{}.size() ==
                  crypto_aead_chacha20poly1305_IETF_KEYBYTES);

    bytes_written = 0;
    if (metadata.sender_id == 0 || metadata.plaintext_bytes != plaintext_len ||
        metadata.sample_rate != opus_network_clock::SAMPLE_RATE ||
        metadata.channels != 1 || metadata.codec != AudioCodec::Opus ||
        !opus_network_clock::is_supported_frame_count(metadata.sample_rate,
                                                      metadata.frame_count) ||
        out == nullptr ||
        (plaintext_len > 0 && plaintext == nullptr) ||
        plaintext_len >
            std::numeric_limits<uint16_t>::max() - SECURE_PACKET_TAG_BYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    const size_t encrypted_bytes = plaintext_len + SECURE_PACKET_TAG_BYTES;
    const size_t required =
        SECURE_PACKET_HEADER_BYTES + encrypted_bytes;
    if (out_capacity < required) {
        return false;
    }

    SecureAudioHdr hdr{};
    hdr.magic = SECURE_AUDIO_MAGIC;
    hdr.sender_id = metadata.sender_id;
    hdr.sequence = metadata.sequence;
    hdr.sample_rate = metadata.sample_rate;
    hdr.frame_count = metadata.frame_count;
    hdr.plaintext_bytes = static_cast<uint16_t>(plaintext_len);
    hdr.encrypted_bytes = static_cast<uint16_t>(encrypted_bytes);
    hdr.channels = metadata.channels;
    hdr.codec = metadata.codec;
    hdr.capture_server_time_ns = metadata.capture_server_time_ns;
    randombytes_buf(hdr.nonce.data(), hdr.nonce.size());
    std::memcpy(out, &hdr, sizeof(hdr));

    unsigned long long actual_encrypted_bytes = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out + SECURE_PACKET_HEADER_BYTES, &actual_encrypted_bytes, plaintext,
            static_cast<unsigned long long>(plaintext_len), out,
            SECURE_PACKET_HEADER_BYTES, nullptr,
            reinterpret_cast<const unsigned char*>(hdr.nonce.data()), key.data()) != 0 ||
        actual_encrypted_bytes != encrypted_bytes) {
        return false;
    }
    bytes_written = required;
    return true;
}

inline bool open_audio_packet(const SessionKey& key, const unsigned char* packet,
                              size_t packet_len, SecureAudioMetadata& metadata,
                              unsigned char* plaintext_out,
                              size_t plaintext_capacity, size_t& plaintext_len) {
    metadata = {};
    plaintext_len = 0;
    if (packet == nullptr || plaintext_out == nullptr ||
        packet_len < SECURE_PACKET_HEADER_BYTES + SECURE_PACKET_TAG_BYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    uint16_t encrypted_bytes = 0;
    if (!parse_secure_audio_header(packet, packet_len, metadata, encrypted_bytes) ||
        plaintext_capacity < metadata.plaintext_bytes) {
        return false;
    }

    SecureAudioHdr hdr{};
    std::memcpy(&hdr, packet, sizeof(hdr));
    unsigned long long actual_plaintext_bytes = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            plaintext_out, &actual_plaintext_bytes, nullptr,
            packet + SECURE_PACKET_HEADER_BYTES, encrypted_bytes, packet,
            SECURE_PACKET_HEADER_BYTES,
            reinterpret_cast<const unsigned char*>(hdr.nonce.data()), key.data()) != 0) {
        return false;
    }
    if (actual_plaintext_bytes != metadata.plaintext_bytes) {
        return false;
    }
    plaintext_len = static_cast<size_t>(actual_plaintext_bytes);
    return true;
}

inline bool seal_control_packet(const SessionKey& key,
                                const SecureControlMetadata& metadata,
                                const unsigned char* plaintext, size_t plaintext_len,
                                unsigned char* out, size_t out_capacity,
                                size_t& bytes_written) {
    static_assert(SECURE_PACKET_NONCE_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    static_assert(SECURE_PACKET_TAG_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_ABYTES);
    static_assert(SessionKey{}.size() ==
                  crypto_aead_chacha20poly1305_IETF_KEYBYTES);

    bytes_written = 0;
    if (metadata.sender_id == 0 || metadata.plaintext_bytes != plaintext_len ||
        out == nullptr || (plaintext_len > 0 && plaintext == nullptr) ||
        plaintext_len >
            std::numeric_limits<uint16_t>::max() - SECURE_PACKET_TAG_BYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    const size_t encrypted_bytes = plaintext_len + SECURE_PACKET_TAG_BYTES;
    const size_t required = SECURE_CONTROL_HEADER_BYTES + encrypted_bytes;
    if (out_capacity < required) {
        return false;
    }

    SecureControlHdr hdr{};
    hdr.magic = SECURE_CONTROL_MAGIC;
    hdr.sender_id = metadata.sender_id;
    hdr.sequence = metadata.sequence;
    hdr.plaintext_bytes = static_cast<uint16_t>(plaintext_len);
    hdr.encrypted_bytes = static_cast<uint16_t>(encrypted_bytes);
    randombytes_buf(hdr.nonce.data(), hdr.nonce.size());
    std::memcpy(out, &hdr, sizeof(hdr));

    unsigned long long actual_encrypted_bytes = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out + SECURE_CONTROL_HEADER_BYTES, &actual_encrypted_bytes, plaintext,
            static_cast<unsigned long long>(plaintext_len), out,
            SECURE_CONTROL_HEADER_BYTES, nullptr,
            reinterpret_cast<const unsigned char*>(hdr.nonce.data()), key.data()) != 0 ||
        actual_encrypted_bytes != encrypted_bytes) {
        return false;
    }
    bytes_written = required;
    return true;
}

inline bool open_control_packet(const SessionKey& key, const unsigned char* packet,
                                size_t packet_len, SecureControlMetadata& metadata,
                                unsigned char* plaintext_out,
                                size_t plaintext_capacity, size_t& plaintext_len) {
    metadata = {};
    plaintext_len = 0;
    if (packet == nullptr || plaintext_out == nullptr ||
        packet_len < SECURE_CONTROL_HEADER_BYTES + SECURE_PACKET_TAG_BYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    uint16_t encrypted_bytes = 0;
    if (!parse_secure_control_header(packet, packet_len, metadata, encrypted_bytes) ||
        plaintext_capacity < metadata.plaintext_bytes) {
        return false;
    }

    SecureControlHdr hdr{};
    std::memcpy(&hdr, packet, sizeof(hdr));
    unsigned long long actual_plaintext_bytes = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            plaintext_out, &actual_plaintext_bytes, nullptr,
            packet + SECURE_CONTROL_HEADER_BYTES, encrypted_bytes, packet,
            SECURE_CONTROL_HEADER_BYTES,
            reinterpret_cast<const unsigned char*>(hdr.nonce.data()), key.data()) != 0) {
        return false;
    }
    if (actual_plaintext_bytes != metadata.plaintext_bytes) {
        return false;
    }
    plaintext_len = static_cast<size_t>(actual_plaintext_bytes);
    return true;
}

}  // namespace session_crypto
