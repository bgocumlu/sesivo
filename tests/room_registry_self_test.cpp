#include "room_registry.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_create_join_and_password_change() {
    room_registry::RoomRegistry registry;
    const auto now = std::chrono::steady_clock::now();

    const auto created =
        registry.create_room("room-a", "Room A", "hash-a", now);
    require(created.ok, "room should be created");
    require(created.created, "create result should mark new room");
    require(!created.admin_token.empty(), "admin token should be returned");
    require(created.room.locked, "room with password hash should be locked");
    require(created.room.access_epoch == 1, "initial access epoch should be 1");

    const auto wrong = registry.authorize_join("room-a", "wrong", now);
    require(!wrong.ok, "wrong password hash should reject");

    const auto joined = registry.authorize_join("room-a", "hash-a", now);
    require(joined.ok, "matching password hash should authorize");

    const auto changed =
        registry.change_password("room-a", created.admin_token, "hash-b", now);
    require(changed.ok, "admin should change password");
    require(changed.room.access_epoch == 2, "password change should bump epoch");

    const auto old_password = registry.authorize_join("room-a", "hash-a", now);
    require(!old_password.ok, "old password hash should reject after change");

    const auto new_password = registry.authorize_join("room-a", "hash-b", now);
    require(new_password.ok, "new password hash should authorize");

    const auto old_claims =
        registry.validate_claims("room-a", joined.room.room_instance_id,
                                 joined.room.access_epoch, now);
    require(!old_claims.ok, "old access epoch should reject");

    const auto current_claims =
        registry.validate_claims("room-a", changed.room.room_instance_id,
                                 changed.room.access_epoch, now);
    require(current_claims.ok, "current access epoch should validate");

    const auto rotated =
        registry.rotate_access_epoch("room-a", created.admin_token, now);
    require(rotated.ok, "admin should rotate access epoch");
    require(rotated.room.access_epoch == 3, "manual epoch rotation should bump epoch");

    const auto stale_after_rotation =
        registry.validate_claims("room-a", changed.room.room_instance_id,
                                 changed.room.access_epoch, now);
    require(!stale_after_rotation.ok, "rotated access epoch should reject old claims");
}

void test_empty_rooms_disappear() {
    room_registry::RoomRegistry registry;
    const auto now = std::chrono::steady_clock::now();

    registry.ensure_open_room("jam-room", now);
    std::unordered_map<std::string, size_t> counts{{"jam-room", 1}};
    require(registry.list_rooms(counts).size() == 1, "room should be listed while occupied");

    counts.clear();
    registry.remove_empty_rooms(counts, now);
    require(registry.list_rooms(counts).empty(), "joined empty room should disappear");
}

}  // namespace

int main() {
    test_create_join_and_password_change();
    test_empty_rooms_disappear();
    std::cout << "room registry self-test passed\n";
    return 0;
}
