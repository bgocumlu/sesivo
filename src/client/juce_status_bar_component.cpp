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

}  // namespace

JuceStatusBarComponent::JuceStatusBarComponent() { setOpaque(false); }

void JuceStatusBarComponent::refresh(
    const ClientAppFacade& client, const JuceClientStartupOptions& startup_options,
    const juce::String& connection_status) {
    const auto participants = client.get_participant_info();
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

    repaint();
}

void JuceStatusBarComponent::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds();
    juce_theme::paint_panel(g, bounds);

    auto area = bounds.reduced(18, 0);
    auto brand = area.removeFromLeft(126);
    juce_theme::draw_wordmark(g, brand.reduced(0, 12), 27.0F);

    draw_metric(g, area.removeFromLeft(170), "Room", room_text_);
    draw_metric(g, area.removeFromLeft(146), "State", state_text_,
                joined_ ? juce_theme::colour::success()
                        : juce_theme::colour::warning());
    draw_metric(g, area.removeFromLeft(82), "Users", users_text_);
    draw_metric(g, area.removeFromLeft(190), "Server", server_text_);
    draw_metric(g, area.removeFromLeft(112), "RTT", rtt_text_);
    draw_metric(g, area.removeFromLeft(116), "RX", rx_text_);
    draw_metric(g, area.removeFromLeft(116), "TX", tx_text_);
}

void JuceStatusBarComponent::resized() {}
