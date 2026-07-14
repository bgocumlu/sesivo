#include "server_rate_limiter.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

#include <asio/ip/address.hpp>

using asio::ip::udp;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

udp::endpoint endpoint(uint16_t port) {
    return udp::endpoint(asio::ip::make_address("127.0.0.1"), port);
}

udp::endpoint unique_endpoint(std::size_t index) {
    const auto address = asio::ip::address_v4(static_cast<uint32_t>(index + 1));
    return udp::endpoint(address, static_cast<uint16_t>(10000 + (index % 50000)));
}

void test_authenticated_low_latency_audio_is_not_throttled() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto ep = endpoint(10001);
    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 800; ++i) {
        const auto now = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(
                                         static_cast<double>(i) / 400.0));
        require(limiter.allow_authenticated_audio(ep, 48000, 120, now),
                "400 pps authenticated 120-frame audio should pass");
    }
}

void test_authenticated_flood_is_throttled() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto ep = endpoint(10002);
    const auto now = std::chrono::steady_clock::now();

    int allowed = 0;
    for (int i = 0; i < 2000; ++i) {
        if (limiter.allow_authenticated_audio(ep, 48000, 120, now)) {
            ++allowed;
        }
    }

    require(allowed >= 1500, "audio burst should allow a generous startup burst");
    require(allowed < 2000, "same-timestamp audio flood should be throttled");
}

void test_unknown_strict_burst() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto ep = endpoint(10003);
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 20; ++i) {
        require(limiter.allow_unknown(ep, now), "unknown burst packet should pass");
    }
    require(!limiter.allow_unknown(ep, now), "unknown packet 21 should be throttled");
}

void test_control_burst() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto ep = endpoint(10004);
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 240; ++i) {
        require(limiter.allow_control(ep, now), "control burst packet should pass");
    }
    require(!limiter.allow_control(ep, now), "control packet 241 should be throttled");
}

void test_authenticated_secure_control_burst() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto ep = endpoint(10005);
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 128; ++i) {
        require(limiter.allow_authenticated_secure_control(ep, now),
                "authenticated secure-control burst should pass within its budget");
    }
    require(!limiter.allow_authenticated_secure_control(ep, now),
            "authenticated secure-control flood should be throttled before forwarding");
    require(limiter.allow_authenticated_secure_control(endpoint(10006), now),
            "endpoint throttling must not drain global control capacity");

    server_rate_limiter::ProtocolRateLimiter global_limiter;
    int global_allowed = 0;
    for (std::size_t i = 0; i < 5000; ++i) {
        if (global_limiter.allow_authenticated_secure_control(
                unique_endpoint(i), now)) {
            ++global_allowed;
        }
    }
    require(global_allowed == 4096,
            "authenticated secure-control aggregate burst must have a global ceiling");
}

void test_endpoint_state_has_fixed_cardinality() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto now = std::chrono::steady_clock::now();
    const auto attempts = server_rate_limiter::ProtocolRateLimiter::MAX_TRACKED_ENDPOINTS * 256;
    for (std::size_t i = 0; i < attempts; ++i) {
        (void)limiter.allow_strict(unique_endpoint(i), now);
        require(limiter.tracked_endpoint_count() <=
                    server_rate_limiter::ProtocolRateLimiter::MAX_TRACKED_ENDPOINTS,
                "endpoint limiter state exceeded its fixed cardinality");
    }
    require(limiter.tracked_endpoint_count() ==
                server_rate_limiter::ProtocolRateLimiter::MAX_TRACKED_ENDPOINTS,
            "endpoint limiter should retain exactly its fixed capacity under attack");
}

void test_endpoint_state_expires_and_erases() {
    server_rate_limiter::ProtocolRateLimiter limiter;
    const auto start = std::chrono::steady_clock::now();
    const auto first = endpoint(11001);
    const auto second = endpoint(11002);
    require(limiter.allow_control(first, start), "first endpoint should be admitted");
    require(limiter.allow_control(second, start + 1s), "second endpoint should be admitted");
    limiter.erase(first);
    require(limiter.tracked_endpoint_count() == 1, "explicit erase must remove all endpoint buckets");
    limiter.expire(start + 32s, 30s);
    require(limiter.tracked_endpoint_count() == 0, "expired endpoint buckets must be reclaimed");
}

}  // namespace

int main() {
    test_authenticated_low_latency_audio_is_not_throttled();
    test_authenticated_flood_is_throttled();
    test_unknown_strict_burst();
    test_control_burst();
    test_authenticated_secure_control_burst();
    test_endpoint_state_has_fixed_cardinality();
    test_endpoint_state_expires_and_erases();

    std::cout << "server rate limiter self-test passed\n";
    return 0;
}
