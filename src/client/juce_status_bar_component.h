#pragma once

#include "client_app_facade.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

class JuceStatusBarComponent final : public juce::Component {
public:
    JuceStatusBarComponent();

    void refresh(const ClientAppFacade& client,
                 const JuceClientStartupOptions& startup_options,
                 const juce::String& connection_status);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String server_text_;
    juce::String room_text_;
    juce::String state_text_;
    juce::String users_text_;
    juce::String rtt_text_;
    juce::String rx_text_;
    juce::String tx_text_;
    juce::String audio_text_;
    juce::String device_text_;
    bool joined_ = false;
    bool audio_running_ = false;
};
