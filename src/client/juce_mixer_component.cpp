#include "juce_mixer_component.h"

#include "opus_network_clock.h"
#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

namespace {
constexpr int PAD = 10;
constexpr int ROW = 28;
constexpr int PARTICIPANT_ROW_HEIGHT = 78;

juce::String format_bytes(uint64_t bytes) {
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    const double value = static_cast<double>(bytes);
    if (value >= gib) {
        return juce::String(value / gib, 2) + " GB";
    }
    if (value >= mib) {
        return juce::String(value / mib, 1) + " MB";
    }
    if (value >= kib) {
        return juce::String(value / kib, 1) + " KB";
    }
    return juce::String(bytes) + " B";
}

juce::String participant_name(const ParticipantInfo& participant) {
    if (!participant.display_name.empty()) {
        return participant.display_name;
    }
    if (!participant.profile_id.empty()) {
        return participant.profile_id;
    }
    return "User " + juce::String(participant.id);
}

juce::String opus_packet_label(int frames) {
    if (frames == opus_network_clock::LOW_LATENCY_FRAME_COUNT) {
        return "Low";
    }
    if (frames == opus_network_clock::FAST_FRAME_COUNT) {
        return "Fast";
    }
    if (frames == opus_network_clock::BALANCED_FRAME_COUNT) {
        return "Balanced";
    }
    return "Stable";
}

juce::String redundancy_label(int depth, int effective_depth) {
    if (depth == OPUS_REDUNDANCY_DEPTH_AUTO) {
        return "Auto (" + juce::String(effective_depth) + ")";
    }
    if (depth == 0) {
        return "Off";
    }
    return juce::String(depth) + " prev";
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

bool matches_api(const AudioStream::DeviceInfo& device, const std::string& api_name) {
    return api_name.empty() || api_name == "All" || device.api_name == api_name;
}

int api_index_for_name(const std::vector<AudioStream::ApiInfo>& apis,
                       const std::string& api_name) {
    if (api_name.empty() || api_name == "All") {
        return -1;
    }
    for (const auto& api: apis) {
        if (api.name == api_name) {
            return api.index;
        }
    }
    return -1;
}

std::string api_name_for_index(const std::vector<AudioStream::ApiInfo>& apis,
                               int api_index) {
    if (api_index < 0) {
        return "All";
    }
    for (const auto& api: apis) {
        if (api.index == api_index) {
            return api.name;
        }
    }
    return "All";
}

bool contains_device_id(const std::vector<AudioStream::DeviceInfo>& devices,
                        AudioStream::DeviceIndex id) {
    return std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
        return device.index == id;
    });
}

AudioStream::DeviceIndex first_device_for_api(
    const std::vector<AudioStream::DeviceInfo>& devices, const std::string& api_name) {
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return matches_api(device, api_name);
    });
    return it == devices.end() ? AudioStream::NO_DEVICE : it->index;
}

AudioStream::DeviceIndex default_device_for_api(
    const std::vector<AudioStream::DeviceInfo>& devices,
    const std::vector<AudioStream::ApiInfo>& apis, int api_index, bool input) {
    if (api_index >= 0) {
        for (const auto& api: apis) {
            if (api.index != api_index) {
                continue;
            }
            const auto default_device =
                input ? api.default_input_device : api.default_output_device;
            if (contains_device_id(devices, default_device)) {
                return default_device;
            }
            return first_device_for_api(devices, api.name);
        }
    }

    const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return input ? device.is_default_input : device.is_default_output;
    });
    if (it != devices.end()) {
        return it->index;
    }
    return devices.empty() ? AudioStream::NO_DEVICE : devices.front().index;
}

}  // namespace

class JuceMixerComponent::ParticipantRowComponent final : public juce::Component {
public:
    ParticipantRowComponent(ClientAppFacade& client) : client_(client) {
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

JuceMixerComponent::JuceMixerComponent(
    ClientAppFacade& client, JuceClientStartupAudioOptions startup_audio_options)
    : client_(client), startup_audio_options_(std::move(startup_audio_options)) {
    configure_controls();
    configure_device_controls();
    startTimerHz(15);
}

JuceMixerComponent::~JuceMixerComponent() {
    stopTimer();
    if (device_job_thread_.joinable()) {
        device_job_thread_.join();
    }
}

void JuceMixerComponent::configure_controls() {
    setOpaque(true);

    status_label_.setFont(juce::Font(juce::FontOptions(16.0F, juce::Font::bold)));
    transport_label_.setFont(juce::Font(juce::FontOptions(13.0F)));
    diagnostics_label_.setFont(juce::Font(juce::FontOptions(13.0F)));
    diagnostics_label_.setJustificationType(juce::Justification::topLeft);
    participant_header_label_.setText("Participants", juce::dontSendNotification);
    participant_header_label_.setFont(
        juce::Font(juce::FontOptions(16.0F, juce::Font::bold)));
    empty_participants_label_.setText("No remote participants", juce::dontSendNotification);
    empty_participants_label_.setJustificationType(juce::Justification::centred);

    mic_mute_button_.setButtonText("Mute mic");
    mic_mute_button_.setClickingTogglesState(true);
    monitor_toggle_.setButtonText("Monitor");
    configure_linear_slider(input_gain_slider_, 0.0, 2.0, 0.01, "x");
    configure_linear_slider(jitter_ms_slider_, 0.0, 200.0, 1.0, " ms");
    configure_linear_slider(queue_limit_slider_, 1.0, MAX_OPUS_QUEUE_LIMIT_PACKETS, 1.0,
                            " pkt");
    configure_linear_slider(age_limit_slider_, 1.0, 500.0, 1.0, " ms");
    auto_jitter_toggle_.setButtonText("Auto jitter default");

    bpm_editor_.setInputRestrictions(6, "0123456789.");
    metronome_start_stop_button_.setButtonText("Start metro");
    metronome_tap_button_.setButtonText("Tap");
    record_button_.setButtonText("Record");

    wav_path_editor_.setTextToShowWhenEmpty("WAV path", juce::Colours::grey);
    wav_load_button_.setButtonText("Load");
    wav_play_button_.setButtonText("Play");
    configure_linear_slider(wav_position_slider_, 0.0, 1.0, 1.0);
    configure_linear_slider(wav_gain_slider_, 0.0, 2.0, 0.01, "x");
    wav_mute_toggle_.setButtonText("Local mute");

    participants_viewport_.setViewedComponent(&participants_content_, false);

    add_all(*this, {&status_label_, &transport_label_, &diagnostics_label_,
                    &participant_header_label_, &empty_participants_label_,
                    &mic_mute_button_, &monitor_toggle_, &input_gain_slider_,
                    &jitter_ms_slider_, &queue_limit_slider_, &age_limit_slider_,
                    &auto_jitter_toggle_, &redundancy_combo_, &bpm_editor_,
                    &metronome_start_stop_button_, &metronome_tap_button_,
                    &record_button_, &wav_path_editor_, &wav_load_button_,
                    &wav_play_button_, &wav_position_slider_, &wav_gain_slider_,
                    &wav_mute_toggle_, &participants_viewport_});

    mic_mute_button_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_mic_muted(mic_mute_button_.getToggleState());
        }
    };
    monitor_toggle_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_self_monitor_enabled(monitor_toggle_.getToggleState());
        }
    };
    input_gain_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_input_gain(static_cast<float>(input_gain_slider_.getValue()));
        }
    };
    jitter_ms_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_opus_jitter_buffer_ms(
                std::max(0, static_cast<int>(std::lround(jitter_ms_slider_.getValue()))));
            if (client_.get_opus_queue_limit_packets() <
                client_.get_opus_jitter_buffer_packets()) {
                client_.set_opus_queue_limit_packets(
                    client_.get_opus_jitter_buffer_packets());
            }
        }
    };
    queue_limit_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_opus_queue_limit_packets(static_cast<size_t>(
                std::max(1, static_cast<int>(std::lround(queue_limit_slider_.getValue())))));
        }
    };
    age_limit_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_jitter_packet_age_limit_ms(
                std::max(1, static_cast<int>(std::lround(age_limit_slider_.getValue()))));
        }
    };
    auto_jitter_toggle_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_opus_auto_jitter_default(auto_jitter_toggle_.getToggleState());
        }
    };
    redundancy_combo_.onChange = [this]() {
        if (updating_from_client_) {
            return;
        }
        const int selected = redundancy_combo_.getSelectedId();
        if (selected == 1) {
            client_.set_opus_redundancy_depth(OPUS_REDUNDANCY_DEPTH_AUTO);
        } else {
            client_.set_opus_redundancy_depth(selected - 2);
        }
    };
    bpm_editor_.onReturnKey = [this]() { commit_metronome_bpm(); };
    bpm_editor_.onFocusLost = [this]() { commit_metronome_bpm(); };
    metronome_start_stop_button_.onClick = [this]() {
        if (client_.get_metronome_state().running) {
            client_.stop_metronome();
        } else {
            commit_metronome_bpm();
            client_.start_metronome();
        }
    };
    metronome_tap_button_.onClick = [this]() { client_.tap_metronome_tempo(); };
    record_button_.onClick = [this]() {
        if (client_.get_recording_state().active) {
            client_.stop_recording();
        } else {
            client_.start_recording();
        }
    };
    wav_load_button_.onClick = [this]() { load_wav_file(); };
    wav_play_button_.onClick = [this]() {
        if (client_.get_wav_state().is_playing) {
            client_.wav_pause();
        } else {
            client_.wav_play();
        }
    };
    wav_position_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.wav_seek(static_cast<int64_t>(std::llround(wav_position_slider_.getValue())));
        }
    };
    wav_gain_slider_.onValueChange = [this]() {
        if (!updating_from_client_) {
            client_.set_wav_gain(static_cast<float>(wav_gain_slider_.getValue()));
        }
    };
    wav_mute_toggle_.onClick = [this]() {
        if (!updating_from_client_) {
            client_.set_wav_muted_local(wav_mute_toggle_.getToggleState());
        }
    };
}

void JuceMixerComponent::configure_device_controls() {
    apply_audio_button_.setButtonText("Apply");
    start_stop_audio_button_.setButtonText("Start");
    reset_audio_button_.setButtonText("Reset");
    refresh_devices_button_.setButtonText("Refresh");

    add_all(*this, {&api_combo_, &input_combo_, &input_channel_combo_, &output_combo_,
                    &buffer_combo_, &opus_packet_combo_, &apply_audio_button_,
                    &start_stop_audio_button_, &reset_audio_button_,
                    &refresh_devices_button_, &device_status_label_});

    set_device_status("Audio devices will load after the window opens...");
    set_device_controls_enabled(false);

    api_combo_.onChange = [this]() {
        if (updating_from_client_) {
            return;
        }
        const int old_api_index = selected_api_index_;
        const int selected_id = api_combo_.getSelectedId();
        selected_api_index_ = selected_id <= 1 ? -1 : selected_id - 2;
        apply_selected_api_to_pending_devices(old_api_index);
        populate_device_combos();
    };
    input_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_input_ = selected_input_device();
            pending_input_channel_ =
                std::clamp(pending_input_channel_, 0,
                           max_input_channels_for(pending_input_) - 1);
            populate_input_channel_combo();
        }
    };
    input_channel_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_input_channel_ = std::max(0, input_channel_combo_.getSelectedId() - 1);
        }
    };
    output_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_output_ = selected_output_device();
        }
    };
    buffer_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_buffer_frames_ = buffer_combo_.getSelectedId();
        }
    };
    opus_packet_combo_.onChange = [this]() {
        if (!updating_from_client_) {
            pending_opus_frames_per_packet_ = opus_packet_combo_.getSelectedId();
        }
    };

    apply_audio_button_.onClick = [this]() { apply_audio_settings(); };
    start_stop_audio_button_.onClick = [this]() { start_or_stop_audio(); };
    reset_audio_button_.onClick = [this]() { reset_audio_path(); };
    refresh_devices_button_.onClick = [this]() { refresh_audio_device_controls(); };

    pending_input_ = client_.get_selected_input_device();
    pending_output_ = client_.get_selected_output_device();
    pending_input_channel_ = client_.get_input_channel_index();
    pending_buffer_frames_ = client_.get_audio_config().frames_per_buffer;
    pending_opus_frames_per_packet_ = client_.get_opus_network_frame_count();
    selected_api_index_ = api_index_for_name(client_.get_audio_api_filter());

    populate_buffer_combo();
    populate_opus_packet_combo();
    populate_redundancy_combo();
}

void JuceMixerComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff12171c));
    g.setColour(juce::Colour(0xff202832));
    g.drawLine(0.0F, 48.0F, static_cast<float>(getWidth()), 48.0F, 1.0F);
    g.drawLine(0.0F, static_cast<float>(getHeight() - 104), static_cast<float>(getWidth()),
               static_cast<float>(getHeight() - 104), 1.0F);
}

void JuceMixerComponent::resized() {
    auto area = getLocalBounds().reduced(PAD);
    auto top = area.removeFromTop(48);
    status_label_.setBounds(top.removeFromTop(24));
    transport_label_.setBounds(top);

    auto bottom = area.removeFromBottom(104);
    auto bottom_top = bottom.removeFromTop(ROW);
    api_combo_.setBounds(bottom_top.removeFromLeft(110).reduced(3));
    input_combo_.setBounds(bottom_top.removeFromLeft(230).reduced(3));
    input_channel_combo_.setBounds(bottom_top.removeFromLeft(70).reduced(3));
    output_combo_.setBounds(bottom_top.removeFromLeft(230).reduced(3));
    buffer_combo_.setBounds(bottom_top.removeFromLeft(90).reduced(3));
    opus_packet_combo_.setBounds(bottom_top.removeFromLeft(120).reduced(3));
    apply_audio_button_.setBounds(bottom_top.removeFromLeft(76).reduced(3));
    start_stop_audio_button_.setBounds(bottom_top.removeFromLeft(76).reduced(3));
    reset_audio_button_.setBounds(bottom_top.removeFromLeft(70).reduced(3));
    refresh_devices_button_.setBounds(bottom_top.removeFromLeft(82).reduced(3));

    device_status_label_.setBounds(bottom.removeFromTop(ROW).reduced(3));

    auto master = area.removeFromLeft(310).reduced(0, PAD);
    mic_mute_button_.setBounds(master.removeFromTop(ROW).removeFromLeft(120).reduced(3));
    monitor_toggle_.setBounds(master.removeFromTop(ROW).reduced(3));
    input_gain_slider_.setBounds(master.removeFromTop(ROW).reduced(3));
    jitter_ms_slider_.setBounds(master.removeFromTop(ROW).reduced(3));
    queue_limit_slider_.setBounds(master.removeFromTop(ROW).reduced(3));
    age_limit_slider_.setBounds(master.removeFromTop(ROW).reduced(3));
    auto_jitter_toggle_.setBounds(master.removeFromTop(ROW).reduced(3));
    redundancy_combo_.setBounds(master.removeFromTop(ROW).reduced(3));

    auto metro = master.removeFromTop(ROW);
    bpm_editor_.setBounds(metro.removeFromLeft(72).reduced(3));
    metronome_start_stop_button_.setBounds(metro.removeFromLeft(110).reduced(3));
    metronome_tap_button_.setBounds(metro.removeFromLeft(70).reduced(3));
    record_button_.setBounds(master.removeFromTop(ROW).removeFromLeft(110).reduced(3));

    wav_path_editor_.setBounds(master.removeFromTop(ROW).reduced(3));
    auto wav_row = master.removeFromTop(ROW);
    wav_load_button_.setBounds(wav_row.removeFromLeft(70).reduced(3));
    wav_play_button_.setBounds(wav_row.removeFromLeft(70).reduced(3));
    wav_mute_toggle_.setBounds(wav_row.reduced(3));
    wav_position_slider_.setBounds(master.removeFromTop(ROW).reduced(3));
    wav_gain_slider_.setBounds(master.removeFromTop(ROW).reduced(3));
    diagnostics_label_.setBounds(master.reduced(3));

    auto participant_area = area.reduced(PAD, PAD);
    participant_header_label_.setBounds(participant_area.removeFromTop(26));
    empty_participants_label_.setBounds(participant_area);
    participants_viewport_.setBounds(participant_area);

    const int content_width =
        std::max(participant_area.getWidth() - participants_viewport_.getScrollBarThickness(),
                 760);
    participants_content_.setSize(content_width,
                                  static_cast<int>(visible_participant_count_) *
                                      PARTICIPANT_ROW_HEIGHT);
    for (size_t index = 0; index < participant_rows_.size(); ++index) {
        participant_rows_[index]->setBounds(
            0, static_cast<int>(index) * PARTICIPANT_ROW_HEIGHT, content_width,
            PARTICIPANT_ROW_HEIGHT);
    }
}

void JuceMixerComponent::timerCallback() {
    poll_audio_device_refresh();
    if (!startup_device_refresh_started_) {
        --device_load_delay_ticks_;
        if (device_load_delay_ticks_ <= 0) {
            startup_device_refresh_started_ = true;
            request_audio_device_refresh(startup_audio_options_.auto_start_audio);
        }
    }
    refresh_live_state();
}

void JuceMixerComponent::refresh_live_state() {
    updating_from_client_ = true;

    const auto participants = client_.get_participant_info();
    const auto device_info = client_.get_device_info();
    bool device_job_active = false;
    {
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        device_job_active = device_job_running_;
    }
    const auto latency =
        device_job_active ? AudioStream::LatencyInfo{} : client_.get_latency_info();
    const auto callback_timing = client_.get_callback_timing_info();
    const auto path = client_.get_path_diagnostics();
    const auto metronome = client_.get_metronome_state();
    const auto recording = client_.get_recording_state();
    const auto wav = client_.get_wav_state();

    status_label_.setText(
        juce::String(client_.get_server_address()) + ":" +
            juce::String(static_cast<int>(client_.get_server_port())) + " | Room " +
            juce::String(client_.get_room_id()) + " | RTT " +
            juce::String(client_.get_rtt_ms(), 1) + " ms | Users " +
            juce::String(static_cast<int>(participants.size())) + " | RX " +
            format_bytes(client_.get_total_bytes_rx()) + " | TX " +
            format_bytes(client_.get_total_bytes_tx()),
        juce::dontSendNotification);

    transport_label_.setText(
        juce::String(client_.is_audio_stream_active() ? "Audio running" : "Audio stopped") +
            " | " + device_info.input_api + " | In " + device_info.input_device_name +
            " ch " + juce::String(device_info.input_channel_index + 1) + " | Out " +
            device_info.output_device_name,
        juce::dontSendNotification);

    mic_mute_button_.setToggleState(client_.get_mic_muted(), juce::dontSendNotification);
    monitor_toggle_.setToggleState(client_.get_self_monitor_enabled(),
                                   juce::dontSendNotification);
    input_gain_slider_.setValue(client_.get_input_gain(), juce::dontSendNotification);
    jitter_ms_slider_.setValue(client_.get_opus_jitter_buffer_ms(),
                               juce::dontSendNotification);
    queue_limit_slider_.setValue(static_cast<double>(client_.get_opus_queue_limit_packets()),
                                 juce::dontSendNotification);
    age_limit_slider_.setValue(client_.get_jitter_packet_age_limit_ms(),
                               juce::dontSendNotification);
    auto_jitter_toggle_.setToggleState(client_.get_opus_auto_jitter_default(),
                                       juce::dontSendNotification);

    const int redundancy_depth = client_.get_opus_redundancy_depth_setting();
    redundancy_combo_.setText(redundancy_label(redundancy_depth,
                                               client_.get_effective_opus_redundancy_depth()),
                              juce::dontSendNotification);

    if (!bpm_editor_.hasKeyboardFocus(true)) {
        bpm_editor_.setText(juce::String(metronome.bpm, 1), false);
    }
    metronome_start_stop_button_.setButtonText(metronome.running ? "Stop metro"
                                                                  : "Start metro");
    record_button_.setButtonText(recording.active ? "Stop rec" : "Record");

    wav_play_button_.setButtonText(wav.is_playing ? "Pause" : "Play");
    wav_gain_slider_.setValue(wav.gain, juce::dontSendNotification);
    wav_mute_toggle_.setToggleState(wav.muted_local, juce::dontSendNotification);
    wav_position_slider_.setRange(0.0, static_cast<double>(std::max<int64_t>(wav.total_frames, 1)),
                                  1.0);
    wav_position_slider_.setValue(static_cast<double>(wav.position),
                                  juce::dontSendNotification);

    diagnostics_label_.setText(
        "Opus " + juce::String(client_.get_opus_network_frame_count()) + " frames / " +
            juce::String(client_.get_opus_network_packet_ms(), 1) + " ms\n" +
            "Jitter " + juce::String(client_.get_opus_jitter_buffer_ms()) + " ms (" +
            juce::String(static_cast<int>(client_.get_opus_jitter_buffer_packets())) +
            " pkt), queue " +
            juce::String(static_cast<int>(client_.get_opus_queue_limit_packets())) + "\n" +
            "Total est " + juce::String(path.total_estimate_ms, 1) + " ms, E2E avg max " +
            juce::String(path.e2e_latency_avg_max_ms, 1) + " ms\n" +
            "Latency in " + juce::String(latency.input_latency_ms, 1) + " ms, out " +
            juce::String(latency.output_latency_ms, 1) + " ms, buffer " +
            juce::String(latency.actual_buffer_frames) + " frames\n" +
            "Callback avg " + juce::String(callback_timing.avg_ms, 2) + " ms, max " +
            juce::String(callback_timing.max_ms, 2) + " ms, late " +
            juce::String(static_cast<int>(callback_timing.over_deadline_count)) + "\n" +
            "Metro beat " + juce::String(static_cast<int>(metronome.beat_number)) +
            ", sync " + juce::String(static_cast<int>(metronome.sync_sent)) + "/" +
            juce::String(static_cast<int>(metronome.sync_received)) + "\n" +
            "Recording " + juce::String(recording.active ? "on" : "off") + ", queued " +
            juce::String(static_cast<int>(recording.queued_blocks)) + ", dropped " +
            juce::String(static_cast<int>(recording.dropped_blocks)),
        juce::dontSendNotification);

    start_stop_audio_button_.setButtonText(client_.is_audio_stream_active() ? "Stop"
                                                                            : "Start");
    apply_audio_button_.setEnabled(device_controls_loaded_ && has_pending_audio_changes());

    updating_from_client_ = false;

    const double now_ms = juce::Time::getMillisecondCounterHiRes();
    if (now_ms - last_participant_refresh_ms_ > 250.0) {
        last_participant_refresh_ms_ = now_ms;
        refresh_participants();
    }
}

void JuceMixerComponent::refresh_participants() {
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

void JuceMixerComponent::refresh_audio_device_controls() {
    request_audio_device_refresh(false);
}

void JuceMixerComponent::request_audio_device_refresh(bool auto_start_audio) {
    {
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        if (device_job_running_) {
            set_device_status("Audio devices are already loading...");
            return;
        }
        device_job_running_ = true;
        device_job_finished_ = false;
        device_job_result_.reset();
    }

    if (device_job_thread_.joinable()) {
        device_job_thread_.join();
    }

    set_device_controls_enabled(false);
    set_device_status(auto_start_audio ? "Loading audio devices and starting audio..."
                                       : "Loading audio devices...");

    device_job_thread_ = std::thread([this, auto_start_audio]() {
        auto result = load_audio_devices(auto_start_audio);
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        device_job_result_ = std::move(result);
        device_job_finished_ = true;
    });
}

JuceMixerComponent::AudioDeviceRefreshResult JuceMixerComponent::load_audio_devices(
    bool auto_start_audio) {
    AudioDeviceRefreshResult result;
    result.pending_buffer_frames = client_.get_audio_config().frames_per_buffer;
    result.pending_opus_frames_per_packet = client_.get_opus_network_frame_count();
    result.pending_input_channel = client_.get_input_channel_index();

    try {
        result.input_devices = AudioStream::get_input_device_stubs();
        result.output_devices = AudioStream::get_output_device_stubs();
        result.available_apis = AudioStream::get_apis();

        const auto& preferences = startup_audio_options_.audio_preferences;
        const bool has_required_api =
            !startup_audio_options_.required_audio_api.empty();
        std::string target_api = has_required_api ? startup_audio_options_.required_audio_api
                                                  : preferences.audio_api;
        if (target_api.empty()) {
            target_api = "All";
        }

        result.selected_api_index = ::api_index_for_name(result.available_apis, target_api);
        const std::string resolved_api =
            api_name_for_index(result.available_apis, result.selected_api_index);

        if (has_required_api) {
            const bool has_input =
                first_device_for_api(result.input_devices, target_api) !=
                AudioStream::NO_DEVICE;
            const bool has_output =
                first_device_for_api(result.output_devices, target_api) !=
                AudioStream::NO_DEVICE;
            client_.set_audio_api_filter(target_api);
            if (!has_input || !has_output) {
                result.status = juce::String("Required audio API '") + target_api +
                                "' does not have both input and output devices";
                return result;
            }
        }

        if (preferences.loaded && !has_required_api) {
            result.pending_input = find_preferred_audio_device(
                result.input_devices, preferences.input_device, preferences.input_api,
                preferences.audio_api);
            result.pending_output = find_preferred_audio_device(
                result.output_devices, preferences.output_device, preferences.output_api,
                preferences.audio_api);
        }

        if (result.pending_input == AudioStream::NO_DEVICE) {
            result.pending_input = default_device_for_api(
                result.input_devices, result.available_apis, result.selected_api_index, true);
        }
        if (result.pending_output == AudioStream::NO_DEVICE) {
            result.pending_output = default_device_for_api(
                result.output_devices, result.available_apis, result.selected_api_index,
                false);
        }

        const auto startup_channel = startup_audio_options_.startup_input_channel_index;
        const auto preferred_channel = preferences.input_channel_index;
        result.pending_input_channel =
            startup_channel.value_or(preferred_channel.value_or(result.pending_input_channel));

        client_.set_audio_api_filter(has_required_api ? target_api : resolved_api);
        bool input_applied = result.pending_input != AudioStream::NO_DEVICE &&
                             client_.set_input_device(result.pending_input);
        bool output_applied = result.pending_output != AudioStream::NO_DEVICE &&
                              client_.set_output_device(result.pending_output);
        if (input_applied) {
            client_.set_input_channel_index(result.pending_input_channel);
            result.pending_input_channel = client_.get_input_channel_index();
        }

        if (auto_start_audio && input_applied && output_applied) {
            result.auto_start_attempted = true;
            AudioStream::AudioConfig config = client_.get_audio_config();
            result.auto_start_succeeded =
                client_.start_audio_stream(result.pending_input, result.pending_output,
                                           config);
        }

        if (result.input_devices.empty() || result.output_devices.empty()) {
            result.status = "No duplex audio devices found";
        } else if (!input_applied || !output_applied) {
            result.status = "Audio devices loaded; choose input and output";
        } else if (result.auto_start_attempted && result.auto_start_succeeded) {
            result.status = "Audio devices ready; audio started";
        } else if (result.auto_start_attempted) {
            const auto& error = AudioStream::get_last_error();
            result.status = error.empty() ? "Audio devices ready; auto-start failed"
                                          : "Auto-start failed: " + juce::String(error);
        } else {
            result.status = "Audio devices ready";
        }
    } catch (const std::exception& e) {
        result.status = "Audio device load failed: " + juce::String(e.what());
    }

    return result;
}

void JuceMixerComponent::poll_audio_device_refresh() {
    std::optional<AudioDeviceRefreshResult> result;
    {
        std::lock_guard<std::mutex> lock(device_job_mutex_);
        if (!device_job_finished_) {
            return;
        }
        result = std::move(device_job_result_);
        device_job_result_.reset();
        device_job_finished_ = false;
        device_job_running_ = false;
    }

    if (device_job_thread_.joinable()) {
        device_job_thread_.join();
    }
    if (result.has_value()) {
        apply_audio_device_refresh_result(std::move(*result));
    }
}

void JuceMixerComponent::apply_audio_device_refresh_result(
    AudioDeviceRefreshResult result) {
    input_devices_ = std::move(result.input_devices);
    output_devices_ = std::move(result.output_devices);
    available_apis_ = std::move(result.available_apis);
    selected_api_index_ = result.selected_api_index;
    pending_input_ = result.pending_input;
    pending_output_ = result.pending_output;
    pending_input_channel_ = result.pending_input_channel;
    pending_buffer_frames_ = result.pending_buffer_frames;
    pending_opus_frames_per_packet_ = result.pending_opus_frames_per_packet;

    device_controls_loaded_ = true;
    set_device_controls_enabled(true);
    populate_api_combo();
    populate_device_combos();
    populate_buffer_combo();
    populate_opus_packet_combo();
    set_device_status(result.status);
}

void JuceMixerComponent::set_device_controls_enabled(bool enabled) {
    api_combo_.setEnabled(enabled);
    input_combo_.setEnabled(enabled);
    input_channel_combo_.setEnabled(enabled);
    output_combo_.setEnabled(enabled);
    buffer_combo_.setEnabled(enabled);
    opus_packet_combo_.setEnabled(enabled);
    apply_audio_button_.setEnabled(enabled && has_pending_audio_changes());
    start_stop_audio_button_.setEnabled(enabled);
    reset_audio_button_.setEnabled(enabled);
    refresh_devices_button_.setEnabled(enabled);
}

void JuceMixerComponent::populate_api_combo() {
    updating_from_client_ = true;
    api_combo_.clear(juce::dontSendNotification);
    api_combo_.addItem("All APIs", 1);
    for (const auto& api: available_apis_) {
        api_combo_.addItem(api.name, api.index + 2);
    }
    api_combo_.setSelectedId(selected_api_index_ < 0 ? 1 : selected_api_index_ + 2,
                             juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_device_combos() {
    updating_from_client_ = true;

    const auto api_name = selected_api_name();
    input_combo_.clear(juce::dontSendNotification);
    int combo_id = 1;
    int selected_combo_id = 0;
    for (const auto& device: input_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        input_combo_.addItem(device.name + " (" + device.api_name + ")", combo_id);
        if (device.index == pending_input_) {
            selected_combo_id = combo_id;
        }
        ++combo_id;
    }
    input_combo_.setSelectedId(selected_combo_id, juce::dontSendNotification);

    output_combo_.clear(juce::dontSendNotification);
    combo_id = 1;
    selected_combo_id = 0;
    for (const auto& device: output_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        output_combo_.addItem(device.name + " (" + device.api_name + ")", combo_id);
        if (device.index == pending_output_) {
            selected_combo_id = combo_id;
        }
        ++combo_id;
    }
    output_combo_.setSelectedId(selected_combo_id, juce::dontSendNotification);

    updating_from_client_ = false;
    populate_input_channel_combo();
}

void JuceMixerComponent::populate_input_channel_combo() {
    updating_from_client_ = true;
    input_channel_combo_.clear(juce::dontSendNotification);
    const int channel_count = max_input_channels_for(pending_input_);
    pending_input_channel_ = std::clamp(pending_input_channel_, 0, channel_count - 1);
    for (int channel = 0; channel < channel_count; ++channel) {
        input_channel_combo_.addItem(juce::String(channel + 1), channel + 1);
    }
    input_channel_combo_.setSelectedId(pending_input_channel_ + 1,
                                       juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_buffer_combo() {
    updating_from_client_ = true;
    buffer_combo_.clear(juce::dontSendNotification);
    for (const int frames: {96, 120, 128, 240, 256}) {
        buffer_combo_.addItem(juce::String(frames), frames);
    }
    buffer_combo_.setSelectedId(pending_buffer_frames_, juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_opus_packet_combo() {
    updating_from_client_ = true;
    opus_packet_combo_.clear(juce::dontSendNotification);
    const int frame_options[] = {opus_network_clock::LOW_LATENCY_FRAME_COUNT,
                                 opus_network_clock::FAST_FRAME_COUNT,
                                 opus_network_clock::BALANCED_FRAME_COUNT,
                                 opus_network_clock::STABLE_FRAME_COUNT};
    for (const int frames: frame_options) {
        opus_packet_combo_.addItem(juce::String(frames) + " " + opus_packet_label(frames),
                                   frames);
    }
    opus_packet_combo_.setSelectedId(pending_opus_frames_per_packet_,
                                     juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::populate_redundancy_combo() {
    updating_from_client_ = true;
    redundancy_combo_.clear(juce::dontSendNotification);
    redundancy_combo_.addItem("Auto", 1);
    redundancy_combo_.addItem("Off", 2);
    for (int depth = 1; depth <= MAX_OPUS_REDUNDANCY_DEPTH_PACKETS; ++depth) {
        redundancy_combo_.addItem(juce::String(depth) + " previous", depth + 2);
    }
    const int current_depth = client_.get_opus_redundancy_depth_setting();
    redundancy_combo_.setSelectedId(current_depth == OPUS_REDUNDANCY_DEPTH_AUTO
                                        ? 1
                                        : current_depth + 2,
                                    juce::dontSendNotification);
    updating_from_client_ = false;
}

void JuceMixerComponent::apply_audio_settings() {
    const bool restart_needed = pending_stream_restart_needed();
    client_.set_audio_api_filter(selected_api_name().toStdString());
    client_.set_input_device(pending_input_);
    client_.set_input_channel_index(pending_input_channel_);
    client_.set_output_device(pending_output_);
    client_.set_requested_frames_per_buffer(pending_buffer_frames_);
    client_.set_opus_network_frame_count(pending_opus_frames_per_packet_);
    client_.save_audio_device_preferences();

    if (client_.is_audio_stream_active() && restart_needed) {
        client_.swap_audio_devices(pending_input_, pending_output_);
    }
}

void JuceMixerComponent::start_or_stop_audio() {
    if (client_.is_audio_stream_active()) {
        client_.stop_audio_stream();
        return;
    }

    if (pending_input_ == AudioStream::NO_DEVICE || pending_output_ == AudioStream::NO_DEVICE) {
        set_device_status("Select input and output devices before starting audio");
        return;
    }

    apply_audio_settings();
    AudioStream::AudioConfig config = client_.get_audio_config();
    if (!client_.start_audio_stream(pending_input_, pending_output_, config)) {
        set_device_status(AudioStream::get_last_error());
    }
}

void JuceMixerComponent::reset_audio_path() {
    client_.reset_audio_path();
    refresh_audio_device_controls();
}

void JuceMixerComponent::commit_metronome_bpm() {
    const float bpm = std::max(1.0F, bpm_editor_.getText().getFloatValue());
    client_.commit_metronome_bpm(bpm);
}

void JuceMixerComponent::load_wav_file() {
    const auto path = wav_path_editor_.getText().toStdString();
    if (!path.empty() && !client_.load_wav_file(path)) {
        set_device_status("Could not load WAV file");
    }
}

void JuceMixerComponent::apply_selected_api_to_pending_devices(int old_api_index) {
    if (old_api_index == selected_api_index_ || selected_api_index_ < 0) {
        return;
    }

    const juce::String api_name = selected_api_name();
    for (const auto& device: input_devices_) {
        if (device.api_name == api_name.toStdString()) {
            pending_input_ = device.index;
            pending_input_channel_ = 0;
            break;
        }
    }
    for (const auto& device: output_devices_) {
        if (device.api_name == api_name.toStdString()) {
            pending_output_ = device.index;
            break;
        }
    }
}

juce::String JuceMixerComponent::selected_api_name() const {
    if (selected_api_index_ < 0) {
        return "All";
    }
    for (const auto& api: available_apis_) {
        if (api.index == selected_api_index_) {
            return api.name;
        }
    }
    return "All";
}

int JuceMixerComponent::api_index_for_name(const std::string& api_name) const {
    if (api_name.empty() || api_name == "All") {
        return -1;
    }
    for (const auto& api: available_apis_) {
        if (api.name == api_name) {
            return api.index;
        }
    }
    return -1;
}

int JuceMixerComponent::max_input_channels_for(AudioStream::DeviceIndex device_index) const {
    const auto active_device_info = client_.get_device_info();
    if (device_index == client_.get_selected_input_device()) {
        return std::max(active_device_info.input_channels, 1);
    }
    for (const auto& device: input_devices_) {
        if (device.index == device_index) {
            return std::max(device.max_input_channels, 1);
        }
    }
    return 1;
}

AudioStream::DeviceIndex JuceMixerComponent::selected_input_device() const {
    const auto api_name = selected_api_name();
    const int selected_id = input_combo_.getSelectedId();
    int combo_id = 1;
    for (const auto& device: input_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        if (combo_id == selected_id) {
            return device.index;
        }
        ++combo_id;
    }
    return AudioStream::NO_DEVICE;
}

AudioStream::DeviceIndex JuceMixerComponent::selected_output_device() const {
    const auto api_name = selected_api_name();
    const int selected_id = output_combo_.getSelectedId();
    int combo_id = 1;
    for (const auto& device: output_devices_) {
        if (api_name != "All" && device.api_name != api_name.toStdString()) {
            continue;
        }
        if (combo_id == selected_id) {
            return device.index;
        }
        ++combo_id;
    }
    return AudioStream::NO_DEVICE;
}

bool JuceMixerComponent::has_pending_audio_changes() const {
    return pending_stream_restart_needed() ||
           pending_opus_frames_per_packet_ != client_.get_opus_network_frame_count() ||
           selected_api_name().toStdString() != client_.get_audio_api_filter();
}

bool JuceMixerComponent::pending_stream_restart_needed() const {
    return pending_input_ != client_.get_selected_input_device() ||
           pending_output_ != client_.get_selected_output_device() ||
           pending_input_channel_ != client_.get_input_channel_index() ||
           pending_buffer_frames_ != client_.get_audio_config().frames_per_buffer;
}

void JuceMixerComponent::set_device_status(const juce::String& text) {
    device_status_label_.setText(text, juce::dontSendNotification);
}
