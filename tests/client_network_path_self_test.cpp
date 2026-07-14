#include "client_network_path.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    require(client_network_path::PING_TIMEOUT_PROMOTE_REPLIES == 4,
            "four missed 250 ms pings must trigger path recovery within one second");
    require(client_network_path::UDP_REBIND_COOLDOWN <= std::chrono::seconds(3),
            "path recovery cooldown must permit prompt retry");
    require(client_network_path::missing_replies_for_timeout(3, 0, false, 0) == 4,
            "missing-reply count must include the current ping");
    require(client_network_path::missing_replies_for_timeout(8, 0, true, 7) == 1,
            "a recent reply must reset the timeout window");
    std::cout << "client network path self-test passed\n";
    return 0;
}
