#include "juce_main_window.h"

#include "client_app_facade.h"
#include "juce_mixer_component.h"

#include <utility>

JuceMainWindow::JuceMainWindow(const juce::String& title, ClientAppFacade& client,
                               JuceClientStartupOptions startup_options,
                               std::function<void()> close_callback)
    : DocumentWindow(title, juce::Colour(0xff111418),
                     juce::DocumentWindow::allButtons),
      close_callback_(std::move(close_callback)) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(900, 600, 2400, 1600);
    setContentOwned(new JuceMixerComponent(client, std::move(startup_options)), true);
    centreWithSize(1080, 720);
    setVisible(true);
}

void JuceMainWindow::closeButtonPressed() {
    if (close_callback_) {
        close_callback_();
    } else {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
}
