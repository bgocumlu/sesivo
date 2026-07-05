#pragma once

#include "juce_startup_options.h"

#include <functional>
#include <string>

class ClientAppFacade;

int run_juce_client_app(ClientAppFacade& client, const std::string& window_title,
                        JuceClientStartupOptions startup_options = {},
                        std::function<void()> close_callback = {});
