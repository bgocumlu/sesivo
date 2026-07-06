#pragma once

#include <filesystem>
#include <string>

std::filesystem::path default_client_config_dir();
std::filesystem::path client_config_path(const std::string& config_dir);
