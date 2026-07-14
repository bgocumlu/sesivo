#include "room_registry.h"
#include "packet_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string commitment(char digit) {
    return std::string(MEDIA_KEY_COMMITMENT_HEX_BYTES, digit);
}

template <size_t N>
std::string fixed_string(const Bytes<N>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), '\0');
    return std::string(bytes.begin(), end);
}

void test_media_key_commitment_control_serialization() {
    const auto expected = commitment('a');

    RoomCreateRequestHdr create{};
    packet_builder::write_fixed(create.media_key_commitment, expected);
    require(fixed_string(create.media_key_commitment) == expected,
            "room create must preserve the complete 64-character key commitment");

    RoomAdminRequestHdr admin{};
    packet_builder::write_fixed(admin.media_key_commitment, expected);
    require(fixed_string(admin.media_key_commitment) == expected,
            "room admin must preserve the complete 64-character key commitment");
}

void test_create_join_and_password_change() {
    room_registry::RoomRegistry registry;
    const auto now = std::chrono::steady_clock::now();

    const auto created =
        registry.create_room("room-a", "Room A", "hash-a",
                             ROOM_ACCESS_PASSWORD, commitment('a'), now);
    require(created.ok, "room should be created");
    require(created.created, "create result should mark new room");
    require(!created.admin_token.empty(), "admin token should be returned");
    require(created.room.locked, "room with password hash should be locked");
    require(created.room.access_mode == ROOM_ACCESS_PASSWORD,
            "room should report password access");
    require(created.room.access_epoch == 1, "initial access epoch should be 1");

    const auto wrong = registry.authorize_join("room-a", "wrong", now);
    require(!wrong.ok, "wrong password hash should reject");

    const auto joined = registry.authorize_join("room-a", "hash-a", now);
    require(joined.ok, "matching password hash should authorize");

    const auto changed =
        registry.change_password("room-a", created.admin_token, "hash-b",
                                 commitment('b'), now);
    require(changed.ok, "admin should change password");
    require(changed.room.access_epoch == 2, "password change should bump epoch");

    const auto old_password = registry.authorize_join("room-a", "hash-a", now);
    require(!old_password.ok, "old password hash should reject after change");

    const auto new_password = registry.authorize_join("room-a", "hash-b", now);
    require(new_password.ok, "new password hash should authorize");

    const auto old_claims =
        registry.validate_claims("room-a", joined.room.room_instance_id,
                                 joined.room.access_epoch, commitment('a'), now);
    require(!old_claims.ok, "old access epoch should reject");

    const auto current_claims =
        registry.validate_claims("room-a", changed.room.room_instance_id,
                                 changed.room.access_epoch, commitment('b'), now);
    require(current_claims.ok, "current access epoch should validate");

    const auto rotated =
        registry.rotate_access_epoch("room-a", created.admin_token,
                                     commitment('c'), now);
    require(rotated.ok, "admin should rotate access epoch");
    require(rotated.room.access_epoch == 3, "manual epoch rotation should bump epoch");

    const auto stale_after_rotation =
        registry.validate_claims("room-a", changed.room.room_instance_id,
                                 changed.room.access_epoch, commitment('b'), now);
    require(!stale_after_rotation.ok, "rotated access epoch should reject old claims");

    const auto wrong_commitment =
        registry.validate_claims("room-a", rotated.room.room_instance_id,
                                 rotated.room.access_epoch, commitment('d'), now);
    require(!wrong_commitment.ok,
            "current epoch with an unauthorized key commitment should reject");
}

void test_open_and_approve_ignore_password() {
    room_registry::RoomRegistry registry;
    const auto now = std::chrono::steady_clock::now();

    const auto open =
        registry.create_room("open-room", "Open Room", "ignored",
                             ROOM_ACCESS_OPEN, commitment('1'), now);
    require(open.ok, "open room should be created");
    require(!open.room.locked, "open room should not be locked");
    require(open.room.access_mode == ROOM_ACCESS_OPEN,
            "open room should report open access");
    require(registry.authorize_join("open-room", "", now).ok,
            "open room should allow empty password");

    const auto approve =
        registry.create_room("approve-room", "Approve Room", "ignored",
                             ROOM_ACCESS_APPROVE, commitment('2'), now);
    require(approve.ok, "approve room should be created");
    require(!approve.room.locked, "approve room should not be password locked");
    require(approve.room.access_mode == ROOM_ACCESS_APPROVE,
            "approve room should report approve access");
    require(registry.authorize_join("approve-room", "", now).ok,
            "approval happens through hidden media key handoff");
}

void test_empty_rooms_disappear() {
    room_registry::RoomRegistry registry;
    const auto now = std::chrono::steady_clock::now();

    registry.ensure_open_room("jam-room", commitment('3'), now);
    std::unordered_map<std::string, size_t> counts{{"jam-room", 1}};
    require(registry.list_rooms(counts).size() == 1, "room should be listed while occupied");

    counts.clear();
    registry.remove_empty_rooms(counts, now);
    require(registry.list_rooms(counts).empty(), "joined empty room should disappear");
}

Bytes<SECURE_PACKET_NONCE_BYTES> nonce_for(int value) {
    Bytes<SECURE_PACKET_NONCE_BYTES> nonce{};
    nonce[0] = static_cast<char>(value);
    return nonce;
}

Bytes<ROOM_CHAT_CIPHERTEXT_MAX_BYTES> ciphertext_for(int value) {
    Bytes<ROOM_CHAT_CIPHERTEXT_MAX_BYTES> ciphertext{};
    const std::string text = "ciphertext-" + std::to_string(value);
    std::copy(text.begin(), text.end(), ciphertext.begin());
    return ciphertext;
}

void test_chat_ring_buffer_and_history() {
    room_registry::RoomRegistry registry;
    const auto now = std::chrono::steady_clock::now();
    const auto created =
        registry.create_room("chat-room", "Chat Room", "", ROOM_ACCESS_OPEN,
                             commitment('4'), now);
    require(created.ok, "chat room should be created");

    for (int i = 1; i <= 12; ++i) {
        const auto stored = registry.store_chat_message(
            created.room.room_id, created.room.room_instance_id,
            created.room.access_epoch, 7, nonce_for(i), ciphertext_for(i),
            static_cast<uint16_t>(12 + std::to_string(i).size()),
            1000 + i, now + std::chrono::seconds(i));
        require(stored.ok, "chat message should store");
        require(stored.message.sequence == static_cast<uint64_t>(i),
                "chat sequence should increase");
    }

    const auto duplicate = registry.store_chat_message(
        created.room.room_id, created.room.room_instance_id,
        created.room.access_epoch, 7, nonce_for(12), ciphertext_for(12),
        14, 2000, now + std::chrono::seconds(20));
    require(!duplicate.ok && duplicate.status == ROOM_STATUS_CONFLICT,
            "duplicate retained nonce should reject");

    const auto history = registry.chat_history_since(
        created.room.room_id, created.room.room_instance_id,
        created.room.access_epoch, 0, now + std::chrono::seconds(30));
    require(history.ok, "chat history should succeed");
    require(history.truncated, "history from before retained tail should be truncated");
    require(history.messages.size() == ROOM_CHAT_RETAINED_MESSAGES,
            "history should retain only configured tail");
    require(history.messages.front().sequence == 3,
            "oldest retained chat sequence should be 3 after 12 sends");
    require(history.messages.back().sequence == 12,
            "newest retained chat sequence should be 12");

    const auto recent = registry.chat_history_since(
        created.room.room_id, created.room.room_instance_id,
        created.room.access_epoch, 10, now + std::chrono::seconds(31));
    require(recent.ok, "recent chat history should succeed");
    require(!recent.truncated, "recent chat history should not be truncated");
    require(recent.messages.size() == 2, "recent history should return messages after cursor");

    const auto wrong_epoch = registry.chat_history_since(
        created.room.room_id, created.room.room_instance_id,
        created.room.access_epoch + 1, 0, now + std::chrono::seconds(32));
    require(!wrong_epoch.ok && wrong_epoch.status == ROOM_STATUS_FORBIDDEN,
            "wrong epoch history should reject");
}

}  // namespace

int main() {
    test_media_key_commitment_control_serialization();
    test_create_join_and_password_change();
    test_open_and_approve_ignore_password();
    test_empty_rooms_disappear();
    test_chat_ring_buffer_and_history();
    std::cout << "room registry self-test passed\n";
    return 0;
}
