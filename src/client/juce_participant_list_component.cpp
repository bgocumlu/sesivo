#include "juce_participant_list_component.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int PAD = 10;
constexpr int PARTICIPANT_ROW_HEIGHT = 78;

juce::String participant_name(const ParticipantInfo& participant) {
    if (!participant.display_name.empty()) {
        return participant.display_name;
    }
    if (!participant.profile_id.empty()) {
        return participant.profile_id;
    }
    return "User " + juce::String(participant.id);
}

void configure_rotary_slider(juce::Slider& slider, double min_value, double max_value,
                             double interval) {
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
    slider.setRange(min_value, max_value, interval);
}

void configure_linear_slider(juce::Slider& slider, double min_value, double max_value,
                             double interval, juce::String suffix = {}) {
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 74, 20);
    slider.setRange(min_value, max_value, interval);
    slider.setTextValueSuffix(suffix);
}

void add_all(juce::Component& parent, std::initializer_list<juce::Component*> children) {
    for (auto* child: children) {
        parent.addAndMakeVisible(child);
    }
}
}  // namespace

class JuceParticipantListComponent::ParticipantRowComponent final
    : public juce::Component {
public:
    explicit ParticipantRowComponent(ClientAppFacade& client) : client_(client) {
        name_label_.setFont(juce::Font(juce::FontOptions(15.0F, juce::Font::bold)));
        stats_label_.setFont(juce::Font(juce::FontOptions(12.0F)));
        level_label_.setJustificationType(juce::Justification::centred);
        mute_button_.setClickingTogglesState(true);
        mute_button_.setButtonText("Mute");
        auto_jitter_toggle_.setButtonText("Auto jitter");
        jitter_ms_editor_.setInputRestrictions(4, "0123456789");
        reset_jitter_button_.setButtonText("Reset");
        configure_linear_slider(gain_slider_, 0.0, 2.0, 0.01, "x");
        configure_rotary_slider(pan_slider_, 0.0, 1.0, 0.01);

        add_all(*this, {&name_label_, &stats_label_, &level_label_, &mute_button_,
                        &gain_slider_, &pan_slider_, &auto_jitter_toggle_,
                        &jitter_ms_editor_, &reset_jitter_button_});

        mute_button_.onClick = [this]() {
            if (!updating_) {
                client_.set_participant_muted(info_.id, mute_button_.getToggleState());
            }
        };
        gain_slider_.onValueChange = [this]() {
            if (!updating_) {
                client_.set_participant_gain(info_.id,
                                             static_cast<float>(gain_slider_.getValue()));
            }
        };
        pan_slider_.onValueChange = [this]() {
            if (!updating_) {
                client_.set_participant_pan(info_.id,
                                            static_cast<float>(pan_slider_.getValue()));
            }
        };
        auto_jitter_toggle_.onClick = [this]() {
            if (!updating_) {
                client_.set_participant_opus_auto_jitter(
                    info_.id, auto_jitter_toggle_.getToggleState());
            }
        };
        jitter_ms_editor_.onReturnKey = [this]() { commit_jitter_ms(); };
        jitter_ms_editor_.onFocusLost = [this]() { commit_jitter_ms(); };
        reset_jitter_button_.onClick = [this]() {
            client_.reset_participant_opus_jitter_buffer_packets(info_.id);
        };
    }

    void set_info(const ParticipantInfo& info) {
        info_ = info;
        updating_ = true;

        name_label_.setText(participant_name(info), juce::dontSendNotification);
        level_label_.setText(juce::String(static_cast<int>(info.audio_level * 100.0F)) + "%",
                             juce::dontSendNotification);
        mute_button_.setToggleState(info.is_muted, juce::dontSendNotification);
        gain_slider_.setValue(info.gain, juce::dontSendNotification);
        pan_slider_.setValue(info.pan, juce::dontSendNotification);
        auto_jitter_toggle_.setToggleState(info.opus_jitter_auto_enabled,
                                           juce::dontSendNotification);
        const auto jitter_ms = static_cast<int>(std::lround(
            static_cast<double>(info.jitter_buffer_floor_packets) *
            client_.get_opus_network_packet_ms()));
        jitter_ms_editor_.setText(juce::String(jitter_ms), false);

        stats_label_.setText(
            "Queue " + juce::String(info.queue_size) + " avg " +
                juce::String(info.queue_size_avg) + " max " +
                juce::String(info.queue_size_max) + " | PLC " +
                juce::String(static_cast<int>(info.plc_count)) + " | E2E " +
                juce::String(info.capture_to_playout_latency_avg_ms, 1) + " ms",
            juce::dontSendNotification);

        updating_ = false;
    }

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat().reduced(2.0F);
        g.setColour(juce::Colour(0xff1d2329));
        g.fillRoundedRectangle(area, 6.0F);
        g.setColour(info_.is_speaking ? juce::Colour(0xff4bb36b)
                                      : juce::Colour(0xff343d45));
        g.drawRoundedRectangle(area, 6.0F, 1.0F);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(PAD, 8);
        auto left = area.removeFromLeft(190);
        name_label_.setBounds(left.removeFromTop(26));
        stats_label_.setBounds(left);

        mute_button_.setBounds(area.removeFromLeft(72).reduced(4, 12));
        level_label_.setBounds(area.removeFromLeft(58).reduced(4, 16));
        gain_slider_.setBounds(area.removeFromLeft(148).reduced(4, 2));
        pan_slider_.setBounds(area.removeFromLeft(92).reduced(4, 0));
        auto_jitter_toggle_.setBounds(area.removeFromLeft(105).reduced(4, 18));
        jitter_ms_editor_.setBounds(area.removeFromLeft(58).reduced(4, 20));
        reset_jitter_button_.setBounds(area.removeFromLeft(76).reduced(4, 18));
    }

private:
    void commit_jitter_ms() {
        if (updating_) {
            return;
        }
        const int value = jitter_ms_editor_.getText().getIntValue();
        client_.set_participant_opus_jitter_buffer_ms(info_.id, std::max(value, 0));
    }

    ClientAppFacade& client_;
    ParticipantInfo info_{};
    bool updating_ = false;

    juce::Label name_label_;
    juce::Label stats_label_;
    juce::Label level_label_;
    juce::TextButton mute_button_;
    juce::Slider gain_slider_;
    juce::Slider pan_slider_;
    juce::ToggleButton auto_jitter_toggle_;
    juce::TextEditor jitter_ms_editor_;
    juce::TextButton reset_jitter_button_;
};

JuceParticipantListComponent::JuceParticipantListComponent(ClientAppFacade& client)
    : client_(client) {
    participant_header_label_.setText("Participants", juce::dontSendNotification);
    participant_header_label_.setFont(
        juce::Font(juce::FontOptions(16.0F, juce::Font::bold)));
    empty_participants_label_.setText("No remote participants", juce::dontSendNotification);
    empty_participants_label_.setJustificationType(juce::Justification::centred);
    participants_viewport_.setViewedComponent(&participants_content_, false);

    addAndMakeVisible(participant_header_label_);
    addAndMakeVisible(empty_participants_label_);
    addAndMakeVisible(participants_viewport_);
}

JuceParticipantListComponent::~JuceParticipantListComponent() = default;

void JuceParticipantListComponent::refresh() {
    const auto participants = client_.get_participant_info();
    visible_participant_count_ = participants.size();
    while (participant_rows_.size() < participants.size()) {
        auto row = std::make_unique<ParticipantRowComponent>(client_);
        participants_content_.addAndMakeVisible(row.get());
        participant_rows_.push_back(std::move(row));
    }

    for (size_t index = 0; index < participant_rows_.size(); ++index) {
        const bool visible = index < participants.size();
        participant_rows_[index]->setVisible(visible);
        if (visible) {
            participant_rows_[index]->set_info(participants[index]);
        }
    }

    empty_participants_label_.setVisible(participants.empty());
    participants_viewport_.setVisible(!participants.empty());
    resized();
}

void JuceParticipantListComponent::resized() {
    auto area = getLocalBounds();
    participant_header_label_.setBounds(area.removeFromTop(26));
    empty_participants_label_.setBounds(area);
    participants_viewport_.setBounds(area);

    const int content_width =
        std::max(area.getWidth() - participants_viewport_.getScrollBarThickness(), 760);
    participants_content_.setSize(content_width,
                                  static_cast<int>(visible_participant_count_) *
                                      PARTICIPANT_ROW_HEIGHT);
    for (size_t index = 0; index < participant_rows_.size(); ++index) {
        participant_rows_[index]->setBounds(
            0, static_cast<int>(index) * PARTICIPANT_ROW_HEIGHT, content_width,
            PARTICIPANT_ROW_HEIGHT);
    }
}
