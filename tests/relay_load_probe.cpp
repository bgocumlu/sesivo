#include "client_join_session.h"
#include "packet_builder.h"
#include "performer_join_token.h"
#include "protocol.h"
#include "session_crypto.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

using asio::ip::udp;
using namespace std::chrono_literals;

namespace {

struct Client {
    explicit Client(asio::io_context& io)
        : socket(io, udp::v4()), native_handle(socket.native_handle()) {
        socket.bind({udp::v4(), 0});
        socket.set_option(asio::socket_base::receive_buffer_size(4 * 1024 * 1024));
        socket.non_blocking(true);
    }

    udp::socket socket;
    udp::socket::native_handle_type native_handle;
    uint32_t id = 0;
    uint32_t sequence = 0;
};

void send_audio(Client& client, const std::vector<unsigned char>& packet,
                const udp::endpoint& server) {
#ifdef _WIN32
    const int sent = ::sendto(client.native_handle,
                              reinterpret_cast<const char*>(packet.data()),
                              static_cast<int>(packet.size()), 0, server.data(),
                              static_cast<int>(server.size()));
    if (sent != static_cast<int>(packet.size())) {
        throw std::runtime_error("native audio send failed");
    }
#else
    const auto sent = ::sendto(client.native_handle, packet.data(), packet.size(), 0,
                               server.data(), server.size());
    if (sent != static_cast<decltype(sent)>(packet.size())) {
        throw std::runtime_error("native audio send failed");
    }
#endif
}

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string option(int argc, char** argv, const std::string& name,
                   const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

int option_int(int argc, char** argv, const std::string& name, int fallback) {
    return std::stoi(option(argc, argv, name, std::to_string(fallback)));
}

template <size_t N>
std::string fixed_string(const Bytes<N>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), '\0');
    return std::string(bytes.begin(), end);
}

template <typename Packet>
void send_control_packet(Client& client, const Packet& packet,
                         const udp::endpoint& server) {
    client.socket.send_to(asio::buffer(&packet, sizeof(packet)), server);
}

template <typename Response>
Response receive_room_response(Client& client, CtrlHdr::Cmd expected_type,
                               uint32_t request_id) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::array<unsigned char, 2048> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = client.socket.receive_from(
            asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            throw std::runtime_error("room control receive failed: " + error.message());
        }
        if (bytes != sizeof(Response)) {
            continue;
        }

        Response response{};
        std::memcpy(&response, buffer.data(), sizeof(response));
        if (response.magic == CTRL_MAGIC && response.type == expected_type &&
            response.request_id == request_id) {
            return response;
        }
    }
    throw std::runtime_error("room control response timeout");
}

uint32_t join_with_ticket(Client& client, const udp::endpoint& server,
                          const std::string& room_id,
                          const std::string& profile_id,
                          const std::string& token,
                          Bytes<E2E_PUBLIC_KEY_BYTES> key_public = {}) {
    PerformerJoinOptions options;
    options.room_id = room_id;
    options.room_handle = room_id;
    options.user_id = profile_id;
    options.display_name = profile_id;
    options.join_token = token;
    options.key_public = key_public;
    ClientJoinSession join_session(std::move(options));
    const JoinHdr join = join_session.make_join_header();
    send_control_packet(client, join, server);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::array<unsigned char, 2048> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = client.socket.receive_from(
            asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            throw std::runtime_error("room join receive failed: " + error.message());
        }
        if (bytes < sizeof(JoinAckHdr)) {
            continue;
        }
        JoinAckHdr ack{};
        std::memcpy(&ack, buffer.data(), sizeof(ack));
        if (ack.magic == CTRL_MAGIC && ack.type == CtrlHdr::Cmd::JOIN_ACK &&
            ack.participant_id != 0) {
            return ack.participant_id;
        }
    }
    throw std::runtime_error("created-room join timeout");
}

Bytes<E2E_PUBLIC_KEY_BYTES> protocol_public_key(
    const session_crypto::E2EPublicKey& key) {
    Bytes<E2E_PUBLIC_KEY_BYTES> result{};
    std::memcpy(result.data(), key.data(), result.size());
    return result;
}

session_crypto::E2EPublicKey crypto_public_key(
    const Bytes<E2E_PUBLIC_KEY_BYTES>& key) {
    session_crypto::E2EPublicKey result{};
    std::memcpy(result.data(), key.data(), result.size());
    return result;
}

Bytes<E2E_PUBLIC_KEY_BYTES> receive_participant_public_key(
    Client& client, uint32_t participant_id) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::array<unsigned char, 2048> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = client.socket.receive_from(
            asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            throw std::runtime_error("participant metadata receive failed: " +
                                     error.message());
        }
        if (bytes < sizeof(ParticipantInfoCapsHdr)) {
            continue;
        }
        ParticipantInfoCapsHdr info{};
        std::memcpy(&info, buffer.data(), sizeof(info));
        if (info.magic != CTRL_MAGIC ||
            info.type != CtrlHdr::Cmd::PARTICIPANT_INFO ||
            info.participant_id != participant_id ||
            !std::any_of(info.key_public.begin(), info.key_public.end(),
                         [](char value) { return value != 0; })) {
            continue;
        }
        return info.key_public;
    }
    throw std::runtime_error("joined participant public key was not broadcast");
}

E2EKeyEnvelopeHdr make_key_envelope(
    uint32_t sender_id, uint32_t target_id, uint32_t access_epoch,
    const std::string& commitment, const std::string& media_secret,
    const Bytes<E2E_PUBLIC_KEY_BYTES>& target_public_key) {
    if (!performer_join_token::valid_sha256_hex(commitment) ||
        media_secret.empty() || media_secret.size() > MEDIA_SECRET_MAX_BYTES) {
        throw std::runtime_error("invalid key envelope test input");
    }

    E2EKeyEnvelopePayload payload{};
    payload.access_epoch = access_epoch;
    payload.media_secret_bytes = static_cast<uint16_t>(media_secret.size());
    std::copy(media_secret.begin(), media_secret.end(), payload.media_secret.begin());

    E2EKeyEnvelopeHdr envelope{};
    envelope.magic = E2E_KEY_ENVELOPE_MAGIC;
    envelope.sender_id = sender_id;
    envelope.target_id = target_id;
    envelope.access_epoch = access_epoch;
    std::copy(commitment.begin(), commitment.end(),
              envelope.media_key_commitment.begin());
    size_t encrypted_bytes = 0;
    if (!session_crypto::seal_key_envelope(
            crypto_public_key(target_public_key),
            reinterpret_cast<const unsigned char*>(&payload), sizeof(payload),
            reinterpret_cast<unsigned char*>(envelope.encrypted.data()),
            envelope.encrypted.size(), encrypted_bytes) ||
        encrypted_bytes > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("key envelope seal failed");
    }
    envelope.encrypted_bytes = static_cast<uint16_t>(encrypted_bytes);
    return envelope;
}

std::optional<E2EKeyEnvelopeHdr> receive_key_envelope(
    Client& client, std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<unsigned char, 2048> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = client.socket.receive_from(
            asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            throw std::runtime_error("key envelope receive failed: " + error.message());
        }
        if (bytes != sizeof(E2EKeyEnvelopeHdr)) {
            continue;
        }
        E2EKeyEnvelopeHdr envelope{};
        std::memcpy(&envelope, buffer.data(), sizeof(envelope));
        if (envelope.magic == E2E_KEY_ENVELOPE_MAGIC) {
            return envelope;
        }
    }
    return std::nullopt;
}

RoomKeyHandoffAckHdr receive_key_handoff_ack(
    Client& client, uint32_t expected_sender_id, uint32_t expected_target_id) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::array<unsigned char, 2048> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = client.socket.receive_from(
            asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            throw std::runtime_error("key handoff ack receive failed: " +
                                     error.message());
        }
        if (bytes != sizeof(RoomKeyHandoffAckHdr)) {
            continue;
        }
        RoomKeyHandoffAckHdr ack{};
        std::memcpy(&ack, buffer.data(), sizeof(ack));
        if (ack.magic == CTRL_MAGIC &&
            ack.type == CtrlHdr::Cmd::ROOM_KEY_HANDOFF_ACK &&
            ack.participant_id == expected_sender_id &&
            ack.target_id == expected_target_id) {
            return ack;
        }
    }
    throw std::runtime_error("room key handoff ack was not relayed");
}

void verify_key_envelope_payload(
    const E2EKeyEnvelopeHdr& envelope,
    const session_crypto::E2EPublicKey& public_key,
    const session_crypto::E2ESecretKey& secret_key,
    const std::string& expected_media_secret, uint32_t expected_access_epoch) {
    std::array<unsigned char, sizeof(E2EKeyEnvelopePayload)> plaintext{};
    size_t plaintext_bytes = 0;
    if (!session_crypto::open_key_envelope(
            public_key, secret_key,
            reinterpret_cast<const unsigned char*>(envelope.encrypted.data()),
            envelope.encrypted_bytes, plaintext.data(), plaintext.size(),
            plaintext_bytes) ||
        plaintext_bytes != sizeof(E2EKeyEnvelopePayload)) {
        throw std::runtime_error("relayed key envelope could not be opened");
    }
    E2EKeyEnvelopePayload payload{};
    std::memcpy(&payload, plaintext.data(), sizeof(payload));
    const std::string media_secret(
        payload.media_secret.begin(),
        payload.media_secret.begin() + payload.media_secret_bytes);
    if (payload.reserved != 0 ||
        payload.access_epoch != expected_access_epoch ||
        payload.media_secret_bytes != expected_media_secret.size() ||
        media_secret != expected_media_secret) {
        throw std::runtime_error("relayed key envelope payload was invalid");
    }
}

void leave_room(Client& client, const udp::endpoint& server,
                uint32_t participant_id) {
    CtrlHdr leave{};
    leave.magic = CTRL_MAGIC;
    leave.type = CtrlHdr::Cmd::LEAVE;
    leave.participant_id = participant_id;
    send_control_packet(client, leave, server);
}

void verify_room_control_workflow(asio::io_context& io,
                                  const udp::endpoint& server) {
    Client creator(io);
    session_crypto::E2EPublicKey creator_public_key{};
    session_crypto::E2ESecretKey creator_secret_key{};
    session_crypto::E2EPublicKey joiner_public_key{};
    session_crypto::E2ESecretKey joiner_secret_key{};
    if (!session_crypto::make_e2e_keypair(creator_public_key,
                                          creator_secret_key) ||
        !session_crypto::make_e2e_keypair(joiner_public_key,
                                          joiner_secret_key)) {
        throw std::runtime_error("room control E2E keypair generation failed");
    }
    const auto initial_commitment = performer_join_token::try_sha256_hex(
        "relay-control-media-secret");
    if (!initial_commitment.has_value()) {
        throw std::runtime_error("room create commitment failed");
    }

    RoomCreateRequestHdr request{};
    request.magic = CTRL_MAGIC;
    request.type = CtrlHdr::Cmd::ROOM_CREATE_REQUEST;
    request.request_id = 73;
    packet_builder::write_fixed(request.room_id, "relay-control-room");
    packet_builder::write_fixed(request.room_name, "Relay Control Room");
    packet_builder::write_fixed(request.profile_id, "relay-control-profile");
    packet_builder::write_fixed(request.display_name, "Relay Control Probe");
    packet_builder::write_fixed(request.media_key_commitment, *initial_commitment);
    request.access_mode = ROOM_ACCESS_OPEN;
    send_control_packet(creator, request, server);
    const auto create_response = receive_room_response<RoomCreateResponseHdr>(
        creator, CtrlHdr::Cmd::ROOM_CREATE_RESPONSE, request.request_id);
    if (create_response.status != ROOM_STATUS_OK) {
        throw std::runtime_error(
            "room create rejected: " + fixed_string(create_response.reason));
    }

    std::string parse_reason;
    const std::string creator_token = fixed_string(create_response.join_token);
    const auto parsed_creator_token = performer_join_token::parse_unverified(
        creator_token, parse_reason);
    if (!parsed_creator_token.has_value() ||
        parsed_creator_token->claims.room_id != "relay-control-room" ||
        parsed_creator_token->claims.profile_id != "relay-control-profile" ||
        parsed_creator_token->claims.access_epoch != 1 ||
        parsed_creator_token->claims.media_key_commitment != *initial_commitment) {
        throw std::runtime_error("room create returned invalid join ticket");
    }
    const uint32_t creator_id = join_with_ticket(
        creator, server, "relay-control-room", "relay-control-profile",
        creator_token, protocol_public_key(creator_public_key));

    RoomAdminRequestHdr malformed_admin{};
    malformed_admin.magic = CTRL_MAGIC;
    malformed_admin.type = CtrlHdr::Cmd::ROOM_ADMIN_REQUEST;
    malformed_admin.request_id = 74;
    malformed_admin.command = ROOM_ADMIN_ROTATE_MEDIA_KEY;
    packet_builder::write_fixed(malformed_admin.room_id, "relay-control-room");
    packet_builder::write_fixed(malformed_admin.admin_token,
                                fixed_string(create_response.admin_token));
    packet_builder::write_fixed(malformed_admin.media_key_commitment, "short");
    send_control_packet(creator, malformed_admin, server);
    const auto malformed_response = receive_room_response<RoomAdminResponseHdr>(
        creator, CtrlHdr::Cmd::ROOM_ADMIN_RESPONSE,
        malformed_admin.request_id);
    if (malformed_response.status != ROOM_STATUS_BAD_REQUEST) {
        throw std::runtime_error("malformed room key commitment was not rejected");
    }

    const auto rotated_commitment = performer_join_token::try_sha256_hex(
        "relay-control-rotated-media-secret");
    if (!rotated_commitment.has_value()) {
        throw std::runtime_error("room rotation commitment failed");
    }
    RoomAdminRequestHdr rotate{};
    rotate.magic = CTRL_MAGIC;
    rotate.type = CtrlHdr::Cmd::ROOM_ADMIN_REQUEST;
    rotate.request_id = 75;
    rotate.command = ROOM_ADMIN_ROTATE_MEDIA_KEY;
    packet_builder::write_fixed(rotate.room_id, "relay-control-room");
    packet_builder::write_fixed(rotate.admin_token,
                                fixed_string(create_response.admin_token));
    packet_builder::write_fixed(rotate.media_key_commitment, *rotated_commitment);
    send_control_packet(creator, rotate, server);
    const auto rotate_response = receive_room_response<RoomAdminResponseHdr>(
        creator, CtrlHdr::Cmd::ROOM_ADMIN_RESPONSE, rotate.request_id);
    if (rotate_response.status != ROOM_STATUS_OK ||
        rotate_response.access_epoch != 2) {
        throw std::runtime_error("room admin key rotation failed");
    }

    Client joiner(io);
    RoomJoinTokenRequestHdr ticket_request{};
    ticket_request.magic = CTRL_MAGIC;
    ticket_request.type = CtrlHdr::Cmd::ROOM_JOIN_TOKEN_REQUEST;
    ticket_request.request_id = 76;
    packet_builder::write_fixed(ticket_request.room_id, "relay-control-room");
    packet_builder::write_fixed(ticket_request.profile_id, "relay-control-joiner");
    packet_builder::write_fixed(ticket_request.display_name, "Relay Control Joiner");
    send_control_packet(joiner, ticket_request, server);
    const auto ticket_response = receive_room_response<RoomJoinTokenResponseHdr>(
        joiner, CtrlHdr::Cmd::ROOM_JOIN_TOKEN_RESPONSE,
        ticket_request.request_id);
    if (ticket_response.status != ROOM_STATUS_OK) {
        throw std::runtime_error(
            "room join ticket rejected: " + fixed_string(ticket_response.reason));
    }
    const std::string joiner_token = fixed_string(ticket_response.join_token);
    const auto parsed_joiner_token = performer_join_token::parse_unverified(
        joiner_token, parse_reason);
    if (!parsed_joiner_token.has_value() ||
        parsed_joiner_token->claims.profile_id != "relay-control-joiner" ||
        parsed_joiner_token->claims.access_epoch != 2 ||
        parsed_joiner_token->claims.media_key_commitment != *rotated_commitment) {
        throw std::runtime_error("room join request returned stale key authority");
    }
    const uint32_t joiner_id = join_with_ticket(
        joiner, server, "relay-control-room", "relay-control-joiner",
        joiner_token, protocol_public_key(joiner_public_key));

    const auto advertised_joiner_key =
        receive_participant_public_key(creator, joiner_id);
    if (advertised_joiner_key != protocol_public_key(joiner_public_key)) {
        throw std::runtime_error("server changed the joined participant public key");
    }

    const std::string rotated_media_secret =
        "relay-control-rotated-media-secret";
    const auto key_envelope = make_key_envelope(
        creator_id, joiner_id, 2, *rotated_commitment,
        rotated_media_secret, advertised_joiner_key);
    send_control_packet(creator, key_envelope, server);
    const auto relayed_envelope = receive_key_envelope(joiner, 2s);
    if (!relayed_envelope.has_value() ||
        relayed_envelope->sender_id != creator_id ||
        relayed_envelope->target_id != joiner_id ||
        relayed_envelope->access_epoch != 2) {
        throw std::runtime_error("authorized room key envelope was not relayed");
    }
    verify_key_envelope_payload(*relayed_envelope, joiner_public_key,
                                joiner_secret_key, rotated_media_secret, 2);

    RoomKeyHandoffAckHdr key_ack{};
    key_ack.magic = CTRL_MAGIC;
    key_ack.type = CtrlHdr::Cmd::ROOM_KEY_HANDOFF_ACK;
    key_ack.participant_id = joiner_id;
    key_ack.target_id = creator_id;
    key_ack.access_epoch = 2;
    std::copy(rotated_commitment->begin(), rotated_commitment->end(),
              key_ack.media_key_commitment.begin());
    send_control_packet(joiner, key_ack, server);
    const auto relayed_ack =
        receive_key_handoff_ack(creator, joiner_id, creator_id);
    if (relayed_ack.access_epoch != 2 ||
        std::string(relayed_ack.media_key_commitment.begin(),
                    relayed_ack.media_key_commitment.end()) !=
            *rotated_commitment) {
        throw std::runtime_error("room key handoff ack authority was changed");
    }

    const auto stale_envelope = make_key_envelope(
        creator_id, joiner_id, 1, *initial_commitment,
        "relay-control-media-secret", advertised_joiner_key);
    send_control_packet(creator, stale_envelope, server);
    if (receive_key_envelope(joiner, 200ms).has_value()) {
        throw std::runtime_error("stale room key envelope was relayed");
    }
    leave_room(joiner, server, joiner_id);
    leave_room(creator, server, creator_id);
}

std::vector<unsigned char> make_join_packet(int index, const std::string& secret) {
    const std::string profile = "load-" + std::to_string(index);
    performer_join_token::Claims claims;
    claims.expires_at_ms = performer_join_token::now_ms() + 60'000;
    claims.server_id = "load-test";
    claims.room_id = "load-room";
    claims.profile_id = profile;
    claims.access_epoch = 1;
    claims.media_key_commitment = performer_join_token::try_sha256_hex(
        "relay-load-media-secret").value_or("");
    claims.nonce = "load-nonce-" + std::to_string(index);
    const auto token = performer_join_token::create(claims, secret);
    if (!token.has_value()) {
        throw std::runtime_error("failed to create join token");
    }

    JoinHdr join{};
    join.magic = CTRL_MAGIC;
    join.type = CtrlHdr::Cmd::JOIN;
    packet_builder::write_fixed(join.room_id, claims.room_id);
    packet_builder::write_fixed(join.room_handle, claims.room_id);
    packet_builder::write_fixed(join.profile_id, profile);
    packet_builder::write_fixed(join.display_name, profile);
    packet_builder::write_fixed(join.join_token, *token);
    join.capabilities = AUDIO_SUPPORTED_CAPABILITIES;

    std::vector<unsigned char> packet(sizeof(join));
    std::memcpy(packet.data(), &join, sizeof(join));
    return packet;
}

std::vector<unsigned char> make_audio_packet(
    Client& client, uint16_t frame_count,
    const session_crypto::SessionKey& media_key) {
    session_crypto::SecureAudioMetadata metadata;
    metadata.sender_id = client.id;
    metadata.sequence = client.sequence++;
    metadata.sample_rate = 48'000;
    metadata.frame_count = frame_count;
    metadata.plaintext_bytes = 1;
    metadata.channels = 1;
    metadata.codec = AudioCodec::Opus;
    metadata.capture_server_time_ns = steady_ns();

    const std::array<unsigned char, 1> plaintext{0xA5};
    std::vector<unsigned char> packet(
        SECURE_PACKET_HEADER_BYTES + plaintext.size() + SECURE_PACKET_TAG_BYTES);
    size_t packet_bytes = 0;
    if (!session_crypto::seal_audio_packet(
            media_key, metadata, plaintext.data(), plaintext.size(), packet.data(),
            packet.size(), packet_bytes)) {
        throw std::runtime_error("audio packet seal failed");
    }
    packet.resize(packet_bytes);
    return packet;
}

void drain_client(Client& client, const session_crypto::SessionKey& media_key,
                  uint64_t& received,
                  std::vector<double>& relay_latency_ms) {
    std::array<unsigned char, 2048> buffer{};
    for (;;) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = client.socket.receive_from(asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            return;
        }
        if (error) {
            throw std::runtime_error("receive failed: " + error.message());
        }
        std::array<unsigned char, 32> plaintext{};
        session_crypto::SecureAudioMetadata metadata;
        size_t plaintext_bytes = 0;
        if (!session_crypto::open_audio_packet(
                media_key, buffer.data(), bytes, metadata, plaintext.data(),
                plaintext.size(), plaintext_bytes) ||
            plaintext_bytes != 1 || plaintext[0] != 0xA5) {
            continue;
        }
        ++received;
        relay_latency_ms.push_back(
            static_cast<double>(steady_ns() - metadata.capture_server_time_ns) / 1e6);
    }
}

double percentile(std::vector<double>& values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(
        std::ceil(quantile * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

void verify_delivery_report_routing(std::vector<std::unique_ptr<Client>>& clients,
                                    const udp::endpoint& server,
                                    const session_crypto::SessionKey& media_key) {
    if (clients.size() < 2) {
        return;
    }
    AudioDeliveryReportPayload report{};
    report.report_epoch = 3;
    report.total_delivered = 97;
    report.total_lost = 3;
    session_crypto::SecureControlMetadata metadata;
    metadata.sender_id = clients[1]->id;
    metadata.target_id = clients[0]->id;
    metadata.sequence = 1;
    metadata.access_epoch = 1;
    metadata.plaintext_bytes = sizeof(report);
    metadata.media_key_commitment = performer_join_token::try_sha256_hex(
        "relay-load-media-secret").value_or("");
    std::vector<unsigned char> packet(
        SECURE_CONTROL_HEADER_BYTES + sizeof(report) + SECURE_PACKET_TAG_BYTES);
    size_t packet_bytes = 0;
    if (!session_crypto::seal_control_packet(
            media_key, metadata,
            reinterpret_cast<const unsigned char*>(&report), sizeof(report),
            packet.data(), packet.size(), packet_bytes)) {
        throw std::runtime_error("delivery report seal failed");
    }
    packet.resize(packet_bytes);
    clients[1]->socket.send_to(asio::buffer(packet), server);

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    std::array<unsigned char, 2048> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        udp::endpoint source;
        std::error_code error;
        const auto bytes = clients[0]->socket.receive_from(
            asio::buffer(buffer), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (error) {
            throw std::runtime_error("delivery report receive failed: " + error.message());
        }
        std::array<unsigned char, 512> plaintext{};
        session_crypto::SecureControlMetadata routed{};
        size_t plaintext_bytes = 0;
        if (!session_crypto::open_control_packet(
                media_key, buffer.data(), bytes, routed, plaintext.data(),
                plaintext.size(), plaintext_bytes)) {
            continue;
        }
        if (routed.target_id == clients[0]->id &&
            routed.sender_id == clients[1]->id && routed.sequence == 1 &&
            plaintext_bytes == sizeof(report)) {
            AudioDeliveryReportPayload opened{};
            std::memcpy(&opened, plaintext.data(), sizeof(opened));
            if (opened.command == SECURE_CONTROL_AUDIO_DELIVERY_REPORT &&
                opened.report_epoch == report.report_epoch &&
                opened.total_delivered == report.total_delivered &&
                opened.total_lost == report.total_lost) {
                return;
            }
        }
    }
    throw std::runtime_error("delivery report routing timeout");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const int port = option_int(argc, argv, "--port", 19999);
        const int client_count = option_int(argc, argv, "--clients", 8);
        const int sender_count = option_int(argc, argv, "--senders", client_count);
        const int room_participants =
            option_int(argc, argv, "--room-participants", client_count);
        const int total_senders = option_int(argc, argv, "--total-senders", sender_count);
        const int profile_offset = option_int(argc, argv, "--profile-offset", 0);
        const int start_delay_ms = option_int(argc, argv, "--start-delay-ms", 0);
        const int duration_ms = option_int(argc, argv, "--duration-ms", 500);
        const int frame_count = option_int(argc, argv, "--frames", 120);
        const int requested_rounds = option_int(argc, argv, "--rounds", 0);
        const double minimum_delivery =
            std::stod(option(argc, argv, "--min-delivery", "0.995"));
        const double maximum_receive_age_p999_ms =
            std::stod(option(argc, argv, "--max-receive-age-p999-ms", "5.0"));
        const std::string secret = option(argc, argv, "--secret", "load-secret");
        const auto media_key = session_crypto::derive_media_key_from_secret(
            "load-room", "relay-load-instance", "relay-load-media-secret");
        if (!media_key.has_value()) {
            throw std::runtime_error("relay media key derivation failed");
        }
        if (client_count < 1 || room_participants < 2 ||
            room_participants > static_cast<int>(MAX_ROOM_PARTICIPANTS) ||
            client_count > room_participants ||
            sender_count < 1 || sender_count > client_count || duration_ms <= 0 ||
            requested_rounds < 0 ||
            total_senders < sender_count || total_senders > room_participants ||
            !opus_network_clock::is_supported_frame_count(48'000, frame_count)) {
            throw std::invalid_argument("invalid load probe arguments");
        }

        asio::io_context io;
        const udp::endpoint server(asio::ip::make_address("127.0.0.1"),
                                   static_cast<uint16_t>(port));
        verify_room_control_workflow(io, server);
        std::vector<std::unique_ptr<Client>> clients;
        clients.reserve(static_cast<size_t>(client_count));
        for (int i = 0; i < client_count; ++i) {
            clients.push_back(std::make_unique<Client>(io));
            const auto join = make_join_packet(profile_offset + i, secret);
            clients.back()->socket.send_to(asio::buffer(join), server);
        }

        const auto join_deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < join_deadline) {
            bool all_joined = true;
            for (auto& client: clients) {
                std::array<unsigned char, 2048> buffer{};
                for (;;) {
                    udp::endpoint source;
                    std::error_code error;
                    const auto bytes = client->socket.receive_from(asio::buffer(buffer), source,
                                                                    0, error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error) {
                        throw std::runtime_error("join receive failed: " + error.message());
                    }
                    if (bytes >= sizeof(JoinAckHdr)) {
                        JoinAckHdr ack{};
                        std::memcpy(&ack, buffer.data(), sizeof(ack));
                        if (ack.magic == CTRL_MAGIC && ack.type == CtrlHdr::Cmd::JOIN_ACK) {
                            client->id = ack.participant_id;
                        }
                    }
                }
                all_joined = all_joined && client->id != 0;
            }
            if (all_joined) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        for (const auto& client: clients) {
            if (client->id == 0) {
                throw std::runtime_error("join timeout");
            }
        }
        verify_delivery_report_routing(clients, server, *media_key);
        if (start_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(start_delay_ms));
        }

        uint64_t received = 0;
        std::vector<double> relay_latency_ms;
        const auto packet_period = std::chrono::duration<double>(
            static_cast<double>(frame_count) / 48'000.0);
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::milliseconds(duration_ms);
        auto next_send = started;
        uint64_t sent = 0;
        uint64_t rounds_sent = 0;
        while ((requested_rounds > 0
                    ? rounds_sent < static_cast<uint64_t>(requested_rounds)
                    : std::chrono::steady_clock::now() < deadline)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_send) {
                for (int i = 0; i < sender_count; ++i) {
                    const auto packet = make_audio_packet(
                        *clients[i], static_cast<uint16_t>(frame_count), *media_key);
                    send_audio(*clients[i], packet, server);
                    ++sent;
                }
                next_send += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    packet_period);
                ++rounds_sent;
            }
            for (auto& client: clients) {
                drain_client(*client, *media_key, received, relay_latency_ms);
            }
            std::this_thread::yield();
        }

        const auto drain_deadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < drain_deadline) {
            const auto before = received;
            for (auto& client: clients) {
                drain_client(*client, *media_key, received, relay_latency_ms);
            }
            if (before == received) {
                std::this_thread::sleep_for(1ms);
            }
        }

        const uint64_t rounds = sent / static_cast<uint64_t>(sender_count);
        const uint64_t expected = rounds *
            (static_cast<uint64_t>(total_senders) * static_cast<uint64_t>(client_count) -
             static_cast<uint64_t>(sender_count));
        const double delivery = expected == 0 ? 0.0 :
            static_cast<double>(received) / static_cast<double>(expected);
        const double p999_ms = percentile(relay_latency_ms, 0.999);
        const double maximum_ms = relay_latency_ms.empty() ? 0.0 : relay_latency_ms.back();
        std::cout << "{\"room_control_workflow\":true"
                  << ",\"clients\":" << client_count
                  << ",\"room_participants\":" << room_participants
                  << ",\"senders\":" << sender_count
                  << ",\"frame_count\":" << frame_count
                  << ",\"sent\":" << sent
                  << ",\"expected_relays\":" << expected
                  << ",\"received_relays\":" << received
                  << ",\"delivery_ratio\":" << delivery
                  << ",\"receive_age_p999_ms\":" << p999_ms
                  << ",\"receive_age_max_ms\":" << maximum_ms << "}\n";
        if (delivery < minimum_delivery || p999_ms > maximum_receive_age_p999_ms) {
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "relay load probe failed: " << error.what() << '\n';
        return 2;
    }
}
