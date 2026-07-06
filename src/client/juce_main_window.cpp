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
    setUsingNativeTitleBar(false);
    setResizable(true, true);
    setResizeLimits(1024, 640, 2400, 1600);
    setContentOwned(new JuceRootComponent(client, std::move(startup_options)), true);
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
