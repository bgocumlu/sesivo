#pragma once

#include "juce_theme.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

class ClientAppFacade;
class JuceRootComponent;

class JuceMainWindow final : public juce::DocumentWindow {
public:
    JuceMainWindow(const juce::String& title, ClientAppFacade& client,
                   JuceClientStartupOptions startup_options,
                   std::function<void()> close_callback);
    ~JuceMainWindow() override;

    void closeButtonPressed() override;
    void open_invite(std::string invite_text);

private:
    juce_theme::LookAndFeel look_and_feel_;
    JuceRootComponent* root_component_ = nullptr;
    std::function<void()> close_callback_;
};
