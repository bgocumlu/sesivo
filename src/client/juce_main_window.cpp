#include "juce_main_window.h"

#include "client_app_facade.h"
#include "juce_root_component.h"

#include <utility>

JuceMainWindow::JuceMainWindow(const juce::String& title, ClientAppFacade& client,
                               JuceClientStartupOptions startup_options,
                               std::function<void()> close_callback)
    : DocumentWindow(title, juce_theme::colour::background(),
                     juce::DocumentWindow::allButtons),
      close_callback_(std::move(close_callback)) {
    juce::LookAndFeel::setDefaultLookAndFeel(&look_and_feel_);
    setLookAndFeel(&look_and_feel_);
    setColour(juce::ResizableWindow::backgroundColourId,
              juce_theme::colour::background());
    setUsingNativeTitleBar(false);
    setResizable(true, true);
    setResizeLimits(1024, 640, 2400, 1600);
    root_component_ = new JuceRootComponent(client, std::move(startup_options));
    setContentOwned(root_component_, true);
    centreWithSize(1180, 720);
    setVisible(true);
}

JuceMainWindow::~JuceMainWindow() {
    setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

void JuceMainWindow::closeButtonPressed() {
    if (close_callback_) {
        close_callback_();
    } else {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
}

void JuceMainWindow::open_invite(std::string invite_text) {
    if (root_component_ != nullptr) {
        root_component_->open_invite(std::move(invite_text));
    }
    toFront(true);
}
