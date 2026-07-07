#pragma once

#include "performer_join_token.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "protocol.h"

namespace room_registry {

using time_point = std::chrono::steady_clock::time_point;

constexpr auto EMPTY_PENDING_ROOM_TTL = std::chrono::minutes(2);
constexpr size_t MAX_ROOM_ID_BYTES = 63;
constexpr size_t MAX_ROOM_NAME_BYTES = 63;

struct RoomSnapshot {
    std::string room_id;
    std::string room_name;
    std::string room_instance_id;
    bool locked = false;
    uint8_t access_mode = ROOM_ACCESS_OPEN;
    uint32_t access_epoch = 1;
};

struct RoomListEntry : RoomSnapshot {
    size_t participant_count = 0;
};

struct CreateResult {
    bool ok = false;
    bool created = false;
    std::string reason;
    RoomSnapshot room;
    std::string admin_token;
};

struct AuthorizeResult {
    bool ok = false;
    std::string reason;
    RoomSnapshot room;
};

inline std::string sha256_hex(const std::string& value) {
    const std::vector<unsigned char> bytes(value.begin(), value.end());
    return performer_join_token::hex(performer_join_token::sha256(bytes));
}

inline std::string make_secret_token() {
    return performer_join_token::random_nonce() + performer_join_token::random_nonce();
}

inline bool valid_room_id(const std::string& room_id) {
    return !room_id.empty() && room_id.size() <= MAX_ROOM_ID_BYTES &&
           std::all_of(room_id.begin(), room_id.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '-' || c == '_' || c == '.';
           });
}

class RoomRegistry {
public:
    CreateResult create_room(std::string room_id, std::string room_name,
                             std::string password_hash, uint8_t access_mode,
                             time_point now) {
        CreateResult result;
        if (!valid_room_id(room_id)) {
            result.reason = "invalid room id";
            return result;
        }
        if (room_name.size() > MAX_ROOM_NAME_BYTES) {
            room_name.resize(MAX_ROOM_NAME_BYTES);
        }
        if (room_name.empty()) {
            room_name = room_id;
        }
        if (rooms_.contains(room_id)) {
            result.reason = "room already exists";
            result.room = snapshot_for(rooms_.at(room_id));
            return result;
        }
        if (!valid_access_mode(access_mode)) {
            result.reason = "invalid access mode";
            return result;
        }
        if (access_mode != ROOM_ACCESS_PASSWORD) {
            password_hash.clear();
        } else if (password_hash.empty()) {
            result.reason = "missing room password";
            return result;
        }

        const std::string admin_token = make_secret_token();
        Room room;
        room.room_id = std::move(room_id);
        room.room_name = std::move(room_name);
        room.room_instance_id = performer_join_token::random_nonce();
        room.password_hash = std::move(password_hash);
        room.access_mode = access_mode;
        room.admin_token_hash = sha256_hex(admin_token);
        room.created_at = now;
        room.last_activity = now;
        result.room = snapshot_for(room);
        result.admin_token = admin_token;
        result.ok = true;
        result.created = true;
        rooms_.emplace(result.room.room_id, std::move(room));
        return result;
    }

    RoomSnapshot ensure_open_room(const std::string& room_id, time_point now) {
        auto it = rooms_.find(room_id);
        if (it != rooms_.end()) {
            it->second.ever_joined = true;
            it->second.last_activity = now;
            return snapshot_for(it->second);
        }

        Room room;
        room.room_id = room_id;
        room.room_name = room_id;
        room.room_instance_id = performer_join_token::random_nonce();
        room.access_mode = ROOM_ACCESS_OPEN;
        room.created_at = now;
        room.last_activity = now;
        room.ever_joined = true;
        auto [inserted, _] = rooms_.emplace(room.room_id, std::move(room));
        return snapshot_for(inserted->second);
    }

    AuthorizeResult authorize_join(const std::string& room_id,
                                   const std::string& password_hash,
                                   time_point now) {
        AuthorizeResult result;
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            result.reason = "room not found";
            return result;
        }
        if (it->second.access_mode == ROOM_ACCESS_PASSWORD &&
            it->second.password_hash != password_hash) {
            result.reason = "wrong room password";
            return result;
        }
        it->second.last_activity = now;
        result.ok = true;
        result.room = snapshot_for(it->second);
        return result;
    }

    AuthorizeResult validate_claims(const std::string& room_id,
                                    const std::string& room_instance_id,
                                    uint32_t access_epoch,
                                    time_point now) {
        AuthorizeResult result;
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            result.reason = "room not found";
            return result;
        }
        if (!room_instance_id.empty() &&
            it->second.room_instance_id != room_instance_id) {
            result.reason = "wrong room instance";
            return result;
        }
        if (access_epoch != 0 && it->second.access_epoch != access_epoch) {
            result.reason = "wrong room access epoch";
            return result;
        }
        it->second.last_activity = now;
        result.ok = true;
        result.room = snapshot_for(it->second);
        return result;
    }

    AuthorizeResult change_password(const std::string& room_id,
                                    const std::string& admin_token,
                                    std::string password_hash,
                                    time_point now) {
        return change_access_mode(room_id, admin_token,
                                  password_hash.empty() ? ROOM_ACCESS_OPEN
                                                        : ROOM_ACCESS_PASSWORD,
                                  std::move(password_hash), now);
    }

    AuthorizeResult change_access_mode(const std::string& room_id,
                                       const std::string& admin_token,
                                       uint8_t access_mode,
                                       std::string password_hash,
                                       time_point now) {
        auto result = authorize_admin(room_id, admin_token, now);
        if (!result.ok) {
            return result;
        }
        if (!valid_access_mode(access_mode)) {
            result.ok = false;
            result.reason = "invalid access mode";
            return result;
        }
        if (access_mode == ROOM_ACCESS_PASSWORD && password_hash.empty()) {
            result.ok = false;
            result.reason = "missing room password";
            return result;
        }
        auto& room = rooms_.at(room_id);
        if (access_mode != ROOM_ACCESS_PASSWORD) {
            password_hash.clear();
        }
        room.password_hash = std::move(password_hash);
        room.access_mode = access_mode;
        ++room.access_epoch;
        room.last_activity = now;
        result.room = snapshot_for(room);
        return result;
    }

    AuthorizeResult rotate_access_epoch(const std::string& room_id,
                                        const std::string& admin_token,
                                        time_point now) {
        auto result = authorize_admin(room_id, admin_token, now);
        if (!result.ok) {
            return result;
        }
        auto& room = rooms_.at(room_id);
        ++room.access_epoch;
        room.last_activity = now;
        result.room = snapshot_for(room);
        return result;
    }

    AuthorizeResult close_room(const std::string& room_id,
                               const std::string& admin_token,
                               time_point now) {
        auto result = authorize_admin(room_id, admin_token, now);
        if (!result.ok) {
            return result;
        }
        result.room = snapshot_for(rooms_.at(room_id));
        rooms_.erase(room_id);
        return result;
    }

    AuthorizeResult authorize_admin(const std::string& room_id,
                                    const std::string& admin_token,
                                    time_point now) {
        AuthorizeResult result;
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            result.reason = "room not found";
            return result;
        }
        if (admin_token.empty() ||
            sha256_hex(admin_token) != it->second.admin_token_hash) {
            result.reason = "invalid room admin token";
            return result;
        }
        it->second.last_activity = now;
        result.ok = true;
        result.room = snapshot_for(it->second);
        return result;
    }

    void mark_joined(const std::string& room_id, time_point now) {
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            return;
        }
        it->second.ever_joined = true;
        it->second.last_activity = now;
    }

    std::vector<RoomListEntry> list_rooms(
        const std::unordered_map<std::string, size_t>& participant_counts) const {
        std::vector<RoomListEntry> rooms;
        rooms.reserve(rooms_.size());
        for (const auto& [room_id, room]: rooms_) {
            RoomListEntry entry;
            static_cast<RoomSnapshot&>(entry) = snapshot_for(room);
            const auto count = participant_counts.find(room_id);
            entry.participant_count =
                count == participant_counts.end() ? 0 : count->second;
            rooms.push_back(std::move(entry));
        }
        std::sort(rooms.begin(), rooms.end(), [](const auto& left, const auto& right) {
            if (left.participant_count != right.participant_count) {
                return left.participant_count > right.participant_count;
            }
            return left.room_name < right.room_name;
        });
        return rooms;
    }

    void remove_empty_rooms(const std::unordered_map<std::string, size_t>& counts,
                            time_point now) {
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            const auto count = counts.find(it->first);
            const bool empty = count == counts.end() || count->second == 0;
            const bool pending_expired =
                !it->second.ever_joined &&
                now - it->second.created_at > EMPTY_PENDING_ROOM_TTL;
            if (empty && (it->second.ever_joined || pending_expired)) {
                it = rooms_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Room {
        std::string room_id;
        std::string room_name;
        std::string room_instance_id;
        std::string password_hash;
        std::string admin_token_hash;
        uint8_t access_mode = ROOM_ACCESS_OPEN;
        uint32_t access_epoch = 1;
        bool ever_joined = false;
        time_point created_at{};
        time_point last_activity{};
    };

    static RoomSnapshot snapshot_for(const Room& room) {
        return RoomSnapshot{
            room.room_id,
            room.room_name,
            room.room_instance_id,
            room.access_mode == ROOM_ACCESS_PASSWORD,
            room.access_mode,
            room.access_epoch,
        };
    }

    static bool valid_access_mode(uint8_t access_mode) {
        return access_mode == ROOM_ACCESS_OPEN ||
               access_mode == ROOM_ACCESS_PASSWORD ||
               access_mode == ROOM_ACCESS_APPROVE;
    }

    std::unordered_map<std::string, Room> rooms_;
};

}  // namespace room_registry
