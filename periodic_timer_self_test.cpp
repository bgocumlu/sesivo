#include "periodic_timer.h"

#include <asio.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <thread>

using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct TimerRunStats {
    int total_callbacks = 0;
    int immediate_callbacks = 0;
};

TimerRunStats callbacks_after_delayed_io_thread() {
    asio::io_context io_context;
    asio::steady_timer stop_timer(io_context);
    TimerRunStats result;
    std::chrono::steady_clock::time_point first_callback_time{};

    PeriodicTimer timer(io_context, 10ms, [&]() {
        const auto now = std::chrono::steady_clock::now();
        ++result.total_callbacks;
        if (result.total_callbacks == 1) {
            first_callback_time = now;
            stop_timer.expires_after(25ms);
            stop_timer.async_wait([&](std::error_code) {
                io_context.stop();
            });
            ++result.immediate_callbacks;
        } else if (now - first_callback_time <= 2ms) {
            ++result.immediate_callbacks;
        }
        if (result.total_callbacks >= 20) {
            io_context.stop();
        }
    });

    asio::post(io_context, []() {
        std::this_thread::sleep_for(75ms);
    });

    io_context.run();
    return result;
}

}  // namespace

int main() {
    const TimerRunStats current = callbacks_after_delayed_io_thread();
    require(current.immediate_callbacks == 1,
            "PeriodicTimer replayed missed ticks after a delayed IO thread");

    std::cout << "periodic timer self-test passed: current burst callbacks="
              << current.immediate_callbacks
              << " current total=" << current.total_callbacks << '\n';
    return 0;
}
