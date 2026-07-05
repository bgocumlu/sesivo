#pragma once

#include "client_app_facade.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

class JuceParticipantListComponent final : public juce::Component {
public:
    explicit JuceParticipantListComponent(ClientAppFacade& client);
    ~JuceParticipantListComponent() override;

    void refresh();
    void resized() override;

private:
    class ParticipantRowComponent;

    ClientAppFacade& client_;
    juce::Label participant_header_label_;
    juce::Label empty_participants_label_;
    juce::Viewport participants_viewport_;
    juce::Component participants_content_;
    std::vector<std::unique_ptr<ParticipantRowComponent>> participant_rows_;
    size_t visible_participant_count_ = 0;
};
