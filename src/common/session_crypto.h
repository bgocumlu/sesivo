#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
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

inline bool ensure_sodium_initialized() {
    return sodium_init() >= 0;
}

inline std::optional<std::vector<unsigned char>> hmac_sha256_bytes(
    const std::vector<unsigned char>& key_input,
    const std::vector<unsigned char>& message) {
    if (!ensure_sodium_initialized()) {
        return std::nullopt;
    }
    std::vector<unsigned char> digest(crypto_auth_hmacsha256_BYTES);
    crypto_auth_hmacsha256_state state{};
    crypto_auth_hmacsha256_init(&state, performer_join_token::sodium_input_data(key_input),
                                key_input.size());
    crypto_auth_hmacsha256_update(
        &state, performer_join_token::sodium_input_data(message),
        static_cast<unsigned long long>(message.size()));
    crypto_auth_hmacsha256_final(&state, digest.data());
    return digest;
}

inline std::optional<SessionKey> session_key_from_digest(
    const std::vector<unsigned char>& digest) {
    if (digest.size() != SessionKey{}.size()) {
        return std::nullopt;
    }
    SessionKey key{};
    std::copy_n(digest.begin(), key.size(), key.begin());
    return key;
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
    uint32_t target_id = 0;
    uint32_t sequence = 0;
    uint32_t access_epoch = 0;
    uint16_t plaintext_bytes = 0;
    std::string media_key_commitment;
};

struct ChatMetadata {
    std::string room_id;
    std::string room_instance_id;
    uint32_t sender_id = 0;
    uint32_t access_epoch = 0;
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce{};
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
    const std::string media_key_commitment(hdr.media_key_commitment.begin(),
                                           hdr.media_key_commitment.end());
    if (hdr.sender_id == 0 || hdr.sequence == 0 || hdr.access_epoch == 0 ||
        !performer_join_token::valid_sha256_hex(media_key_commitment) ||
        hdr.plaintext_bytes == 0 ||
        hdr.encrypted_bytes < SECURE_PACKET_TAG_BYTES ||
        hdr.encrypted_bytes != hdr.plaintext_bytes + SECURE_PACKET_TAG_BYTES ||
        packet_len != sizeof(SecureControlHdr) + hdr.encrypted_bytes) {
        if (reason != nullptr) {
            *reason = "invalid secure control lengths";
        }
        return false;
    }

    metadata.sender_id = hdr.sender_id;
    metadata.target_id = hdr.target_id;
    metadata.sequence = hdr.sequence;
    metadata.access_epoch = hdr.access_epoch;
    metadata.plaintext_bytes = hdr.plaintext_bytes;
    metadata.media_key_commitment = media_key_commitment;
    encrypted_bytes = hdr.encrypted_bytes;
    return true;
}

inline std::string nonce_replay_key(const performer_join_token::Claims& claims) {
    return std::to_string(claims.server_id.size()) + ":" + claims.server_id + "|" +
           std::to_string(claims.room_id.size()) + ":" + claims.room_id + "|" +
           std::to_string(claims.room_instance_id.size()) + ":" +
           claims.room_instance_id + "|" + std::to_string(claims.access_epoch) + "|" +
           std::to_string(claims.media_key_commitment.size()) + ":" +
           claims.media_key_commitment + "|" +
           std::to_string(claims.profile_id.size()) + ":" + claims.profile_id + "|" +
           std::to_string(claims.nonce.size()) + ":" + claims.nonce;
}

inline std::optional<SessionKey> derive_media_key_from_secret(
    const std::string& room_id,
    const std::string& room_instance_id,
    const std::string& media_secret) {
    std::vector<unsigned char> message;
    detail::append_string(message, "sesivo-e2e-media-v3|");
    detail::append_string(message, room_id);
    detail::append_string(message, "|");
    detail::append_string(message, room_instance_id);
    const auto digest = detail::hmac_sha256_bytes(
        detail::bytes_from_string(media_secret), message);
    if (!digest.has_value()) {
        return std::nullopt;
    }
    return detail::session_key_from_digest(*digest);
}

inline std::optional<std::string> media_key_commitment(
    const std::string& media_secret) {
    if (media_secret.empty() || media_secret.size() > MEDIA_SECRET_MAX_BYTES) {
        return std::nullopt;
    }
    return performer_join_token::try_sha256_hex(media_secret);
}

inline bool media_secret_matches_commitment(
    const std::string& media_secret, const std::string& commitment) {
    const auto actual = media_key_commitment(media_secret);
    return actual.has_value() &&
           performer_join_token::constant_time_equal(*actual, commitment);
}

inline std::optional<SessionKey> derive_chat_key_from_secret(
    const std::string& room_id,
    const std::string& room_instance_id,
    const std::string& media_secret) {
    std::vector<unsigned char> message;
    detail::append_string(message, "sesivo-e2e-chat-v1|");
    detail::append_string(message, room_id);
    detail::append_string(message, "|");
    detail::append_string(message, room_instance_id);
    const auto digest = detail::hmac_sha256_bytes(
        detail::bytes_from_string(media_secret), message);
    if (!digest.has_value()) {
        return std::nullopt;
    }
    return detail::session_key_from_digest(*digest);
}

inline bool make_chat_nonce(Bytes<SECURE_PACKET_NONCE_BYTES>& nonce) {
    static_assert(SECURE_PACKET_NONCE_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    if (!detail::ensure_sodium_initialized()) {
        nonce.fill(0);
        return false;
    }
    randombytes_buf(nonce.data(), nonce.size());
    return true;
}

inline std::vector<unsigned char> chat_associated_data(const ChatMetadata& metadata) {
    std::vector<unsigned char> associated_data;
    detail::append_string(associated_data, std::to_string(metadata.room_id.size()));
    associated_data.push_back(':');
    detail::append_string(associated_data, metadata.room_id);
    associated_data.push_back('|');
    detail::append_string(associated_data, std::to_string(metadata.room_instance_id.size()));
    associated_data.push_back(':');
    detail::append_string(associated_data, metadata.room_instance_id);
    associated_data.push_back('|');
    detail::append_string(associated_data, std::to_string(metadata.sender_id));
    associated_data.push_back('|');
    detail::append_string(associated_data, std::to_string(metadata.access_epoch));
    associated_data.push_back('|');
    associated_data.insert(associated_data.end(), metadata.nonce.begin(), metadata.nonce.end());
    return associated_data;
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
    if (metadata.sender_id == 0 || metadata.sequence == 0 ||
        metadata.access_epoch == 0 ||
        !performer_join_token::valid_sha256_hex(metadata.media_key_commitment) ||
        metadata.plaintext_bytes != plaintext_len ||
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
    hdr.target_id = metadata.target_id;
    hdr.sequence = metadata.sequence;
    hdr.access_epoch = metadata.access_epoch;
    hdr.plaintext_bytes = static_cast<uint16_t>(plaintext_len);
    hdr.encrypted_bytes = static_cast<uint16_t>(encrypted_bytes);
    std::copy(metadata.media_key_commitment.begin(),
              metadata.media_key_commitment.end(),
              hdr.media_key_commitment.begin());
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

inline bool seal_chat_message(const SessionKey& key, const ChatMetadata& metadata,
                              const unsigned char* plaintext, size_t plaintext_len,
                              unsigned char* ciphertext_out, size_t ciphertext_capacity,
                              size_t& ciphertext_len) {
    static_assert(SECURE_PACKET_NONCE_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    static_assert(SECURE_PACKET_TAG_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_ABYTES);
    static_assert(SessionKey{}.size() ==
                  crypto_aead_chacha20poly1305_IETF_KEYBYTES);

    ciphertext_len = 0;
    if (metadata.room_id.empty() || metadata.room_instance_id.empty() ||
        metadata.sender_id == 0 || metadata.access_epoch == 0 ||
        plaintext == nullptr || plaintext_len == 0 ||
        plaintext_len > ROOM_CHAT_PLAINTEXT_MAX_BYTES ||
        ciphertext_out == nullptr ||
        ciphertext_capacity < plaintext_len + SECURE_PACKET_TAG_BYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    const auto associated_data = chat_associated_data(metadata);
    unsigned long long actual_ciphertext_bytes = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            ciphertext_out, &actual_ciphertext_bytes, plaintext,
            static_cast<unsigned long long>(plaintext_len),
            associated_data.data(),
            static_cast<unsigned long long>(associated_data.size()),
            nullptr,
            reinterpret_cast<const unsigned char*>(metadata.nonce.data()),
            key.data()) != 0 ||
        actual_ciphertext_bytes != plaintext_len + SECURE_PACKET_TAG_BYTES ||
        actual_ciphertext_bytes > ROOM_CHAT_CIPHERTEXT_MAX_BYTES) {
        return false;
    }
    ciphertext_len = static_cast<size_t>(actual_ciphertext_bytes);
    return true;
}

inline bool open_chat_message(const SessionKey& key, const ChatMetadata& metadata,
                              const unsigned char* ciphertext, size_t ciphertext_len,
                              unsigned char* plaintext_out, size_t plaintext_capacity,
                              size_t& plaintext_len) {
    static_assert(SECURE_PACKET_NONCE_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_NPUBBYTES);
    static_assert(SECURE_PACKET_TAG_BYTES ==
                  crypto_aead_chacha20poly1305_IETF_ABYTES);
    static_assert(SessionKey{}.size() ==
                  crypto_aead_chacha20poly1305_IETF_KEYBYTES);

    plaintext_len = 0;
    if (metadata.room_id.empty() || metadata.room_instance_id.empty() ||
        metadata.sender_id == 0 || metadata.access_epoch == 0 ||
        ciphertext == nullptr ||
        ciphertext_len <= SECURE_PACKET_TAG_BYTES ||
        ciphertext_len > ROOM_CHAT_CIPHERTEXT_MAX_BYTES ||
        plaintext_out == nullptr ||
        plaintext_capacity < ciphertext_len - SECURE_PACKET_TAG_BYTES ||
        !detail::ensure_sodium_initialized()) {
        return false;
    }

    const auto associated_data = chat_associated_data(metadata);
    unsigned long long actual_plaintext_bytes = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            plaintext_out, &actual_plaintext_bytes, nullptr,
            ciphertext, static_cast<unsigned long long>(ciphertext_len),
            associated_data.data(),
            static_cast<unsigned long long>(associated_data.size()),
            reinterpret_cast<const unsigned char*>(metadata.nonce.data()),
            key.data()) != 0 ||
        actual_plaintext_bytes > ROOM_CHAT_PLAINTEXT_MAX_BYTES) {
        return false;
    }
    plaintext_len = static_cast<size_t>(actual_plaintext_bytes);
    return true;
}

}  // namespace session_crypto
