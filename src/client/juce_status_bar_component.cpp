#include "juce_status_bar_component.h"

namespace {
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
}  // namespace

JuceStatusBarComponent::JuceStatusBarComponent() {
    status_label_.setFont(juce::Font(juce::FontOptions(16.0F, juce::Font::bold)));
    transport_label_.setFont(juce::Font(juce::FontOptions(13.0F)));
    addAndMakeVisible(status_label_);
    addAndMakeVisible(transport_label_);
}

void JuceStatusBarComponent::refresh(
    const ClientAppFacade& client, const JuceClientStartupOptions& startup_options,
    const juce::String& connection_status) {
    const auto participants = client.get_participant_info();
    const auto device_info = client.get_device_info();
    const auto connected_port = client.get_server_port();
    const juce::String server_text =
        connected_port == 0
            ? juce::String(startup_options.server_address) + ":" +
                  juce::String(static_cast<int>(startup_options.server_port))
            : juce::String(client.get_server_address()) + ":" +
                  juce::String(static_cast<int>(connected_port));

    status_label_.setText(
        server_text + " | " + connection_status + " | Room " +
            juce::String(client.get_room_id()) + " | RTT " +
            juce::String(client.get_rtt_ms(), 1) + " ms | Users " +
            juce::String(static_cast<int>(participants.size())) + " | RX " +
            format_bytes(client.get_total_bytes_rx()) + " | TX " +
            format_bytes(client.get_total_bytes_tx()),
        juce::dontSendNotification);

    transport_label_.setText(
        juce::String(client.is_audio_stream_active() ? "Audio running" : "Audio stopped") +
            " | " + device_info.input_api + " | In " + device_info.input_device_name +
            " ch " + juce::String(device_info.input_channel_index + 1) + " | Out " +
            device_info.output_device_name,
        juce::dontSendNotification);
}

void JuceStatusBarComponent::resized() {
    auto area = getLocalBounds();
    status_label_.setBounds(area.removeFromTop(24));
    transport_label_.setBounds(area);
}
