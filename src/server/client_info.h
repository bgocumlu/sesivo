#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "protocol.h"

// Per-client state for SFU server
struct ClientInfo {
    std::chrono::steady_clock::time_point last_alive;
    std::chrono::steady_clock::time_point joined_at;
    uint32_t                              client_id = 0;  // Unique ID for this client
    std::string                           room_id;
    std::string                           profile_id;
    std::string                           display_name;
    uint32_t                              capabilities = 0;
    Bytes<E2E_PUBLIC_KEY_BYTES>           key_public{};
    bool                                  joined_with_metadata = false;
    bool                                  has_authenticated_session = false;
    std::string                           token_nonce_key;
};
