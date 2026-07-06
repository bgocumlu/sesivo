#include "juce_root_component.h"

#include "juce_mixer_component.h"

#include <utility>

JuceRootComponent::JuceRootComponent(ClientAppFacade& client,
                                     JuceClientStartupOptions startup_options)
    : client_(client),
      startup_options_(std::move(startup_options)) {
    if (startup_options_.auto_connect) {
        show_mixer(startup_options_);
    } else {
        show_browser();
    }
}

void JuceRootComponent::resized() {
    if (active_component_ != nullptr) {
        active_component_->setBounds(getLocalBounds());
    }
}

void JuceRootComponent::show_browser() {
    auto browser = std::make_unique<JuceRoomBrowserComponent>(
        client_, startup_options_,
        [this](JuceRoomBrowserComponent::JoinLaunch launch) {
            auto options = startup_options_;
            options.server_address = launch.server_address;
            options.server_port = launch.server_port;
            options.room_admin_token = launch.room_admin_token;
            options.auto_connect = false;
            show_mixer(std::move(options));
        });
    active_component_ = std::move(browser);
    addAndMakeVisible(*active_component_);
    resized();
}

void JuceRootComponent::show_mixer(JuceClientStartupOptions options) {
    active_component_ = std::make_unique<JuceMixerComponent>(
        client_, std::move(options),
        [this]() {
            show_browser();
        });
    addAndMakeVisible(*active_component_);
    resized();
}
