#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct SecureInvite {
    std::string server_address;
    uint16_t server_port = 0;
    std::string room_id;
};

std::string make_media_secret();
std::string make_secure_invite_link(const std::string& server_address,
                                    uint16_t server_port,
                                    const std::string& room_id);
bool contains_secure_invite_link(const std::string& text);
std::optional<SecureInvite> parse_secure_invite_text(const std::string& text,
                                                     std::string& reason);
bool invite_matches_room(const SecureInvite& invite,
                         const std::string& server_address,
                         uint16_t server_port,
                         const std::string& room_id);
