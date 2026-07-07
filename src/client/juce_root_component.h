#pragma once

#include "client_app_facade.h"
#include "juce_room_browser_component.h"
#include "juce_startup_options.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>

class JuceRootComponent final : public juce::Component {
public:
    JuceRootComponent(ClientAppFacade& client,
                      JuceClientStartupOptions startup_options);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void open_invite(std::string invite_text);

private:
    JuceRoomBrowserComponent& show_browser();
    void show_mixer(JuceClientStartupOptions options);

    ClientAppFacade& client_;
    JuceClientStartupOptions startup_options_;
    std::unique_ptr<juce::Component> active_component_;
};
