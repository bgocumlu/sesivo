#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sodium.h>

namespace performer_join_token {

struct Claims {
    int64_t     expires_at_ms = 0;
    std::string server_id;
    std::string room_id;
    std::string profile_id;
    std::string room_instance_id;
    uint32_t    access_epoch = 0;
    std::string nonce;
};

struct ValidationResult {
    bool        ok = false;
    std::string reason;
};

struct ValidatedToken {
    bool        ok = false;
    std::string reason;
    Claims      claims;
    std::string signature_hex;
    std::string signing_input;
};

inline int64_t now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

inline std::string hex(const std::vector<unsigned char>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte: bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

inline std::string base64url_encode(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((input.size() * 4 + 2) / 3);

    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char byte: input) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        output.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
    }
    return output;
}

inline std::optional<std::string> base64url_decode(const std::string& input) {
    std::array<int, 256> values{};
    values.fill(-1);
    for (int i = 0; i < 26; ++i) {
        values[static_cast<unsigned char>('A' + i)] = i;
        values[static_cast<unsigned char>('a' + i)] = 26 + i;
    }
    for (int i = 0; i < 10; ++i) {
        values[static_cast<unsigned char>('0' + i)] = 52 + i;
    }
    values[static_cast<unsigned char>('-')] = 62;
    values[static_cast<unsigned char>('_')] = 63;

    std::string output;
    output.reserve((input.size() * 3) / 4);
    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char c: input) {
        const int value = values[c];
        if (value < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return output;
}

inline bool ensure_sodium_initialized() {
    return sodium_init() >= 0;
}

inline const unsigned char* sodium_input_data(const std::vector<unsigned char>& bytes) {
    static constexpr unsigned char empty = 0;
    return bytes.empty() ? &empty : bytes.data();
}

inline const unsigned char* sodium_input_data(const std::string& text) {
    static constexpr unsigned char empty = 0;
    return text.empty() ? &empty : reinterpret_cast<const unsigned char*>(text.data());
}

inline std::optional<std::vector<unsigned char>> try_sha256(
    const std::vector<unsigned char>& bytes) {
    std::vector<unsigned char> digest(crypto_hash_sha256_BYTES);
    if (!ensure_sodium_initialized()) {
        return std::nullopt;
    }
    crypto_hash_sha256(digest.data(), sodium_input_data(bytes),
                       static_cast<unsigned long long>(bytes.size()));
    return digest;
}

inline std::optional<std::string> try_hmac_sha256_hex(const std::string& secret,
                                                      const std::string& message) {
    if (!ensure_sodium_initialized()) {
        return std::nullopt;
    }
    std::array<unsigned char, crypto_auth_hmacsha256_BYTES> digest{};
    crypto_auth_hmacsha256_state state{};
    const auto* key = sodium_input_data(secret);
    const auto* data = sodium_input_data(message);
    crypto_auth_hmacsha256_init(&state, key, secret.size());
    crypto_auth_hmacsha256_update(&state, data,
                                  static_cast<unsigned long long>(message.size()));
    crypto_auth_hmacsha256_final(&state, digest.data());
    return hex(std::vector<unsigned char>(digest.begin(), digest.end()));
}

inline void append_claim_field(std::string& payload, const std::string& value) {
    payload += std::to_string(value.size());
    payload.push_back(':');
    payload += value;
}

inline std::string claims_payload(const Claims& claims) {
    std::string payload;
    append_claim_field(payload, std::to_string(claims.expires_at_ms));
    append_claim_field(payload, claims.server_id);
    append_claim_field(payload, claims.room_id);
    append_claim_field(payload, claims.profile_id);
    append_claim_field(payload, claims.room_instance_id);
    append_claim_field(payload, std::to_string(claims.access_epoch));
    append_claim_field(payload, claims.nonce);
    return payload;
}

inline bool read_claim_field(const std::string& payload, size_t& offset,
                             std::string& value) {
    const size_t colon = payload.find(':', offset);
    if (colon == std::string::npos || colon == offset) {
        return false;
    }
    for (size_t i = offset; i < colon; ++i) {
        if (payload[i] < '0' || payload[i] > '9') {
            return false;
        }
    }

    size_t length = 0;
    try {
        length = static_cast<size_t>(std::stoull(payload.substr(offset, colon - offset)));
    } catch (...) {
        return false;
    }

    const size_t value_offset = colon + 1;
    if (length > payload.size() - value_offset) {
        return false;
    }
    value = payload.substr(value_offset, length);
    offset = value_offset + length;
    return true;
}

inline std::optional<Claims> claims_from_payload(const std::string& payload,
                                                 std::string& reason) {
    std::array<std::string, 7> fields;
    size_t offset = 0;
    for (auto& field: fields) {
        if (!read_claim_field(payload, offset, field)) {
            reason = "malformed token payload";
            return std::nullopt;
        }
    }
    if (offset != payload.size()) {
        reason = "malformed token payload";
        return std::nullopt;
    }

    Claims claims;
    try {
        claims.expires_at_ms = std::stoll(fields[0]);
    } catch (...) {
        reason = "malformed expiry";
        return std::nullopt;
    }
    claims.server_id = fields[1];
    claims.room_id = fields[2];
    claims.profile_id = fields[3];
    claims.room_instance_id = fields[4];
    try {
        const unsigned long epoch = std::stoul(fields[5]);
        if (epoch > UINT32_MAX) {
            reason = "malformed access epoch";
            return std::nullopt;
        }
        claims.access_epoch = static_cast<uint32_t>(epoch);
    } catch (...) {
        reason = "malformed access epoch";
        return std::nullopt;
    }
    claims.nonce = fields[6];
    return claims;
}

inline std::string signing_message(const Claims& claims) {
    return "v2|" + claims_payload(claims);
}

inline std::string random_nonce() {
    std::random_device              random;
    std::uniform_int_distribution<> hex_digit(0, 15);
    static constexpr char           digits[] = "0123456789abcdef";
    std::string                     nonce;
    nonce.reserve(32);
    for (int i = 0; i < 32; ++i) {
        nonce.push_back(digits[hex_digit(random)]);
    }
    return nonce;
}

inline std::optional<std::string> try_sign(const Claims& claims,
                                           const std::string& secret) {
    return try_hmac_sha256_hex(secret, signing_message(claims));
}

inline std::optional<std::string> create(const Claims& claims, const std::string& secret) {
    auto signature = try_sign(claims, secret);
    if (!signature.has_value()) {
        return std::nullopt;
    }
    return "v2." + base64url_encode(claims_payload(claims)) + "." + *signature;
}

inline std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string              part;
    std::istringstream       stream(value);
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

inline bool constant_time_equal(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

inline std::optional<ValidatedToken> parse_unverified(const std::string& token,
                                                      std::string& reason) {
    const auto parts = split(token, '.');
    if (parts.size() != 3 || parts[0] != "v2") {
        reason = "malformed token";
        return std::nullopt;
    }

    const auto payload = base64url_decode(parts[1]);
    if (!payload.has_value()) {
        reason = "malformed token payload";
        return std::nullopt;
    }

    auto claims = claims_from_payload(*payload, reason);
    if (!claims.has_value()) {
        return std::nullopt;
    }

    ValidatedToken parsed;
    parsed.claims = std::move(*claims);
    parsed.signature_hex = parts[2];
    parsed.signing_input = signing_message(parsed.claims);
    parsed.ok = true;
    return parsed;
}

inline ValidatedToken validate_with_claims(const std::string& token, const std::string& secret,
                                           const std::string& expected_server_id,
                                           const std::string& expected_room_id,
                                           const std::string& expected_profile_id,
                                           const std::string& expected_room_instance_id = {},
                                           uint32_t expected_access_epoch = 0) {
    ValidatedToken detailed;

    if (secret.empty()) {
        detailed.reason = "join secret not configured";
        return detailed;
    }

    std::string parse_reason;
    auto parsed = parse_unverified(token, parse_reason);
    if (!parsed.has_value()) {
        detailed.reason = parse_reason;
        return detailed;
    }

    Claims claims = parsed->claims;
    detailed.claims = claims;
    detailed.signature_hex = parsed->signature_hex;
    detailed.signing_input = parsed->signing_input;

    if (claims.expires_at_ms < now_ms()) {
        detailed.reason = "expired token";
        return detailed;
    }
    if (claims.server_id != expected_server_id) {
        detailed.reason = "wrong server id";
        return detailed;
    }
    if (claims.room_id != expected_room_id) {
        detailed.reason = "wrong room id";
        return detailed;
    }
    if (claims.profile_id != expected_profile_id) {
        detailed.reason = "wrong profile id";
        return detailed;
    }
    if (!expected_room_instance_id.empty() &&
        claims.room_instance_id != expected_room_instance_id) {
        detailed.reason = "wrong room instance";
        return detailed;
    }
    if (expected_access_epoch != 0 && claims.access_epoch != expected_access_epoch) {
        detailed.reason = "wrong room access epoch";
        return detailed;
    }
    const auto expected_signature = try_sign(claims, secret);
    if (!expected_signature.has_value()) {
        detailed.reason = "crypto unavailable";
        return detailed;
    }
    if (!constant_time_equal(*expected_signature, parsed->signature_hex)) {
        detailed.reason = "invalid signature";
        return detailed;
    }

    detailed.ok = true;
    return detailed;
}

inline ValidationResult validate(const std::string& token, const std::string& secret,
                                 const std::string& expected_server_id,
                                 const std::string& expected_room_id,
                                 const std::string& expected_profile_id,
                                 const std::string& expected_room_instance_id = {},
                                 uint32_t expected_access_epoch = 0) {
    const auto detailed = validate_with_claims(token, secret, expected_server_id,
                                               expected_room_id, expected_profile_id,
                                               expected_room_instance_id,
                                               expected_access_epoch);
    return {detailed.ok, detailed.reason};
}

}  // namespace performer_join_token
