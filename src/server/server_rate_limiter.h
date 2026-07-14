#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <list>
#include <unordered_map>

#include <asio/ip/udp.hpp>

#include "endpoint_hash.h"

namespace server_rate_limiter {

class TokenBucket {
public:
    bool allow(std::chrono::steady_clock::time_point now, double rate_per_second,
               double burst_tokens, double cost = 1.0) {
        rate_per_second = std::max(0.0, rate_per_second);
        burst_tokens = std::max(0.0, burst_tokens);
        cost = std::max(0.0, cost);

        if (last_refill_.time_since_epoch().count() == 0) {
            tokens_ = burst_tokens;
            last_refill_ = now;
        } else if (now > last_refill_) {
            const double elapsed_seconds =
                std::chrono::duration<double>(now - last_refill_).count();
            tokens_ = std::min(burst_tokens, tokens_ + elapsed_seconds * rate_per_second);
            last_refill_ = now;
        }

        if (tokens_ + 1e-9 < cost) {
            return false;
        }
        tokens_ -= cost;
        return true;
    }

private:
    double tokens_ = 0.0;
    std::chrono::steady_clock::time_point last_refill_{};
};

class ProtocolRateLimiter {
public:
    using endpoint = asio::ip::udp::endpoint;
    using time_point = std::chrono::steady_clock::time_point;
    static constexpr std::size_t MAX_TRACKED_ENDPOINTS = 4096;

    bool allow_unknown(const endpoint& ep, time_point now) {
        // Reject source-cardinality floods before allocating endpoint state.
        if (!unknown_global_.allow(now, 2000.0, 4000.0)) {
            return false;
        }
        return allow(ep, now, &EndpointBuckets::unknown, 20.0, 20.0);
    }

    bool allow_strict(const endpoint& ep, time_point now) {
        return allow(ep, now, &EndpointBuckets::strict, 20.0, 20.0);
    }

    bool allow_control(const endpoint& ep, time_point now) {
        return allow(ep, now, &EndpointBuckets::control, 120.0, 240.0);
    }

    bool allow_status(const endpoint& ep, time_point now) {
        return allow(ep, now, &EndpointBuckets::status, 0.8, 8.0);
    }

    bool allow_room_control(const endpoint& ep, time_point now) {
        return allow(ep, now, &EndpointBuckets::room_control, 2.0, 8.0);
    }

    bool allow_chat_send(const endpoint& ep, time_point now) {
        return allow(ep, now, &EndpointBuckets::chat_send, 5.0, 10.0);
    }

    bool allow_authenticated_secure_control(const endpoint& ep, time_point now) {
        if (!allow(ep, now, &EndpointBuckets::secure_control, 64.0, 128.0)) {
            return false;
        }
        return secure_control_global_.allow(now, 2048.0, 4096.0);
    }

    bool allow_authenticated_audio(const endpoint& ep, uint32_t sample_rate,
                                   uint16_t frame_count, time_point now) {
        if (sample_rate == 0 || frame_count == 0) {
            return allow_strict(ep, now);
        }

        const double packet_rate =
            std::ceil(static_cast<double>(sample_rate) / static_cast<double>(frame_count));
        const double allowed_rate = std::max(600.0, packet_rate * 2.0);
        return allow(ep, now, &EndpointBuckets::audio, allowed_rate, allowed_rate * 2.0);
    }

    void erase(const endpoint& ep) {
        const auto it = endpoints_.find(ep);
        if (it == endpoints_.end()) {
            return;
        }
        recency_.erase(it->second.recency);
        endpoints_.erase(it);
    }

    void expire(time_point now, std::chrono::steady_clock::duration ttl) {
        while (!recency_.empty()) {
            const auto it = endpoints_.find(recency_.front());
            if (it == endpoints_.end()) {
                recency_.pop_front();
                continue;
            }
            if (now - it->second.last_seen <= ttl) {
                break;
            }
            endpoints_.erase(it);
            recency_.pop_front();
        }
    }

    std::size_t tracked_endpoint_count() const {
        return endpoints_.size();
    }

private:
    struct EndpointBuckets {
        TokenBucket unknown;
        TokenBucket strict;
        TokenBucket control;
        TokenBucket status;
        TokenBucket room_control;
        TokenBucket chat_send;
        TokenBucket secure_control;
        TokenBucket audio;
        time_point last_seen{};
        std::list<endpoint>::iterator recency;
    };

    using BucketMember = TokenBucket EndpointBuckets::*;

    bool allow(const endpoint& ep, time_point now, BucketMember bucket,
               double rate_per_second, double burst_tokens) {
        auto it = endpoints_.find(ep);
        if (it == endpoints_.end()) {
            if (endpoints_.size() >= MAX_TRACKED_ENDPOINTS) {
                const auto oldest = endpoints_.find(recency_.front());
                if (oldest != endpoints_.end()) {
                    endpoints_.erase(oldest);
                }
                recency_.pop_front();
            }
            recency_.push_back(ep);
            auto [inserted, created] = endpoints_.try_emplace(ep);
            (void)created;
            it = inserted;
            it->second.recency = std::prev(recency_.end());
        } else {
            recency_.splice(recency_.end(), recency_, it->second.recency);
            it->second.recency = std::prev(recency_.end());
        }
        it->second.last_seen = now;
        return (it->second.*bucket).allow(now, rate_per_second, burst_tokens);
    }

    TokenBucket unknown_global_;
    TokenBucket secure_control_global_;
    std::list<endpoint> recency_;
    std::unordered_map<endpoint, EndpointBuckets, endpoint_hash> endpoints_;
};

}  // namespace server_rate_limiter
