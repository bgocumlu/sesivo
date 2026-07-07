#include "juce_root_component.h"

#include "juce_mixer_component.h"
#include "juce_theme.h"

#include <utility>

JuceRootComponent::JuceRootComponent(ClientAppFacade& client,
                                     JuceClientStartupOptions startup_options)
    : client_(client),
      startup_options_(std::move(startup_options)) {
    setOpaque(true);
    if (startup_options_.auto_connect) {
        show_mixer(startup_options_);
    } else {
        show_browser();
    }
}

void JuceRootComponent::paint(juce::Graphics& g) {
    g.fillAll(juce_theme::colour::background());
}

void JuceRootComponent::resized() {
    if (active_component_ != nullptr) {
        active_component_->setBounds(getLocalBounds());
    }
}

void JuceRootComponent::open_invite(std::string invite_text) {
    client_.stop_connection();
    startup_options_.room_admin_token.clear();
    startup_options_.room_instance_id.clear();
    startup_options_.access_epoch = 0;
    startup_options_.media_secret.clear();
    startup_options_.access_mode = ROOM_ACCESS_OPEN;
    startup_options_.auto_connect = false;
    auto& browser = show_browser();
    browser.open_invite(std::move(invite_text));
}

JuceRoomBrowserComponent& JuceRootComponent::show_browser() {
    if (auto* browser =
            dynamic_cast<JuceRoomBrowserComponent*>(active_component_.get())) {
        return *browser;
    }

    auto browser = std::make_unique<JuceRoomBrowserComponent>(
        client_, startup_options_,
        [this](JuceRoomBrowserComponent::JoinLaunch launch) {
            auto options = startup_options_;
            options.server_address = launch.server_address;
            options.server_port = launch.server_port;
            options.room_admin_token = launch.room_admin_token;
            options.room_instance_id = launch.room_instance_id;
            options.access_epoch = launch.access_epoch;
            options.media_secret = launch.media_secret;
            options.access_mode = launch.access_mode;
            options.auto_connect = false;
            show_mixer(std::move(options));
        });
    auto* browser_ptr = browser.get();
    active_component_ = std::move(browser);
    addAndMakeVisible(*active_component_);
    resized();
    return *browser_ptr;
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
