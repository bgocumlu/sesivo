#pragma once

#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class ClientAppFacade;

class JuceMainWindow final : public juce::DocumentWindow {
public:
    JuceMainWindow(const juce::String& title, ClientAppFacade& client,
                   JuceClientStartupAudioOptions startup_audio_options,
                   std::function<void()> close_callback);

    void closeButtonPressed() override;

private:
    std::function<void()> close_callback_;
};
