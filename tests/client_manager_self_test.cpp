#include <chrono>
#include <iostream>
#include <optional>

#include <asio/ip/address.hpp>
#include <asio/ip/udp.hpp>

#include "client_manager.h"

using asio::ip::udp;
using namespace std::chrono_literals;

int main() {
    ClientManager manager;
    const auto now = std::chrono::steady_clock::now();

    const udp::endpoint first(asio::ip::make_address("127.0.0.1"), 10001);
    const udp::endpoint second(asio::ip::make_address("127.0.0.1"), 10002);
    const udp::endpoint receiver(asio::ip::make_address("127.0.0.1"), 10003);
    const udp::endpoint secure(asio::ip::make_address("127.0.0.1"), 10004);

    auto first_join = manager.register_client(first, now, "room-a", "user-a", "User A");
    auto receiver_join = manager.register_client(receiver, now, "room-a", "user-b",
                                                 "User B");
    auto retry_same_endpoint = manager.register_client(first, now + 1s, "room-a",
                                                       "user-a", "User A");
    auto duplicate_join = manager.register_client(second, now + 2s, "room-a",
                                                  "user-a", "User A");

    const auto second_targets = manager.get_room_endpoints_except(second);
    const auto first_targets = manager.get_room_endpoints_except(first);

    std::cout << "first_id=" << first_join.client_id << "\n";
    std::cout << "receiver_id=" << receiver_join.client_id << "\n";
    std::cout << "retry_id=" << retry_same_endpoint.client_id << "\n";
    std::cout << "duplicate_id=" << duplicate_join.client_id << "\n";
    std::cout << "duplicate_removed_count=" << duplicate_join.removed_client_ids.size() << "\n";
    std::cout << "manager_count=" << manager.count() << "\n";
    std::cout << "second_targets=" << second_targets.size() << "\n";
    std::cout << "first_targets=" << first_targets.size() << "\n";

    if (first_join.client_id == 0 || receiver_join.client_id == 0) {
        std::cerr << "initial joins failed\n";
        return 1;
    }
    if (retry_same_endpoint.client_id != first_join.client_id ||
        !retry_same_endpoint.removed_client_ids.empty()) {
        std::cerr << "same-endpoint retry did not preserve participant identity\n";
        return 2;
    }
    if (duplicate_join.client_id == first_join.client_id) {
        std::cerr << "duplicate endpoint reused stale participant id\n";
        return 3;
    }
    if (duplicate_join.removed_client_ids.size() != 1 ||
        duplicate_join.removed_client_ids.front() != first_join.client_id) {
        std::cerr << "duplicate endpoint did not remove stale participant\n";
        return 4;
    }
    if (manager.exists(first)) {
        std::cerr << "stale endpoint still exists\n";
        return 5;
    }
    if (!manager.exists(second) || !manager.exists(receiver) || manager.count() != 2) {
        std::cerr << "live endpoints were not retained correctly\n";
        return 6;
    }
    if (second_targets.size() != 1 || second_targets.front() != receiver) {
        std::cerr << "active duplicate sender does not forward exactly to receiver\n";
        return 7;
    }
    if (!first_targets.empty()) {
        std::cerr << "stale endpoint still has forwarding targets\n";
        return 8;
    }

    ClientManager::ClientSecurityConfig security;
    security.token_nonce_key = "secure-room|secure-user|nonce-a";
    auto secure_join = manager.register_client(
        secure, now + 3s, "secure-room", "secure-user", "Secure User",
        AUDIO_CAP_SECURE_AUDIO, {},
        std::optional<ClientManager::ClientSecurityConfig>{security});
    if (secure_join.client_id == 0) {
        std::cerr << "authenticated session was not registered\n";
        return 9;
    }
    if (!manager.has_authenticated_session(secure) ||
        manager.token_nonce_key_for(secure) != security.token_nonce_key) {
        std::cerr << "authenticated session accessors failed\n";
        return 10;
    }
    auto secure_retry = manager.register_client(
        secure, now + 4s, "secure-room", "secure-user", "Secure User",
        AUDIO_CAP_SECURE_AUDIO, {},
        std::optional<ClientManager::ClientSecurityConfig>{security});
    if (secure_retry.client_id != secure_join.client_id ||
        manager.token_nonce_key_for(secure) != security.token_nonce_key) {
        std::cerr << "same authenticated session retry changed auth state\n";
        return 11;
    }
    auto rotated_security = security;
    rotated_security.token_nonce_key = "secure-room|secure-user|nonce-b";
    manager.register_client(
        secure, now + 5s, "secure-room", "secure-user", "Secure User",
        AUDIO_CAP_SECURE_AUDIO, {},
        std::optional<ClientManager::ClientSecurityConfig>{rotated_security});
    if (!manager.has_authenticated_session(secure) ||
        manager.token_nonce_key_for(secure) != rotated_security.token_nonce_key) {
        std::cerr << "rotated authenticated session did not replace token nonce state\n";
        return 12;
    }

    return 0;
}
