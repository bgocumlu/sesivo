#include "client_config_path.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool ends_with(const std::filesystem::path& path, const std::filesystem::path& suffix) {
    const auto path_text = path.generic_string();
    const auto suffix_text = suffix.generic_string();
    return path_text.size() >= suffix_text.size() &&
           path_text.compare(path_text.size() - suffix_text.size(), suffix_text.size(),
                             suffix_text) == 0;
}

}  // namespace

int main() {
    const auto explicit_path = client_config_path("portable-config");
    if (explicit_path != std::filesystem::path("portable-config") / "sesivo-client.json") {
        std::cerr << "explicit config dir was not respected: " << explicit_path << "\n";
        return 1;
    }

    const auto default_dir = default_client_config_dir();
    const auto default_path = client_config_path("");
    if (default_dir.empty() || default_path != default_dir / "sesivo-client.json") {
        std::cerr << "default config path does not use default config dir\n";
        return 2;
    }

#if defined(_WIN32)
    if (!ends_with(default_path, std::filesystem::path("sesivo") / "sesivo-client.json")) {
        std::cerr << "windows config path should end under sesivo app data: "
                  << default_path << "\n";
        return 3;
    }
#elif defined(__APPLE__)
    if (!ends_with(default_path, std::filesystem::path("Library") /
                                     "Application Support" / "sesivo" /
                                     "sesivo-client.json")) {
        std::cerr << "macos config path should use Application Support: "
                  << default_path << "\n";
        return 3;
    }
#else
    if (!ends_with(default_path,
                   std::filesystem::path("sesivo") / "sesivo-client.json")) {
        std::cerr << "linux config path should end under sesivo config dir: "
                  << default_path << "\n";
        return 3;
    }
#endif

    return 0;
}
