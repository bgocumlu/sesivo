#include "secure_invite.h"

#include "performer_join_token.h"
#include "udp_port.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace {

std::string lowercase_copy(std::string value) {
    for (auto& c: value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

int hex_digit_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string url_decode_component(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hex_digit_value(value[i + 1]);
            const int low = hex_digit_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

std::string url_encode_component(const std::string& value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char c: value) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[c >> 4]);
            encoded.push_back(digits[c & 0x0F]);
        }
    }
    return encoded;
}

void apply_invite_field(SecureInvite& invite, const std::string& key,
                        const std::string& value, std::string& reason) {
    const auto normalized_key = lowercase_copy(url_decode_component(key));
    const auto decoded_value = url_decode_component(value);
    if (normalized_key == "server") {
        invite.server_address = decoded_value;
    } else if (normalized_key == "port") {
        try {
            invite.server_port = parse_udp_port(decoded_value, "invite port");
        } catch (const std::exception& e) {
            reason = e.what();
        }
    } else if (normalized_key == "room") {
        invite.room_id = decoded_value;
    }
}

void parse_invite_pairs(SecureInvite& invite, const std::string& pairs,
                        std::string& reason) {
    size_t offset = 0;
    while (offset <= pairs.size() && reason.empty()) {
        const size_t next = pairs.find('&', offset);
        const auto pair = pairs.substr(
            offset, next == std::string::npos ? std::string::npos : next - offset);
        if (!pair.empty()) {
            const size_t equals = pair.find('=');
            if (equals == std::string::npos) {
                reason = "Invite link is malformed";
                return;
            }
            apply_invite_field(invite, pair.substr(0, equals),
                               pair.substr(equals + 1), reason);
        }
        if (next == std::string::npos) {
            break;
        }
        offset = next + 1;
    }
}

std::optional<std::pair<size_t, size_t>> find_invite_uri(const std::string& text) {
    const auto normalized = lowercase_copy(text);
    for (const std::string prefix: {"sesivo://join?", "sesivo://join/?"}) {
        const size_t start = normalized.find(prefix);
        if (start != std::string::npos) {
            return std::make_pair(start, start + prefix.size());
        }
    }
    return std::nullopt;
}

}  // namespace

std::string make_media_secret() {
    return performer_join_token::random_nonce() + performer_join_token::random_nonce();
}

std::string make_secure_invite_link(const std::string& server_address,
                                    uint16_t server_port,
                                    const std::string& room_id) {
    return "sesivo://join?server=" + url_encode_component(server_address) +
           "&port=" + std::to_string(server_port) +
           "&room=" + url_encode_component(room_id);
}

bool contains_secure_invite_link(const std::string& text) {
    return lowercase_copy(text).find("sesivo://join") != std::string::npos;
}

std::optional<SecureInvite> parse_secure_invite_text(const std::string& text,
                                                     std::string& reason) {
    reason.clear();
    const auto uri_bounds = find_invite_uri(text);
    if (!uri_bounds.has_value()) {
        reason = "Paste a Sesivo invite link";
        return std::nullopt;
    }

    const size_t start = uri_bounds->first;
    const size_t query_start = uri_bounds->second;
    const size_t end = text.find_first_of(" \t\r\n\"'", start);
    const std::string uri =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t query_offset = query_start - start;
    const size_t fragment_start = uri.find('#', query_offset);
    const std::string query =
        uri.substr(query_offset,
                   fragment_start == std::string::npos
                       ? std::string::npos
                       : fragment_start - query_offset);

    SecureInvite invite;
    parse_invite_pairs(invite, query, reason);
    if (!reason.empty()) {
        return std::nullopt;
    }

    if (invite.server_address.empty() || invite.server_port == 0 ||
        invite.room_id.empty()) {
        reason = "Invite link is incomplete";
        return std::nullopt;
    }
    return invite;
}

bool invite_matches_room(const SecureInvite& invite,
                         const std::string& server_address,
                         uint16_t server_port,
                         const std::string& room_id) {
    return invite.server_address == server_address &&
           invite.server_port == server_port && invite.room_id == room_id;
}
