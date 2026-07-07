#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio/ip/udp.hpp>

#include "client_info.h"
#include "endpoint_hash.h"

// Thread-safe client lifecycle manager
class ClientManager {
public:
    using endpoint   = asio::ip::udp::endpoint;
    using time_point = std::chrono::steady_clock::time_point;

    ClientManager() : next_client_id_(1) {}

    struct RegistrationResult {
        uint32_t              client_id = 0;
        std::vector<uint32_t> removed_client_ids;
    };

    struct ClientSecurityConfig {
        std::string token_nonce_key;
    };

    RegistrationResult register_client(const endpoint& ep, time_point now, std::string room_id,
                                       std::string profile_id, std::string display_name,
                                       uint32_t capabilities = 0,
                                       Bytes<E2E_PUBLIC_KEY_BYTES> key_public = {},
                                       std::optional<ClientSecurityConfig> security =
                                           std::nullopt) {
        std::lock_guard<std::mutex> lock(mutex_);

        RegistrationResult result;
        auto existing = clients_.find(ep);
        const bool can_reuse_existing =
            existing != clients_.end() && existing->second.room_id == room_id &&
            existing->second.profile_id == profile_id;

        if (can_reuse_existing) {
            auto& client = existing->second;
            client.last_alive           = now;
            client.display_name         = std::move(display_name);
            client.capabilities         = capabilities;
            client.key_public           = key_public;
            client.joined_with_metadata = true;
            apply_security(client, security);
            result.client_id            = client.client_id;
        } else {
            if (existing != clients_.end()) {
                result.removed_client_ids.push_back(existing->second.client_id);
                clients_.erase(existing);
            }

            ClientInfo client;
            client.client_id = next_client_id_++;
            client.joined_at = now;
            client.last_alive = now;
            client.room_id = room_id;
            client.profile_id = profile_id;
            client.display_name = std::move(display_name);
            client.capabilities = capabilities;
            client.key_public = key_public;
            client.joined_with_metadata = true;
            apply_security(client, security);
            result.client_id = client.client_id;
            clients_[ep] = std::move(client);
        }

        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->first != ep && it->second.room_id == room_id &&
                it->second.profile_id == profile_id) {
                result.removed_client_ids.push_back(it->second.client_id);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }

        return result;
    }

    // Update client last_alive timestamp
    void update_alive(const endpoint& ep, time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        if (it != clients_.end()) {
            it->second.last_alive = now;
        }
    }

    std::optional<ClientInfo> remove_client_with_info(const endpoint& ep) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        if (it == clients_.end()) {
            return std::nullopt;
        }
        ClientInfo info = it->second;
        clients_.erase(it);
        return info;
    }

    std::optional<std::pair<endpoint, ClientInfo>> remove_room_client_with_info(
        const std::string& room_id, uint32_t client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->second.room_id == room_id && it->second.client_id == client_id) {
                auto removed = std::make_pair(it->first, it->second);
                clients_.erase(it);
                return removed;
            }
        }
        return std::nullopt;
    }

    std::vector<std::pair<endpoint, ClientInfo>> remove_room_clients_with_info(
        const std::string& room_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<endpoint, ClientInfo>> removed;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->second.room_id == room_id) {
                removed.emplace_back(it->first, it->second);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        return removed;
    }

    // Check if client exists
    bool exists(const endpoint& ep) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.contains(ep);
    }

    // Get client ID (returns 0 if not found)
    uint32_t get_client_id(const endpoint& ep) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        return it != clients_.end() ? it->second.client_id : 0;
    }

    uint32_t get_client_capabilities(const endpoint& ep) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        return it != clients_.end() ? it->second.capabilities : 0;
    }

    bool has_authenticated_session(const endpoint& ep) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        return it != clients_.end() && it->second.has_authenticated_session;
    }

    std::optional<std::string> token_nonce_key_for(const endpoint& ep) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        if (it == clients_.end() || !it->second.has_authenticated_session) {
            return std::nullopt;
        }
        return it->second.token_nonce_key;
    }

    // Get count of clients
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.size();
    }

    // Get all client endpoints (copy for safe iteration)
    std::vector<endpoint> get_all_endpoints() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<endpoint>       endpoints;
        endpoints.reserve(clients_.size());
        for (const auto& [ep, info]: clients_) {
            endpoints.push_back(ep);
        }
        return endpoints;
    }

    std::unordered_map<std::string, size_t> room_counts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unordered_map<std::string, size_t> counts;
        for (const auto& [_, info]: clients_) {
            ++counts[info.room_id];
        }
        return counts;
    }

    std::vector<endpoint> get_room_endpoints_except(const endpoint& exclude) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<endpoint>       endpoints;
        auto                        sender_it = clients_.find(exclude);
        if (sender_it == clients_.end()) {
            return endpoints;
        }

        const std::string& room_id = sender_it->second.room_id;
        endpoints.reserve(clients_.size());
        for (const auto& [ep, info]: clients_) {
            if (ep != exclude && info.room_id == room_id) {
                endpoints.push_back(ep);
            }
        }
        return endpoints;
    }

    std::vector<std::pair<endpoint, ClientInfo>> get_room_clients_except(
        const endpoint& exclude) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<endpoint, ClientInfo>> clients;
        auto sender_it = clients_.find(exclude);
        if (sender_it == clients_.end()) {
            return clients;
        }

        const std::string& room_id = sender_it->second.room_id;
        clients.reserve(clients_.size());
        for (const auto& [ep, info]: clients_) {
            if (ep != exclude && info.room_id == room_id) {
                clients.emplace_back(ep, info);
            }
        }
        return clients;
    }

    std::optional<endpoint> get_room_endpoint_by_client_id(
        const endpoint& sender, uint32_t target_client_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sender_it = clients_.find(sender);
        if (sender_it == clients_.end()) {
            return std::nullopt;
        }

        const std::string& room_id = sender_it->second.room_id;
        for (const auto& [ep, info]: clients_) {
            if (info.room_id == room_id && info.client_id == target_client_id) {
                return ep;
            }
        }
        return std::nullopt;
    }

    // Remove timed out clients (returns list of timed out client IDs)
    std::vector<uint32_t> remove_timed_out_clients(time_point now, std::chrono::seconds timeout) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t>       timed_out_ids;

        for (auto it = clients_.begin(); it != clients_.end();) {
            if (now - it->second.last_alive > timeout) {
                timed_out_ids.push_back(it->second.client_id);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }

        return timed_out_ids;
    }

    // Access client info with lock (use with caution - callback must be fast)
    template <typename Func>
    void with_client(const endpoint& ep, Func&& func) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        if (it != clients_.end()) {
            func(it->second);
        }
    }

    // Access client info with lock (const version)
    template <typename Func>
    void with_client(const endpoint& ep, Func&& func) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = clients_.find(ep);
        if (it != clients_.end()) {
            func(it->second);
        }
    }

private:
    static void apply_security(ClientInfo& client,
                               const std::optional<ClientSecurityConfig>& security) {
        if (!security.has_value()) {
            client.has_authenticated_session = false;
            client.token_nonce_key.clear();
            return;
        }

        client.has_authenticated_session = true;
        client.token_nonce_key = security->token_nonce_key;
    }

    mutable std::mutex                                      mutex_;
    std::unordered_map<endpoint, ClientInfo, endpoint_hash> clients_;
    uint32_t                                                next_client_id_;
};
