#pragma once

#include "join_reliability.h"
#include "protocol.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

struct PerformerJoinOptions {
    std::string room_id;
    std::string room_handle;
    std::string user_id;
    std::string display_name;
    std::string join_token;
    std::string media_secret;
    uint8_t access_mode = ROOM_ACCESS_OPEN;
    Bytes<E2E_PUBLIC_KEY_BYTES> key_public{};
};

class ClientJoinSession {
public:
    explicit ClientJoinSession(
        PerformerJoinOptions options = {},
        std::chrono::steady_clock::duration retry_interval = std::chrono::seconds(1));

    const std::string& room_id() const;
    const std::string& user_id() const;
    const std::string& join_token() const;
    const std::string& media_secret() const;
    bool has_join_token() const;
    bool has_media_secret() const;

    void configure(PerformerJoinOptions options);
    void set_media_secret(std::string media_secret);
    void set_key_public(Bytes<E2E_PUBLIC_KEY_BYTES> key_public);
    void set_access_mode(uint8_t access_mode);
    uint8_t access_mode() const;
    JoinHdr make_join_header() const;

    void reset();
    bool should_send_join(std::chrono::steady_clock::time_point now) const;
    void mark_join_sent(std::chrono::steady_clock::time_point now);
    void mark_join_ack(uint32_t participant_id, uint32_t server_capabilities);
    void mark_join_required();

    bool is_join_confirmed() const;
    bool can_send_audio() const;
    uint32_t participant_id() const;
    bool server_supports(uint32_t capability) const;

private:
    PerformerJoinOptions options_;
    join_reliability::State state_;
};
