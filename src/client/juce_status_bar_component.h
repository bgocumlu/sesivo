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
    void resized() override;

private:
    juce::Label status_label_;
    juce::Label transport_label_;
};
