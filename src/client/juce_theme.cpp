#include "juce_theme.h"

#include <algorithm>

namespace juce_theme {
namespace colour {
juce::Colour background() { return juce::Colour{0xff121315}; }
juce::Colour panel_top() { return juce::Colour{0xff222426}; }
juce::Colour panel_bottom() { return juce::Colour{0xff191b1d}; }
juce::Colour row() { return juce::Colour{0xff222426}; }
juce::Colour row_hover() { return juce::Colour{0xff232528}; }
juce::Colour border() { return juce::Colour{0xff343639}; }
juce::Colour border_soft() { return juce::Colour{0xff2c2e31}; }
juce::Colour text() { return juce::Colour{0xffe7ebee}; }
juce::Colour text_dim() { return juce::Colour{0xffaab3ba}; }
juce::Colour text_faint() { return juce::Colour{0xff79838b}; }
juce::Colour accent() { return juce::Colour{0xfffd943c}; }
juce::Colour accent_hi() { return juce::Colour{0xfffc8c3c}; }
juce::Colour success() { return juce::Colour{0xff7ccb4c}; }
juce::Colour warning() { return juce::Colour{0xfffd943c}; }
juce::Colour danger() { return juce::Colour{0xffd9574f}; }
} // namespace colour

namespace {
juce::Rectangle<float> control_area(int width, int height) {
    return juce::Rectangle<float>{0.5f, 0.5f, static_cast<float>(width) - 1.0f,
                                  static_cast<float>(height) - 1.0f};
}

juce::Colour control_border(bool enabled, bool highlighted, bool active) {
    if (!enabled) {
        return colour::border_soft();
    }
    if (active) {
        return colour::accent();
    }
    return highlighted ? colour::accent().withAlpha(0.75f) : colour::border();
}
} // namespace

juce::Font font(float size, bool bold) {
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold
                                                   : juce::Font::plain));
}

void paint_panel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    auto area = bounds.toFloat().reduced(0.5f);
    g.setColour(colour::panel_bottom());
    g.fillRect(area);
    g.setColour(colour::border().withAlpha(0.78f));
    g.drawRect(area, 1.0f);
}

void style_label(juce::Label& label, juce::Colour text_colour, float size, bool bold) {
    label.setFont(font(size, bold));
    label.setColour(juce::Label::textColourId, text_colour);
    label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}

void style_editor(juce::TextEditor& editor) {
    editor.setFont(font(13.0f));
    editor.setColour(juce::TextEditor::textColourId, colour::text());
    editor.setColour(juce::TextEditor::highlightColourId, colour::accent().withAlpha(0.45f));
    editor.setColour(juce::TextEditor::highlightedTextColourId, colour::text());
    editor.setColour(juce::TextEditor::backgroundColourId,
                     juce::Colours::transparentBlack);
    editor.setColour(juce::TextEditor::outlineColourId, colour::border());
    editor.setColour(juce::TextEditor::focusedOutlineColourId, colour::accent());
}

LookAndFeel::LookAndFeel()
    : juce::LookAndFeel_V4(juce::LookAndFeel_V4::ColourScheme{
          colour::background(), colour::row(), colour::panel_top(), colour::border(),
          colour::text(), colour::row(), colour::text(), colour::accent(),
          colour::text()}) {
    setColour(juce::Label::textColourId, colour::text());
    setColour(juce::TextButton::textColourOffId, colour::text());
    setColour(juce::TextButton::textColourOnId, colour::text());
    setColour(juce::TextButton::buttonColourId, colour::row());
    setColour(juce::TextButton::buttonOnColourId, colour::accent());
    setColour(juce::ComboBox::backgroundColourId, colour::row());
    setColour(juce::ComboBox::textColourId, colour::text());
    setColour(juce::ComboBox::outlineColourId, colour::border());
    setColour(juce::PopupMenu::backgroundColourId, colour::panel_top());
    setColour(juce::PopupMenu::textColourId, colour::text());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, colour::row_hover());
    setColour(juce::PopupMenu::highlightedTextColourId, colour::text());
    setColour(juce::Slider::trackColourId, colour::border_soft());
    setColour(juce::Slider::thumbColourId, colour::accent_hi());
    setColour(juce::Slider::textBoxTextColourId, colour::text_dim());
    setColour(juce::Slider::textBoxBackgroundColourId, colour::row());
    setColour(juce::Slider::textBoxOutlineColourId, colour::border());
    setColour(juce::Slider::textBoxHighlightColourId, colour::accent().withAlpha(0.45f));
    setColour(juce::TextEditor::textColourId, colour::text());
    setColour(juce::TextEditor::backgroundColourId, colour::row());
    setColour(juce::TextEditor::outlineColourId, colour::border());
    setColour(juce::TextEditor::focusedOutlineColourId, colour::accent());
    setColour(juce::ScrollBar::thumbColourId, colour::text_faint().withAlpha(0.45f));
}

juce::Font LookAndFeel::getTextButtonFont(juce::TextButton&, int button_height) {
    return font(std::min(14.0f, static_cast<float>(button_height) * 0.45f), true);
}

juce::Font LookAndFeel::getComboBoxFont(juce::ComboBox&) {
    return font(13.0f);
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                       const juce::Colour&, bool highlighted,
                                       bool down) {
    auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const bool enabled = button.isEnabled();
    const bool active = button.getToggleState();
    auto fill = active ? juce::Colour{0xff2a241f}
                       : (highlighted ? colour::row_hover() : colour::row());
    if (!enabled) {
        fill = juce::Colour{0xff17191b};
    } else if (down) {
        fill = juce::Colour{0xff191a1c};
    }
    g.setColour(fill);
    g.fillRoundedRectangle(area, radius);
    g.setColour(control_border(enabled, highlighted, active));
    g.drawRoundedRectangle(area, radius, 1.0f);
}

void LookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                   bool highlighted, bool down) {
    auto bounds = button.getLocalBounds();
    auto tick = bounds.removeFromLeft(22).withSizeKeepingCentre(16, 16).toFloat();
    const bool enabled = button.isEnabled();
    const bool active = button.getToggleState();

    g.setColour(active ? colour::accent().withAlpha(enabled ? 0.28f : 0.12f)
                       : colour::row());
    g.fillRoundedRectangle(tick, 3.0f);
    g.setColour(control_border(enabled, highlighted || down, active));
    g.drawRoundedRectangle(tick, 3.0f, 1.0f);
    if (active) {
        juce::Path check;
        check.startNewSubPath(tick.getX() + 3.5f, tick.getCentreY());
        check.lineTo(tick.getCentreX() - 1.0f, tick.getBottom() - 4.0f);
        check.lineTo(tick.getRight() - 3.0f, tick.getY() + 4.0f);
        g.setColour(enabled ? colour::accent_hi() : colour::text_faint());
        g.strokePath(check, juce::PathStrokeType(1.8f));
    }

    g.setColour(enabled ? colour::text_dim() : colour::text_faint());
    g.setFont(font(13.0f));
    g.drawFittedText(button.getButtonText(), bounds.reduced(4, 0),
                     juce::Justification::centredLeft, 1);
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int width, int height,
                               bool is_button_down, int, int, int, int,
                               juce::ComboBox& box) {
    auto area = control_area(width, height);
    g.setColour(is_button_down || box.isMouseOverOrDragging() ? colour::row_hover()
                                                               : colour::row());
    g.fillRoundedRectangle(area, radius);
    g.setColour(control_border(box.isEnabled(), box.isMouseOverOrDragging(),
                               box.hasKeyboardFocus(false)));
    g.drawRoundedRectangle(area, radius, 1.0f);

    auto arrow = juce::Rectangle<float>{static_cast<float>(width - 22),
                                        static_cast<float>(height / 2 - 3), 9.0f,
                                        6.0f};
    juce::Path path;
    path.startNewSubPath(arrow.getX(), arrow.getY());
    path.lineTo(arrow.getCentreX(), arrow.getBottom());
    path.lineTo(arrow.getRight(), arrow.getY());
    g.setColour((is_button_down ? colour::accent_hi() : colour::text_dim())
                    .withAlpha(box.isEnabled() ? 1.0f : 0.45f));
    g.strokePath(path, juce::PathStrokeType(1.6f));
}

void LookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label) {
    label.setBounds(10, 1, box.getWidth() - 32, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId,
                    box.isEnabled() ? colour::text() : colour::text_faint());
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                           juce::TextEditor&) {
    auto area = control_area(width, height);
    g.setColour(colour::row());
    g.fillRoundedRectangle(area, radius);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                        juce::TextEditor& editor) {
    auto area = control_area(width, height);
    g.setColour(editor.hasKeyboardFocus(true) ? colour::accent() : colour::border());
    g.drawRoundedRectangle(area, radius, 1.0f);
}

void LookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width,
                                   int height, float slider_pos,
                                   float min_slider_pos, float max_slider_pos,
                                   juce::Slider::SliderStyle style,
                                   juce::Slider& slider) {
    if (style != juce::Slider::LinearHorizontal) {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, slider_pos,
                                               min_slider_pos, max_slider_pos, style,
                                               slider);
        return;
    }

    const auto centre_y = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    auto track = juce::Rectangle<float>{static_cast<float>(x) + 4.0f, centre_y - 2.0f,
                                        static_cast<float>(width) - 8.0f, 4.0f};
    g.setColour(colour::border_soft());
    g.fillRoundedRectangle(track, 2.0f);
    const auto left = std::min(min_slider_pos, slider_pos);
    const auto right = std::max(min_slider_pos, slider_pos);
    auto fill = juce::Rectangle<float>{left, track.getY(), right - left, track.getHeight()};
    g.setColour(slider.isEnabled() ? colour::accent_hi() : colour::text_faint());
    g.fillRoundedRectangle(fill, 2.0f);
    g.setColour(colour::text());
    g.fillEllipse(slider_pos - 5.0f, centre_y - 5.0f, 10.0f, 10.0f);
}

void LookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width,
                                   int height, float slider_pos,
                                   float rotary_start_angle,
                                   float rotary_end_angle, juce::Slider& slider) {
    const auto size = static_cast<float>(std::min(width, height)) - 8.0f;
    auto area = juce::Rectangle<float>{static_cast<float>(x), static_cast<float>(y),
                                       static_cast<float>(width),
                                       static_cast<float>(height)}
                    .withSizeKeepingCentre(size, size);
    g.setColour(colour::row_hover());
    g.fillEllipse(area);
    g.setColour(colour::border());
    g.drawEllipse(area, 1.0f);

    const auto centre = area.getCentre();
    const auto radius_value = area.getWidth() * 0.5f;
    for (int tick = 0; tick <= 10; ++tick) {
        const float pos = static_cast<float>(tick) / 10.0f;
        const float angle =
            rotary_start_angle + pos * (rotary_end_angle - rotary_start_angle) -
            juce::MathConstants<float>::halfPi;
        const auto inner =
            centre + juce::Point<float>{std::cos(angle), std::sin(angle)} *
                         (radius_value + 2.0f);
        const auto outer =
            centre + juce::Point<float>{std::cos(angle), std::sin(angle)} *
                         (radius_value + 5.0f);
        g.setColour(tick == 5 ? colour::text_faint() : colour::border());
        g.drawLine({inner, outer}, tick == 5 ? 1.1f : 0.8f);
    }

    const auto angle =
        rotary_start_angle + slider_pos * (rotary_end_angle - rotary_start_angle);
    juce::Path marker;
    marker.startNewSubPath(centre);
    marker.lineTo(centre.x + std::cos(angle - juce::MathConstants<float>::halfPi) *
                                 area.getWidth() * 0.38f,
                  centre.y + std::sin(angle - juce::MathConstants<float>::halfPi) *
                                 area.getHeight() * 0.38f);
    g.setColour(slider.isEnabled() ? colour::accent_hi() : colour::text_faint());
    g.strokePath(marker, juce::PathStrokeType(2.0f));
}
} // namespace juce_theme
