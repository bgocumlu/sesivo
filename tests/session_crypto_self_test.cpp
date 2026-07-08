#include "session_crypto.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "audio_packet.h"
#include "packet_builder.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_libsodium_sha256_and_hmac_vectors() {
    const std::vector<unsigned char> abc{'a', 'b', 'c'};
    const auto sha = performer_join_token::try_sha256(abc);
    require(sha.has_value(), "sha256 should succeed");
    require(performer_join_token::hex(*sha) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256 vector should match");
    const auto hmac = performer_join_token::try_hmac_sha256_hex(
        "key", "The quick brown fox jumps over the lazy dog");
    require(hmac.has_value(), "hmac-sha256 should succeed");
    require(*hmac ==
                "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
            "hmac-sha256 vector should match");
}

bool same_metadata(const session_crypto::SecureAudioMetadata& lhs,
                   const session_crypto::SecureAudioMetadata& rhs) {
    return lhs.sender_id == rhs.sender_id &&
           lhs.sequence == rhs.sequence &&
           lhs.sample_rate == rhs.sample_rate &&
           lhs.frame_count == rhs.frame_count &&
           lhs.plaintext_bytes == rhs.plaintext_bytes &&
           lhs.channels == rhs.channels &&
           lhs.codec == rhs.codec &&
           lhs.capture_server_time_ns == rhs.capture_server_time_ns;
}

void test_key_derivation_is_stable() {
    const auto key = session_crypto::derive_media_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    const auto same_key = session_crypto::derive_media_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    const auto other_room_key = session_crypto::derive_media_key_from_secret(
        "other.room", "room.instance.a", "test-media-secret");
    const auto other_instance_key = session_crypto::derive_media_key_from_secret(
        "secure.room", "room.instance.b", "test-media-secret");
    const auto other_secret_key = session_crypto::derive_media_key_from_secret(
        "secure.room", "room.instance.a", "other-media-secret");

    require(key.has_value(), "media key derivation should succeed");
    require(same_key.has_value(), "same media key derivation should succeed");
    require(other_room_key.has_value(), "other room media key derivation should succeed");
    require(other_instance_key.has_value(),
            "other instance media key derivation should succeed");
    require(other_secret_key.has_value(),
            "other secret media key derivation should succeed");
    require(*key == *same_key, "same media secret and room should produce same key");
    require(*key != *other_room_key, "different room should produce different media key");
    require(*key != *other_instance_key,
            "different room instance should produce different media key");
    require(*key != *other_secret_key, "different secret should produce different media key");

    const auto chat_key = session_crypto::derive_chat_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    const auto same_chat_key = session_crypto::derive_chat_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    require(chat_key.has_value(), "chat key derivation should succeed");
    require(same_chat_key.has_value(), "same chat key derivation should succeed");
    require(*chat_key == *same_chat_key, "same room chat key should be stable");
    require(*chat_key != *key, "chat key should be domain-separated from media key");
}

void test_seal_open_round_trip_and_tamper_rejection() {
    const auto key = session_crypto::derive_media_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    require(key.has_value(), "media key derivation should succeed");

    const std::array<unsigned char, 4> payload{0x11, 0x22, 0x33, 0x44};
    const auto audio = audio_packet::create_audio_packet_v3(
        42, 48000, 120, 1, payload.data(),
        static_cast<uint16_t>(payload.size()), 123456789LL);
    require(audio != nullptr, "audio packet should build");
    packet_builder::embed_sender_id(audio->data(), 7);

    session_crypto::SecureAudioMetadata metadata;
    metadata.sender_id = 7;
    metadata.sequence = 42;
    metadata.sample_rate = 48000;
    metadata.frame_count = 120;
    metadata.plaintext_bytes = static_cast<uint16_t>(audio->size());
    metadata.channels = 1;
    metadata.codec = AudioCodec::Opus;
    metadata.capture_server_time_ns = 123456789LL;

    std::array<unsigned char, 2048> sealed{};
    size_t sealed_bytes = 0;
    require(session_crypto::seal_audio_packet(*key, metadata, audio->data(), audio->size(),
                                              sealed.data(), sealed.size(), sealed_bytes),
            "seal should succeed");
    require(sealed_bytes == SECURE_PACKET_HEADER_BYTES + audio->size() +
                                SECURE_PACKET_TAG_BYTES,
            "AEAD packet size should include header and authentication tag");

    uint32_t magic = 0;
    std::memcpy(&magic, sealed.data(), sizeof(magic));
    require(magic == SECURE_AUDIO_MAGIC, "secure packet should use secure magic");

    session_crypto::SecureAudioMetadata parsed_metadata;
    uint16_t encrypted_bytes = 0;
    require(session_crypto::parse_secure_audio_header(
                sealed.data(), sealed_bytes, parsed_metadata, encrypted_bytes),
            "secure header should parse");
    require(same_metadata(parsed_metadata, metadata), "secure metadata should round trip");
    require(encrypted_bytes == audio->size() + SECURE_PACKET_TAG_BYTES,
            "encrypted byte count should include authentication tag");

    std::array<unsigned char, 2048> opened{};
    size_t opened_bytes = 0;
    session_crypto::SecureAudioMetadata opened_metadata;
    require(session_crypto::open_audio_packet(*key, sealed.data(), sealed_bytes,
                                              opened_metadata, opened.data(),
                                              opened.size(), opened_bytes),
            "open should succeed");
    require(same_metadata(opened_metadata, metadata), "opened metadata should match");
    require(opened_bytes == audio->size(), "opened size should match plaintext");
    require(std::equal(audio->begin(), audio->end(), opened.begin()),
            "opened plaintext should match");

    std::array<unsigned char, 2048> wrong_key_opened{};
    auto wrong_key = *key;
    wrong_key[0] ^= 0x80;
    require(!session_crypto::open_audio_packet(wrong_key, sealed.data(), sealed_bytes,
                                               opened_metadata, wrong_key_opened.data(),
                                               wrong_key_opened.size(), opened_bytes),
            "wrong key should fail authentication");

    auto tampered_ciphertext = sealed;
    tampered_ciphertext[SECURE_PACKET_HEADER_BYTES] ^= 0x01;
    require(!session_crypto::open_audio_packet(*key, tampered_ciphertext.data(), sealed_bytes,
                                               opened_metadata, opened.data(), opened.size(),
                                               opened_bytes),
            "ciphertext tamper should fail");

    auto tampered_header = sealed;
    SecureAudioHdr header{};
    std::memcpy(&header, tampered_header.data(), sizeof(header));
    header.reserved = 1;
    std::memcpy(tampered_header.data(), &header, sizeof(header));
    require(!session_crypto::open_audio_packet(*key, tampered_header.data(), sealed_bytes,
                                               opened_metadata, opened.data(), opened.size(),
                                               opened_bytes),
            "reserved header tamper should fail");

    auto tampered_nonce = sealed;
    std::memcpy(&header, tampered_nonce.data(), sizeof(header));
    header.nonce[0] ^= 0x01;
    std::memcpy(tampered_nonce.data(), &header, sizeof(header));
    require(!session_crypto::open_audio_packet(*key, tampered_nonce.data(), sealed_bytes,
                                               opened_metadata, opened.data(), opened.size(),
                                               opened_bytes),
            "nonce tamper should fail");

    auto tampered_tag = sealed;
    tampered_tag[sealed_bytes - 1] ^= 0x01;
    require(!session_crypto::open_audio_packet(*key, tampered_tag.data(), sealed_bytes,
                                               opened_metadata, opened.data(), opened.size(),
                                               opened_bytes),
            "tag tamper should fail");

    require(!session_crypto::open_audio_packet(*key, sealed.data(), sealed_bytes - 1,
                                               opened_metadata, opened.data(), opened.size(),
                                               opened_bytes),
            "truncated packet should fail");
}

void test_secure_control_round_trip_and_tamper_rejection() {
    const auto key = session_crypto::derive_media_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    require(key.has_value(), "media key derivation should succeed");

    MediaKeyRotationPayload payload{};
    const std::string next_secret = "next-media-secret";
    payload.access_epoch = 3;
    payload.media_secret_bytes = static_cast<uint16_t>(next_secret.size());
    std::copy_n(next_secret.begin(), next_secret.size(), payload.media_secret.begin());

    session_crypto::SecureControlMetadata metadata;
    metadata.sender_id = 7;
    metadata.sequence = 3;
    metadata.plaintext_bytes = sizeof(payload);

    std::array<unsigned char, 512> sealed{};
    size_t sealed_bytes = 0;
    require(session_crypto::seal_control_packet(
                *key, metadata, reinterpret_cast<const unsigned char*>(&payload),
                sizeof(payload), sealed.data(), sealed.size(), sealed_bytes),
            "control seal should succeed");
    require(sealed_bytes == SECURE_CONTROL_HEADER_BYTES + sizeof(payload) +
                                SECURE_PACKET_TAG_BYTES,
            "control AEAD packet size should include header and authentication tag");

    session_crypto::SecureControlMetadata parsed_metadata;
    uint16_t encrypted_bytes = 0;
    require(session_crypto::parse_secure_control_header(
                sealed.data(), sealed_bytes, parsed_metadata, encrypted_bytes),
            "secure control header should parse");
    require(parsed_metadata.sender_id == metadata.sender_id &&
                parsed_metadata.sequence == metadata.sequence &&
                parsed_metadata.plaintext_bytes == metadata.plaintext_bytes,
            "secure control metadata should round trip");

    std::array<unsigned char, 512> opened{};
    size_t opened_bytes = 0;
    session_crypto::SecureControlMetadata opened_metadata;
    require(session_crypto::open_control_packet(
                *key, sealed.data(), sealed_bytes, opened_metadata, opened.data(),
                opened.size(), opened_bytes),
            "control open should succeed");
    require(opened_bytes == sizeof(payload), "opened control size should match");
    require(opened_metadata.sender_id == metadata.sender_id &&
                opened_metadata.sequence == metadata.sequence,
            "opened control metadata should match");
    MediaKeyRotationPayload opened_payload{};
    std::memcpy(&opened_payload, opened.data(), sizeof(opened_payload));
    require(opened_payload.command == SECURE_CONTROL_ROTATE_MEDIA_KEY,
            "opened control command should match");
    require(opened_payload.access_epoch == payload.access_epoch,
            "opened access epoch should match");
    require(opened_payload.media_secret_bytes == next_secret.size(),
            "opened media secret size should match");
    require(std::equal(next_secret.begin(), next_secret.end(),
                       opened_payload.media_secret.begin()),
            "opened media secret should match");

    auto tampered_header = sealed;
    SecureControlHdr header{};
    std::memcpy(&header, tampered_header.data(), sizeof(header));
    header.sequence ^= 0x01;
    std::memcpy(tampered_header.data(), &header, sizeof(header));
    require(!session_crypto::open_control_packet(
                *key, tampered_header.data(), sealed_bytes, opened_metadata,
                opened.data(), opened.size(), opened_bytes),
            "control header tamper should fail");

    auto tampered_ciphertext = sealed;
    tampered_ciphertext[SECURE_CONTROL_HEADER_BYTES] ^= 0x01;
    require(!session_crypto::open_control_packet(
                *key, tampered_ciphertext.data(), sealed_bytes, opened_metadata,
                opened.data(), opened.size(), opened_bytes),
            "control ciphertext tamper should fail");
}

void test_chat_seal_open_and_aad_rejection() {
    const auto key = session_crypto::derive_chat_key_from_secret(
        "secure.room", "room.instance.a", "test-media-secret");
    require(key.has_value(), "chat key derivation should succeed");

    session_crypto::ChatMetadata metadata;
    metadata.room_id = "secure.room";
    metadata.room_instance_id = "room.instance.a";
    metadata.sender_id = 7;
    metadata.access_epoch = 3;
    require(session_crypto::make_chat_nonce(metadata.nonce),
            "chat nonce generation should succeed");

    const std::string plaintext = "hello encrypted room";
    std::array<unsigned char, ROOM_CHAT_CIPHERTEXT_MAX_BYTES> ciphertext{};
    size_t ciphertext_bytes = 0;
    require(session_crypto::seal_chat_message(
                *key, metadata,
                reinterpret_cast<const unsigned char*>(plaintext.data()),
                plaintext.size(), ciphertext.data(), ciphertext.size(),
                ciphertext_bytes),
            "chat seal should succeed");
    require(ciphertext_bytes == plaintext.size() + SECURE_PACKET_TAG_BYTES,
            "chat ciphertext should include authentication tag");

    std::array<unsigned char, ROOM_CHAT_PLAINTEXT_MAX_BYTES> opened{};
    size_t opened_bytes = 0;
    require(session_crypto::open_chat_message(
                *key, metadata, ciphertext.data(), ciphertext_bytes,
                opened.data(), opened.size(), opened_bytes),
            "chat open should succeed");
    require(opened_bytes == plaintext.size(), "chat plaintext size should match");
    require(std::equal(plaintext.begin(), plaintext.end(), opened.begin()),
            "chat plaintext should round trip");

    auto wrong_epoch = metadata;
    wrong_epoch.access_epoch += 1;
    require(!session_crypto::open_chat_message(
                *key, wrong_epoch, ciphertext.data(), ciphertext_bytes,
                opened.data(), opened.size(), opened_bytes),
            "chat AAD epoch tamper should fail");

    auto wrong_sender = metadata;
    wrong_sender.sender_id += 1;
    require(!session_crypto::open_chat_message(
                *key, wrong_sender, ciphertext.data(), ciphertext_bytes,
                opened.data(), opened.size(), opened_bytes),
            "chat AAD sender tamper should fail");

    auto tampered_ciphertext = ciphertext;
    tampered_ciphertext[0] ^= 0x01;
    require(!session_crypto::open_chat_message(
                *key, metadata, tampered_ciphertext.data(), ciphertext_bytes,
                opened.data(), opened.size(), opened_bytes),
            "chat ciphertext tamper should fail");
}

}  // namespace

int main() {
    test_libsodium_sha256_and_hmac_vectors();
    test_key_derivation_is_stable();
    test_seal_open_round_trip_and_tamper_rejection();
    test_secure_control_round_trip_and_tamper_rejection();
    test_chat_seal_open_and_aad_rejection();
    std::cout << "session crypto self-test passed\n";
    return 0;
}
