#pragma once

#include <juce_core/juce_core.h>

#include <filesystem>
#include <functional>
#include <string>

juce::var read_client_config_root(const std::filesystem::path& path);
juce::DynamicObject* client_config_root_object(juce::var& root);

// Enqueues a serialized background read-modify-write. The return value means
// the write was accepted, not that it has already reached disk.
bool enqueue_client_config_write(
    std::filesystem::path path,
    std::function<void(juce::DynamicObject&)> mutation,
    std::string log_message);
