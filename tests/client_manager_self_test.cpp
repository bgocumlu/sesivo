#include <algorithm>
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

    auto first_join = manager.register_client(first, now, "room-a", "instance-a", 1,
                                              "user-a", "User A");
    auto receiver_join = manager.register_client(receiver, now, "room-a", "instance-a", 1,
                                                 "user-b",
                                                 "User B");
    auto retry_same_endpoint = manager.register_client(first, now + 1s, "room-a",
                                                       "instance-a", 1,
                                                       "user-a", "User A");
    auto duplicate_join = manager.register_client(second, now + 2s, "room-a",
                                                  "instance-a", 1,
                                                  "user-a", "User A");

    const auto second_room = manager.get_cached_room_endpoints(second);
    const auto first_room = manager.get_cached_room_endpoints(first);

    std::cout << "first_id=" << first_join.client_id << "\n";
    std::cout << "receiver_id=" << receiver_join.client_id << "\n";
    std::cout << "retry_id=" << retry_same_endpoint.client_id << "\n";
    std::cout << "duplicate_id=" << duplicate_join.client_id << "\n";
    std::cout << "duplicate_removed_count=" << duplicate_join.removed_client_ids.size() << "\n";
    std::cout << "manager_count=" << manager.count() << "\n";
    std::cout << "second_room=" << second_room->size() << "\n";
    std::cout << "first_room=" << first_room->size() << "\n";

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
    if (second_room->size() != 2 ||
        std::find(second_room->begin(), second_room->end(), second) == second_room->end() ||
        std::find(second_room->begin(), second_room->end(), receiver) == second_room->end()) {
        std::cerr << "active duplicate sender room cache is incorrect\n";
        return 7;
    }
    if (!first_room->empty()) {
        std::cerr << "stale endpoint still has a room cache\n";
        return 8;
    }

    ClientManager::ClientSecurityConfig security;
    security.token_nonce_key = "secure-room|secure-user|nonce-a";
    auto secure_join = manager.register_client(
        secure, now + 3s, "secure-room", "secure-instance", 1,
        "secure-user", "Secure User",
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
        secure, now + 4s, "secure-room", "secure-instance", 1,
        "secure-user", "Secure User",
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
        secure, now + 5s, "secure-room", "secure-instance", 2,
        "secure-user", "Secure User",
        AUDIO_CAP_SECURE_AUDIO, {},
        std::optional<ClientManager::ClientSecurityConfig>{rotated_security});
    if (!manager.has_authenticated_session(secure) ||
        manager.token_nonce_key_for(secure) != rotated_security.token_nonce_key) {
        std::cerr << "rotated authenticated session did not replace token nonce state\n";
        return 12;
    }

    ClientManager capacity_manager;
    for (size_t index = 0; index < MAX_ROOM_PARTICIPANTS; ++index) {
        const udp::endpoint endpoint(asio::ip::make_address("127.0.0.1"),
                                     static_cast<unsigned short>(11000 + index));
        const auto registration = capacity_manager.register_client(
            endpoint, now, "room-a", "instance-a", 1,
            "capacity-user-" + std::to_string(index),
            "Capacity User " + std::to_string(index));
        if (registration.client_id == 0 || registration.rejected_room_full) {
            std::cerr << "room rejected a participant before reaching capacity\n";
            return 13;
        }
    }

    const udp::endpoint overflow(asio::ip::make_address("127.0.0.1"), 12000);
    const auto overflow_registration = capacity_manager.register_client(
        overflow, now, "room-a", "instance-a", 1, "overflow-user", "Overflow User");
    const auto counts_after_overflow = capacity_manager.room_counts();
    if (!overflow_registration.rejected_room_full || overflow_registration.client_id != 0 ||
        capacity_manager.exists(overflow) || counts_after_overflow.at("room-a") !=
                                                 MAX_ROOM_PARTICIPANTS) {
        std::cerr << "room capacity rejection changed the full room\n";
        return 14;
    }

    const auto other_room_registration = capacity_manager.register_client(
        overflow, now, "room-b", "instance-b", 1, "overflow-user", "Overflow User");
    if (other_room_registration.client_id == 0 ||
        other_room_registration.rejected_room_full) {
        std::cerr << "room capacity was not scoped per room\n";
        return 15;
    }

    const udp::endpoint existing_member(asio::ip::make_address("127.0.0.1"), 11000);
    const auto existing_registration = capacity_manager.register_client(
        existing_member, now + 1s, "room-a", "instance-a", 1,
        "capacity-user-0", "Capacity User 0");
    if (existing_registration.client_id == 0 || existing_registration.rejected_room_full) {
        std::cerr << "full room rejected an existing member re-registering\n";
        return 16;
    }

    ClientManager cache_manager;
    const udp::endpoint cache_first(asio::ip::make_address("127.0.0.1"), 13001);
    const udp::endpoint cache_second(asio::ip::make_address("127.0.0.1"), 13002);
    const udp::endpoint cache_third(asio::ip::make_address("127.0.0.1"), 13003);
    cache_manager.register_client(cache_first, now, "cache-room", "cache-instance", 1,
                                  "cache-user-1", "Cache User 1");
    cache_manager.register_client(cache_second, now, "cache-room", "cache-instance", 1,
                                  "cache-user-2", "Cache User 2");
    const auto cached_before = cache_manager.get_cached_room_endpoints(cache_first);
    const auto cached_again = cache_manager.get_cached_room_endpoints(cache_second);
    if (cached_before != cached_again || cached_before->size() != 2) {
        std::cerr << "room endpoint list was not reused from the cache\n";
        return 17;
    }

    cache_manager.register_client(cache_third, now, "cache-room", "cache-instance", 1,
                                  "cache-user-3", "Cache User 3");
    const auto cached_after_join = cache_manager.get_cached_room_endpoints(cache_first);
    if (cached_after_join == cached_before || cached_after_join->size() != 3) {
        std::cerr << "room endpoint cache was not invalidated after join\n";
        return 18;
    }

    cache_manager.remove_client_with_info(cache_second);
    const auto cached_after_leave = cache_manager.get_cached_room_endpoints(cache_first);
    if (cached_after_leave == cached_after_join || cached_after_leave->size() != 2 ||
        std::find(cached_after_leave->begin(), cached_after_leave->end(), cache_second) !=
            cached_after_leave->end()) {
        std::cerr << "room endpoint cache was not invalidated after leave\n";
        return 19;
    }

    return 0;
}
