#include "client_join_session.h"

#include "packet_builder.h"

#include <utility>

ClientJoinSession::ClientJoinSession(
    PerformerJoinOptions options,
    std::chrono::steady_clock::duration retry_interval)
    : options_(std::move(options)),
      state_(retry_interval) {}

const std::string& ClientJoinSession::room_id() const {
    return options_.room_id;
}

const std::string& ClientJoinSession::user_id() const {
    return options_.user_id;
}

const std::string& ClientJoinSession::join_token() const {
    return options_.join_token;
}

bool ClientJoinSession::has_join_token() const {
    return !options_.join_token.empty();
}

void ClientJoinSession::configure(PerformerJoinOptions options) {
    options_ = std::move(options);
    state_.reset();
}

JoinHdr ClientJoinSession::make_join_header() const {
    JoinHdr join{};
    join.magic = CTRL_MAGIC;
    join.type  = CtrlHdr::Cmd::JOIN;
    packet_builder::write_fixed(join.room_id, options_.room_id);
    packet_builder::write_fixed(join.room_handle, options_.room_handle);
    packet_builder::write_fixed(join.profile_id, options_.user_id);
    packet_builder::write_fixed(join.display_name, options_.display_name);
    packet_builder::write_fixed(join.join_token, options_.join_token);
    join.capabilities = AUDIO_SUPPORTED_CAPABILITIES;
    return join;
}

void ClientJoinSession::reset() {
    state_.reset();
}

bool ClientJoinSession::should_send_join(std::chrono::steady_clock::time_point now) const {
    return state_.should_send_join(now);
}

void ClientJoinSession::mark_join_sent(std::chrono::steady_clock::time_point now) {
    state_.mark_join_sent(now);
}

void ClientJoinSession::mark_join_ack(uint32_t participant_id, uint32_t server_capabilities) {
    state_.mark_join_ack(participant_id, server_capabilities);
}

void ClientJoinSession::mark_join_required() {
    state_.mark_join_required();
}

bool ClientJoinSession::is_join_confirmed() const {
    return state_.is_join_confirmed();
}

bool ClientJoinSession::can_send_audio() const {
    return state_.can_send_audio();
}

bool ClientJoinSession::server_supports(uint32_t capability) const {
    return state_.server_supports(capability);
}
