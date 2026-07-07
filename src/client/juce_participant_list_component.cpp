#include "juce_participant_list_component.h"

#include "juce_theme.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int PAD = 10;
constexpr int PARTICIPANT_ROW_HEIGHT = 64;
constexpr int NAME_WIDTH = 200;
constexpr int LEVEL_WIDTH = 220;
constexpr int BUTTONS_WIDTH = 86;
constexpr int GAIN_WIDTH = 170;
constexpr int PAN_WIDTH = 94;
constexpr int QUEUE_WIDTH = 118;
constexpr int STATUS_WIDTH = 112;
constexpr int JITTER_WIDTH = 76;
constexpr int MS_WIDTH = 64;
constexpr int RESET_WIDTH = 72;
constexpr float METER_LEVEL_RELEASE = 0.82F;
constexpr float METER_PEAK_RELEASE = 0.78F;
constexpr int CONTENT_MIN_WIDTH =
    PAD * 2 + NAME_WIDTH + 8 + LEVEL_WIDTH + BUTTONS_WIDTH + GAIN_WIDTH +
    PAN_WIDTH + QUEUE_WIDTH + STATUS_WIDTH + JITTER_WIDTH + MS_WIDTH + RESET_WIDTH;

juce::String participant_name(const ParticipantInfo& participant) {
    if (!participant.display_name.empty()) {
        return participant.display_name;
    }
    if (!participant.profile_id.empty()) {
        return participant.profile_id;
    }
    return "User " + juce::String(participant.id);
}

juce::String profile_subtitle(const ParticipantInfo& participant) {
    juce::String suffix =
        "E2E " + juce::String(participant.capture_to_playout_latency_avg_ms, 1) +
        " ms  PLC " + juce::String(static_cast<int>(participant.plc_count));
    if (!participant.profile_id.empty() &&
        participant.profile_id != participant.display_name) {
        return juce::String(participant.profile_id) + "  " + suffix;
    }
    return suffix;
}

juce::String pan_display_text(double value) {
    const double offset = value - 0.5;
    if (std::abs(offset) < 0.02) {
        return "C";
    }
    const int amount = static_cast<int>(std::lround(std::abs(offset) * 100.0));
    return juce::String(offset < 0.0 ? "L " : "R ") + juce::String(amount);
}

void draw_meter_bars(juce::Graphics& g, juce::Rectangle<int> bounds, float amount,
                     juce::Colour active_colour) {
    bounds = bounds.reduced(0, 1);
    constexpr int bar_width = 3;
    constexpr int gap = 2;
    const int bars = std::max(1, bounds.getWidth() / (bar_width + gap));
    const int active_bars =
        juce::jlimit(0, bars, static_cast<int>(std::ceil(amount * bars)));
    for (int index = 0; index < bars; ++index) {
        auto bar = juce::Rectangle<int>{bounds.getX() + index * (bar_width + gap),
                                        bounds.getY(), bar_width,
                                        bounds.getHeight()};
        g.setColour(index < active_bars ? active_colour
                                        : juce_theme::colour::border_soft());
        g.fillRoundedRectangle(bar.toFloat(), 1.0F);
    }
}

void draw_level_meter(juce::Graphics& g, juce::Rectangle<int> bounds, float amount,
                      float peak_amount, juce::Colour active_colour) {
    auto meter = bounds.withSizeKeepingCentre(bounds.getWidth(), 16);
    auto bar_bounds = meter.reduced(0, 1);
    constexpr int bar_width = 3;
    constexpr int gap = 2;
    const int bars = std::max(1, bar_bounds.getWidth() / (bar_width + gap));
    amount = juce::jlimit(0.0F, 1.0F, amount);
    peak_amount = juce::jlimit(0.0F, 1.0F, peak_amount);
    const int active_bars =
        juce::jlimit(0, bars, static_cast<int>(std::ceil(amount * bars)));
    const int peak_bars = juce::jlimit(
        active_bars, bars, static_cast<int>(std::ceil(peak_amount * bars)));

    for (int index = 0; index < bars; ++index) {
        auto bar = juce::Rectangle<int>{bar_bounds.getX() + index * (bar_width + gap),
                                        bar_bounds.getY(), bar_width,
                                        bar_bounds.getHeight()};
        if (index < active_bars) {
            g.setColour(active_colour);
        } else if (index < peak_bars) {
            g.setColour(active_colour.withAlpha(0.18F));
        } else {
            g.setColour(juce_theme::colour::border_soft());
        }
        g.fillRoundedRectangle(bar.toFloat(), 1.0F);
    }
}

void draw_row_separators(juce::Graphics& g, int height) {
    int x = PAD + NAME_WIDTH + 4;
    const int y1 = 8;
    const int y2 = height - 8;
    const auto draw = [&](int next_width) {
        g.drawVerticalLine(x, static_cast<float>(y1), static_cast<float>(y2));
        x += next_width;
    };

    g.setColour(juce_theme::colour::border_soft().withAlpha(0.72F));
    draw(4 + LEVEL_WIDTH);
    draw(BUTTONS_WIDTH);
    draw(GAIN_WIDTH);
    draw(PAN_WIDTH);
    draw(QUEUE_WIDTH);
    draw(STATUS_WIDTH);
    draw(JITTER_WIDTH);
    draw(MS_WIDTH);
    draw(RESET_WIDTH);
}

void draw_column_label(juce::Graphics& g, juce::Rectangle<int> bounds,
                       const juce::String& label) {
    g.setFont(juce_theme::font(12.0F));
    g.setColour(juce_theme::colour::text_faint());
    g.drawFittedText(label, bounds, juce::Justification::centredLeft, 1);
}

enum class RowIcon {
    user,
    speaker,
};

juce::String hex_byte(int value) {
    auto hex = juce::String::toHexString(value);
    return hex.length() == 1 ? "0" + hex : hex;
}

juce::String svg_colour(juce::Colour colour) {
    return "#" + hex_byte(colour.getRed()) + hex_byte(colour.getGreen()) +
           hex_byte(colour.getBlue());
}

const char* icon_svg_body(RowIcon icon) {
    switch (icon) {
    case RowIcon::user:
        return R"(<path d="M16 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="10" cy="7" r="4"/>)";
    case RowIcon::speaker:
        return R"(<path d="M11 5 6 9H2v6h4l5 4z"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/>)";
    }
    return "";
}

void draw_icon(juce::Graphics& g, juce::Rectangle<int> bounds, RowIcon icon,
               juce::Colour colour) {
    const auto svg =
        juce::String(R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke=")") +
        svg_colour(colour) +
        R"(" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)" +
        icon_svg_body(icon) + "</svg>";

    if (auto xml = juce::parseXML(svg)) {
        if (auto drawable = juce::Drawable::createFromSVG(*xml)) {
            drawable->drawWithin(g, bounds.toFloat().reduced(2.0F),
                                 juce::RectanglePlacement::centred, 1.0F);
        }
    }
}

void configure_rotary_slider(juce::Slider& slider, double min_value, double max_value,
                             double interval) {
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
    slider.setRange(min_value, max_value, interval);
    slider.textFromValueFunction = [](double value) {
        return pan_display_text(value);
    };
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

class JuceParticipantListComponent::LocalParticipantRowComponent final
    : public juce::Component {
public:
    explicit LocalParticipantRowComponent(ClientAppFacade& client) : client_(client) {
        juce_theme::style_label(name_label_, juce_theme::colour::text(), 15.0F, true);
        juce_theme::style_label(stats_label_, juce_theme::colour::text_dim(), 12.0F);
        juce_theme::style_label(queue_label_, juce_theme::colour::text_faint(), 12.0F);
        juce_theme::style_label(status_label_, juce_theme::colour::text_dim(), 12.0F);

        name_label_.setText("You", juce::dontSendNotification);
        stats_label_.setText("Local", juce::dontSendNotification);
        queue_label_.setText("-", juce::dontSendNotification);
        queue_label_.setJustificationType(juce::Justification::centred);
        juce_theme::style_label(pan_label_, juce_theme::colour::text_faint(), 12.0F);
        pan_label_.setText("-", juce::dontSendNotification);
        pan_label_.setJustificationType(juce::Justification::centred);

        mute_button_.setClickingTogglesState(true);
        mute_button_.setButtonText("M");
        monitor_button_.setClickingTogglesState(true);
        monitor_button_.setButtonText("S");
        configure_linear_slider(input_gain_slider_, 0.0, 2.0, 0.01, "x");

        add_all(*this, {&name_label_, &stats_label_, &input_gain_slider_, &pan_label_,
                        &queue_label_, &status_label_, &monitor_button_,
                        &mute_button_});

        mute_button_.onClick = [this]() {
            if (!updating_) {
                client_.set_mic_muted(mute_button_.getToggleState());
            }
        };
        monitor_button_.onClick = [this]() {
            if (!updating_) {
                client_.set_self_monitor_enabled(monitor_button_.getToggleState());
            }
        };
        input_gain_slider_.onValueChange = [this]() {
            if (!updating_) {
                client_.set_input_gain(static_cast<float>(input_gain_slider_.getValue()));
            }
        };
    }

    void refresh() {
        updating_ = true;
        const float level = client_.get_own_audio_level();
        const float peak = client_.get_own_audio_peak();
        audio_level_ = level >= audio_level_ ? level
                                             : std::max(level,
                                                        audio_level_ * METER_LEVEL_RELEASE);
        audio_peak_ = peak >= audio_peak_ ? peak
                                          : std::max(peak,
                                                     audio_peak_ * METER_PEAK_RELEASE);
        const bool muted = client_.get_mic_muted();
        mute_button_.setToggleState(muted, juce::dontSendNotification);
        monitor_button_.setToggleState(client_.get_self_monitor_enabled(),
                                       juce::dontSendNotification);
        input_gain_slider_.setValue(client_.get_input_gain(), juce::dontSendNotification);

        if (!client_.is_join_confirmed()) {
            status_text_ = "Joining...";
            status_colour_ = juce_theme::colour::warning();
        } else {
            status_text_ =
                client_.is_audio_stream_active() ? "Audio running" : "Audio stopped";
            status_colour_ = muted ? juce_theme::colour::danger()
                                   : (client_.is_audio_stream_active()
                                          ? juce_theme::colour::success()
                                          : juce_theme::colour::warning());
        }
        status_label_.setText(status_text_, juce::dontSendNotification);
        status_label_.setColour(juce::Label::textColourId, status_colour_);
        updating_ = false;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce_theme::colour::row());
        g.fillRect(area);
        g.setColour(juce_theme::colour::border_soft());
        g.drawHorizontalLine(getHeight() - 1, 0.0F, static_cast<float>(getWidth()));
        draw_row_separators(g, getHeight());

        auto avatar = avatar_bounds_.toFloat();
        g.setColour(juce::Colours::transparentBlack);
        g.fillRoundedRectangle(avatar, 3.0F);
        g.setColour(juce_theme::colour::border());
        g.drawRoundedRectangle(avatar, 3.0F, 1.0F);
        draw_icon(g, avatar_bounds_.reduced(8), RowIcon::user,
                  juce_theme::colour::text_dim());

        draw_level_meter(g, level_meter_bounds_, audio_level_, audio_peak_,
                         client_.get_mic_muted() ? juce_theme::colour::text_faint()
                                                 : juce_theme::colour::accent_hi());
        draw_icon(g, speaker_icon_bounds_, RowIcon::speaker,
                  client_.get_mic_muted() ? juce_theme::colour::text_faint()
                                          : juce_theme::colour::text_dim());

        auto status = status_bounds_;
        auto dot = status.removeFromLeft(16).toFloat();
        g.setColour(status_colour_);
        g.fillEllipse(dot.withSizeKeepingCentre(8.0F, 8.0F));
    }

    void resized() override {
        auto area = getLocalBounds().reduced(PAD, 6);
        auto left = area.removeFromLeft(NAME_WIDTH);
        avatar_bounds_ = left.removeFromLeft(44).withSizeKeepingCentre(36, 36);
        left.removeFromLeft(10);
        name_label_.setBounds(left.removeFromTop(24));
        stats_label_.setBounds(left);

        area.removeFromLeft(8);
        auto level = area.removeFromLeft(LEVEL_WIDTH);
        speaker_icon_bounds_ = level.removeFromRight(36).withSizeKeepingCentre(20, 20);
        level_meter_bounds_ = level.withSizeKeepingCentre(level.getWidth(), 36);
        auto buttons = area.removeFromLeft(BUTTONS_WIDTH).reduced(8, 0);
        mute_button_.setBounds(buttons.removeFromLeft(28).withSizeKeepingCentre(28, 24));
        buttons.removeFromLeft(8);
        monitor_button_.setBounds(buttons.removeFromLeft(28).withSizeKeepingCentre(28, 24));
        input_gain_slider_.setBounds(area.removeFromLeft(GAIN_WIDTH).reduced(6, 2));
        pan_label_.setBounds(area.removeFromLeft(PAN_WIDTH).withSizeKeepingCentre(54, 22));
        queue_label_.setBounds(area.removeFromLeft(QUEUE_WIDTH).withSizeKeepingCentre(54, 22));
        status_bounds_ = area.removeFromLeft(STATUS_WIDTH).withSizeKeepingCentre(92, 24);
        status_label_.setBounds(status_bounds_.withTrimmedLeft(16));
        area.removeFromLeft(JITTER_WIDTH);
        area.removeFromLeft(MS_WIDTH);
        area.removeFromLeft(RESET_WIDTH);
    }

private:
    ClientAppFacade& client_;
    bool updating_ = false;
    float audio_level_ = 0.0F;
    float audio_peak_ = 0.0F;
    juce::String status_text_;
    juce::Colour status_colour_ = juce_theme::colour::text_faint();

    juce::Label name_label_;
    juce::Label stats_label_;
    juce::Label queue_label_;
    juce::Label status_label_;
    juce::Label pan_label_;
    juce::Slider input_gain_slider_;
    juce::TextButton monitor_button_;
    juce::TextButton mute_button_;
    juce::Rectangle<int> avatar_bounds_;
    juce::Rectangle<int> level_meter_bounds_;
    juce::Rectangle<int> speaker_icon_bounds_;
    juce::Rectangle<int> status_bounds_;
};

class JuceParticipantListComponent::ParticipantRowComponent final
    : public juce::Component {
public:
    explicit ParticipantRowComponent(ClientAppFacade& client) : client_(client) {
        juce_theme::style_label(name_label_, juce_theme::colour::text(), 15.0F, true);
        juce_theme::style_label(stats_label_, juce_theme::colour::text_dim(), 12.0F);
        mute_button_.setClickingTogglesState(true);
        mute_button_.setButtonText("M");
        auto_jitter_toggle_.setButtonText("Auto");
        jitter_ms_editor_.setInputRestrictions(4, "0123456789");
        juce_theme::style_editor(jitter_ms_editor_);
        reset_jitter_button_.setButtonText("Reset");
        configure_linear_slider(gain_slider_, 0.0, 2.0, 0.01, "x");
        configure_rotary_slider(pan_slider_, 0.0, 1.0, 0.01);

        add_all(*this, {&name_label_, &stats_label_, &mute_button_, &gain_slider_,
                        &pan_slider_, &auto_jitter_toggle_, &jitter_ms_editor_,
                        &reset_jitter_button_});

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
        displayed_audio_level_ = info.audio_level >= displayed_audio_level_
                                     ? info.audio_level
                                     : std::max(info.audio_level,
                                                displayed_audio_level_ *
                                                    METER_LEVEL_RELEASE);
        displayed_audio_peak_ = info.audio_peak >= displayed_audio_peak_
                                    ? info.audio_peak
                                    : std::max(info.audio_peak,
                                               displayed_audio_peak_ * METER_PEAK_RELEASE);

        name_label_.setText(participant_name(info), juce::dontSendNotification);
        mute_button_.setToggleState(info.is_muted, juce::dontSendNotification);
        gain_slider_.setValue(info.gain, juce::dontSendNotification);
        pan_slider_.setValue(info.pan, juce::dontSendNotification);
        auto_jitter_toggle_.setToggleState(info.opus_jitter_auto_enabled,
                                           juce::dontSendNotification);
        const auto jitter_ms = static_cast<int>(std::lround(
            static_cast<double>(info.jitter_buffer_floor_packets) *
            client_.get_opus_network_packet_ms()));
        jitter_ms_editor_.setText(juce::String(jitter_ms), false);

        stats_label_.setText(profile_subtitle(info), juce::dontSendNotification);

        updating_ = false;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce_theme::colour::row());
        g.fillRect(area);
        g.setColour(juce_theme::colour::border_soft());
        g.drawHorizontalLine(getHeight() - 1, 0.0F, static_cast<float>(getWidth()));
        draw_row_separators(g, getHeight());

        auto avatar = avatar_bounds_.toFloat();
        juce::Colour avatar_colour =
            info_.is_muted ? juce_theme::colour::danger().withAlpha(0.35F)
                           : juce_theme::colour::accent().withAlpha(0.45F);
        g.setColour(avatar_colour);
        g.fillRoundedRectangle(avatar, 3.0F);
        g.setColour(juce_theme::colour::text());
        g.setFont(juce_theme::font(20.0F, true));
        g.drawFittedText(participant_name(info_).substring(0, 1), avatar_bounds_,
                         juce::Justification::centred, 1);

        draw_level_meter(g, level_meter_bounds_, displayed_audio_level_,
                         displayed_audio_peak_,
                         info_.is_speaking ? juce_theme::colour::accent_hi()
                                           : juce_theme::colour::success());
        draw_icon(g, speaker_icon_bounds_, RowIcon::speaker,
                  info_.is_muted ? juce_theme::colour::text_faint()
                                 : juce_theme::colour::text_dim());

        const auto queue_limit =
            std::max<size_t>(info_.opus_queue_limit_packets, size_t{1});
        const float queue_amount =
            static_cast<float>(std::min(info_.queue_size, queue_limit)) /
            static_cast<float>(queue_limit);
        draw_meter_bars(g, queue_meter_bounds_, queue_amount,
                        juce_theme::colour::accent_hi());

        g.setFont(juce_theme::font(12.0F));
        g.setColour(juce_theme::colour::text_dim());
        g.drawFittedText(juce::String(static_cast<int>(info_.queue_size)) + " / " +
                             juce::String(static_cast<int>(queue_limit)),
                         queue_text_bounds_, juce::Justification::centred, 1);

        const auto status_colour =
            info_.is_muted ? juce_theme::colour::danger()
                           : (info_.buffer_ready ? juce_theme::colour::success()
                                                 : juce_theme::colour::warning());
        auto status = status_bounds_;
        auto dot = status.removeFromLeft(16).toFloat();
        g.setColour(status_colour);
        g.fillEllipse(dot.withSizeKeepingCentre(8.0F, 8.0F));
        g.setColour(info_.is_muted ? juce_theme::colour::danger()
                                   : juce_theme::colour::text_dim());
        g.drawFittedText(info_.is_muted ? "Muted"
                                        : (info_.buffer_ready ? "Connected" : "Buffering"),
                         status, juce::Justification::centredLeft, 1);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(PAD, 6);
        auto left = area.removeFromLeft(NAME_WIDTH);
        avatar_bounds_ = left.removeFromLeft(44).withSizeKeepingCentre(36, 36);
        left.removeFromLeft(10);
        name_label_.setBounds(left.removeFromTop(24));
        stats_label_.setBounds(left);

        area.removeFromLeft(8);
        auto level = area.removeFromLeft(LEVEL_WIDTH);
        speaker_icon_bounds_ = level.removeFromRight(36).withSizeKeepingCentre(20, 20);
        level_meter_bounds_ = level.withSizeKeepingCentre(level.getWidth(), 36);
        auto buttons = area.removeFromLeft(BUTTONS_WIDTH).reduced(8, 0);
        mute_button_.setBounds(buttons.removeFromLeft(28).withSizeKeepingCentre(28, 24));

        gain_slider_.setBounds(area.removeFromLeft(GAIN_WIDTH).reduced(6, 2));
        pan_slider_.setBounds(area.removeFromLeft(PAN_WIDTH).reduced(4, 0));

        auto queue = area.removeFromLeft(QUEUE_WIDTH);
        queue_meter_bounds_ = queue.removeFromTop(36).reduced(6, 4);
        queue_text_bounds_ = queue.reduced(0, 0);

        status_bounds_ = area.removeFromLeft(STATUS_WIDTH).withSizeKeepingCentre(92, 24);
        auto_jitter_toggle_.setBounds(
            area.removeFromLeft(JITTER_WIDTH).withSizeKeepingCentre(66, 24));
        jitter_ms_editor_.setBounds(
            area.removeFromLeft(MS_WIDTH).withSizeKeepingCentre(54, 22));
        reset_jitter_button_.setBounds(
            area.removeFromLeft(RESET_WIDTH).withSizeKeepingCentre(62, 24));
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
    float displayed_audio_level_ = 0.0F;
    float displayed_audio_peak_ = 0.0F;

    juce::Label name_label_;
    juce::Label stats_label_;
    juce::TextButton mute_button_;
    juce::Slider gain_slider_;
    juce::Slider pan_slider_;
    juce::ToggleButton auto_jitter_toggle_;
    juce::TextEditor jitter_ms_editor_;
    juce::TextButton reset_jitter_button_;
    juce::Rectangle<int> avatar_bounds_;
    juce::Rectangle<int> level_meter_bounds_;
    juce::Rectangle<int> speaker_icon_bounds_;
    juce::Rectangle<int> queue_meter_bounds_;
    juce::Rectangle<int> queue_text_bounds_;
    juce::Rectangle<int> status_bounds_;
};

JuceParticipantListComponent::JuceParticipantListComponent(ClientAppFacade& client)
    : client_(client) {
    participant_header_label_.setText("Participants", juce::dontSendNotification);
    juce_theme::style_label(participant_header_label_, juce_theme::colour::text(),
                            15.0F, true);
    empty_participants_label_.setText("No remote participants", juce::dontSendNotification);
    juce_theme::style_label(empty_participants_label_, juce_theme::colour::text_faint(),
                            14.0F);
    empty_participants_label_.setJustificationType(juce::Justification::centred);
    participants_viewport_.setViewedComponent(&participants_content_, false);

    addAndMakeVisible(participant_header_label_);
    addAndMakeVisible(empty_participants_label_);
    addAndMakeVisible(participants_viewport_);
    startTimerHz(60);
}

JuceParticipantListComponent::~JuceParticipantListComponent() {
    stopTimer();
}

void JuceParticipantListComponent::timerCallback() {
    refresh();
}

void JuceParticipantListComponent::refresh() {
    const auto participants = client_.get_participant_info();
    local_participant_visible_ = !client_.get_room_id().empty();
    visible_participant_count_ = participants.size();
    if (!local_participant_row_) {
        local_participant_row_ = std::make_unique<LocalParticipantRowComponent>(client_);
        participants_content_.addAndMakeVisible(local_participant_row_.get());
    }
    local_participant_row_->setVisible(local_participant_visible_);
    if (local_participant_visible_) {
        local_participant_row_->refresh();
    }

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

    const size_t visible_row_count =
        visible_participant_count_ + (local_participant_visible_ ? size_t{1} : size_t{0});
    empty_participants_label_.setVisible(visible_row_count == 0);
    participants_viewport_.setVisible(visible_row_count > 0);
    resized();
}

void JuceParticipantListComponent::paint(juce::Graphics& g) {
    auto area = getLocalBounds();
    area.removeFromTop(28);
    auto header = area.removeFromTop(28).reduced(10, 0);

    header.removeFromLeft(NAME_WIDTH);
    header.removeFromLeft(8);
    draw_column_label(g, header.removeFromLeft(LEVEL_WIDTH), "Level");
    header.removeFromLeft(BUTTONS_WIDTH);
    draw_column_label(g, header.removeFromLeft(GAIN_WIDTH), "Gain");
    draw_column_label(g, header.removeFromLeft(PAN_WIDTH), "Pan");
    draw_column_label(g, header.removeFromLeft(QUEUE_WIDTH), "Queue");
    draw_column_label(g, header.removeFromLeft(STATUS_WIDTH), "Status");
    draw_column_label(g, header.removeFromLeft(JITTER_WIDTH), "Jitter");
    draw_column_label(g, header.removeFromLeft(MS_WIDTH), "ms");
    draw_column_label(g, header.removeFromLeft(RESET_WIDTH), "Reset");

    g.setColour(juce_theme::colour::border_soft());
    g.drawHorizontalLine(55, 10.0F, static_cast<float>(getWidth() - 10));
}

void JuceParticipantListComponent::resized() {
    auto area = getLocalBounds();
    participant_header_label_.setBounds(area.removeFromTop(28));
    area.removeFromTop(28);
    area.removeFromTop(4);
    empty_participants_label_.setBounds(area);
    participants_viewport_.setBounds(area);

    const int content_width =
        std::max(area.getWidth() - participants_viewport_.getScrollBarThickness(),
                 CONTENT_MIN_WIDTH);
    const size_t visible_row_count =
        visible_participant_count_ + (local_participant_visible_ ? size_t{1} : size_t{0});
    participants_content_.setSize(content_width,
                                  static_cast<int>(visible_row_count) *
                                      PARTICIPANT_ROW_HEIGHT);
    int row_index = 0;
    if (local_participant_row_) {
        local_participant_row_->setBounds(0, 0, content_width, PARTICIPANT_ROW_HEIGHT);
        row_index = local_participant_visible_ ? 1 : 0;
    }
    for (size_t index = 0; index < participant_rows_.size(); ++index) {
        participant_rows_[index]->setBounds(
            0, (static_cast<int>(index) + row_index) * PARTICIPANT_ROW_HEIGHT,
            content_width,
            PARTICIPANT_ROW_HEIGHT);
    }
}
