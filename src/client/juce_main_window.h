#pragma once

#include "juce_theme.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class ClientAppFacade;

class JuceMainWindow final : public juce::DocumentWindow {
public:
    JuceMainWindow(const juce::String& title, ClientAppFacade& client,
                   JuceClientStartupOptions startup_options,
                   std::function<void()> close_callback);
    ~JuceMainWindow() override;

    void closeButtonPressed() override;

private:
    juce_theme::LookAndFeel look_and_feel_;
    std::function<void()> close_callback_;
};
