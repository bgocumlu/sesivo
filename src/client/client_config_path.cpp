#include "client_config_path.h"

#include <juce_core/juce_core.h>

std::filesystem::path default_client_config_dir() {
    auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if defined(__APPLE__)
    folder = folder.getChildFile("Application Support");
#endif
    return std::filesystem::path(
        folder.getChildFile("sesivo").getFullPathName().toStdString());
}

std::filesystem::path client_config_path(const std::string& config_dir) {
    const auto folder = config_dir.empty() ? default_client_config_dir()
                                           : std::filesystem::path(config_dir);
    return folder / "sesivo-client.json";
}
