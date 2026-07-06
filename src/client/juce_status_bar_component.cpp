#include "juce_status_bar_component.h"

#include "juce_theme.h"

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

void draw_metric(juce::Graphics& g, juce::Rectangle<int> bounds,
                 const juce::String& label, const juce::String& value,
                 juce::Colour value_colour = juce_theme::colour::text()) {
    g.setColour(juce_theme::colour::border_soft());
    g.drawVerticalLine(bounds.getX(), bounds.getY() + 4.0F,
                       bounds.getBottom() - 4.0F);

    auto text = bounds.reduced(18, 8);
    g.setFont(juce_theme::font(12.0F));
    g.setColour(juce_theme::colour::text_faint());
    g.drawFittedText(label, text.removeFromTop(17),
                     juce::Justification::centredLeft, 1);
    g.setFont(juce_theme::font(15.0F, true));
    g.setColour(value_colour);
    g.drawFittedText(value, text, juce::Justification::centredLeft, 1);
}

void draw_status_dot(juce::Graphics& g, juce::Point<float> centre,
                     juce::Colour colour) {
    g.setColour(colour.withAlpha(0.22F));
    g.fillEllipse(centre.x - 6.0F, centre.y - 6.0F, 12.0F, 12.0F);
    g.setColour(colour);
    g.fillEllipse(centre.x - 3.5F, centre.y - 3.5F, 7.0F, 7.0F);
}
}  // namespace

JuceStatusBarComponent::JuceStatusBarComponent() { setOpaque(false); }

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
    juce::String effective_connection_status = connection_status;
    if (connected_port != 0 && client.is_join_confirmed()) {
        effective_connection_status = "Joined";
    } else if (connected_port != 0 && connection_status == "Join sent") {
        effective_connection_status = "Waiting for join ack";
    }
    const bool joined = connected_port != 0 && client.is_join_confirmed();

    server_text_ = server_text;
    room_text_ = client.get_room_id().empty() ? "-" : juce::String(client.get_room_id());
    state_text_ = effective_connection_status;
    users_text_ = juce::String(static_cast<int>(participants.size() +
                                                (joined ? size_t{1} : size_t{0})));
    rtt_text_ = juce::String(client.get_rtt_ms(), 1) + " ms";
    rx_text_ = format_bytes(client.get_total_bytes_rx());
    tx_text_ = format_bytes(client.get_total_bytes_tx());
    joined_ = joined;
    audio_running_ = client.is_audio_stream_active();
    audio_text_ = audio_running_ ? "Audio running" : "Audio stopped";
    device_text_ = juce::String(device_info.input_api) + "  In " +
                   juce::String(device_info.input_device_name) + " ch " +
                   juce::String(device_info.input_channel_index + 1) + "  Out " +
                   juce::String(device_info.output_device_name);

    repaint();
}

void JuceStatusBarComponent::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds();
    juce_theme::paint_panel(g, bounds);

    auto area = bounds.reduced(18, 0);
    auto brand = area.removeFromLeft(126);
    juce::AttributedString logo;
    logo.setJustification(juce::Justification::centredLeft);
    const auto logo_font = juce_theme::font(27.0F, true);
    logo.append("ses", logo_font, juce_theme::colour::text());
    logo.append("ivo", logo_font, juce_theme::colour::accent_hi());
    logo.draw(g, brand.reduced(0, 12).toFloat());

    draw_metric(g, area.removeFromLeft(170), "Room", room_text_);
    draw_metric(g, area.removeFromLeft(174), "State", state_text_,
                joined_ ? juce_theme::colour::success()
                        : juce_theme::colour::warning());
    draw_metric(g, area.removeFromLeft(112), "Users", users_text_);
    draw_metric(g, area.removeFromLeft(166), "Server", server_text_);
    draw_metric(g, area.removeFromLeft(112), "RTT", rtt_text_);
    draw_metric(g, area.removeFromLeft(116), "RX", rx_text_);
    draw_metric(g, area.removeFromLeft(116), "TX", tx_text_);

    auto device_area = area.reduced(18, 8);
    draw_status_dot(g, {static_cast<float>(device_area.getX() + 5),
                        static_cast<float>(device_area.getY() + 12)},
                    audio_running_ ? juce_theme::colour::success()
                                   : juce_theme::colour::text_faint());
    device_area.removeFromLeft(18);
    g.setFont(juce_theme::font(13.0F, true));
    g.setColour(audio_running_ ? juce_theme::colour::text()
                               : juce_theme::colour::text_dim());
    g.drawFittedText(audio_text_, device_area.removeFromTop(18),
                     juce::Justification::centredLeft, 1);
    g.setFont(juce_theme::font(12.0F));
    g.setColour(juce_theme::colour::text_faint());
    g.drawFittedText(device_text_, device_area, juce::Justification::centredLeft, 1);
}

void JuceStatusBarComponent::resized() {}
