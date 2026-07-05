#include "juce_app.h"

#include "client_app_facade.h"
#include "juce_main_window.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>

int run_juce_client_app(ClientAppFacade& client, const std::string& window_title,
                        JuceClientStartupOptions startup_options,
                        std::function<void()> close_callback) {
    juce::ScopedJuceInitialiser_GUI juce_runtime;

    JuceMainWindow window(
        juce::String(window_title), client, std::move(startup_options),
        [close_callback = std::move(close_callback)]() mutable {
            if (close_callback) {
                close_callback();
            }
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });

    juce::MessageManager::getInstance()->runDispatchLoop();
    window.setVisible(false);
    return 0;
}
