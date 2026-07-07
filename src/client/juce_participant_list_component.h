#pragma once

#include "client_app_facade.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

class JuceParticipantListComponent final : public juce::Component, private juce::Timer {
public:
    explicit JuceParticipantListComponent(ClientAppFacade& client);
    ~JuceParticipantListComponent() override;

    void refresh();
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    class LocalParticipantRowComponent;
    class ParticipantRowComponent;

    ClientAppFacade& client_;
    juce::Label participant_header_label_;
    juce::Label empty_participants_label_;
    juce::Viewport participants_viewport_;
    juce::Component participants_content_;
    std::unique_ptr<LocalParticipantRowComponent> local_participant_row_;
    std::vector<std::unique_ptr<ParticipantRowComponent>> participant_rows_;
    size_t visible_participant_count_ = 0;
    bool local_participant_visible_ = false;
};
