#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace juce_theme {
namespace colour {
juce::Colour background();
juce::Colour panel_top();
juce::Colour panel_bottom();
juce::Colour row();
juce::Colour row_hover();
juce::Colour border();
juce::Colour border_soft();
juce::Colour text();
juce::Colour text_dim();
juce::Colour text_faint();
juce::Colour accent();
juce::Colour accent_hi();
juce::Colour success();
juce::Colour warning();
juce::Colour danger();
} // namespace colour

constexpr float radius = 2.0f;
constexpr int pad = 14;
constexpr int gap = 10;
constexpr int row_height = 28;

juce::Font font(float size, bool bold = false);
void paint_panel(juce::Graphics& g, juce::Rectangle<int> bounds);
void style_label(juce::Label& label, juce::Colour text_colour, float size,
                 bool bold = false);
void style_editor(juce::TextEditor& editor);

class LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();

    juce::Font getTextButtonFont(juce::TextButton&, int button_height) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& background_colour,
                              bool highlighted, bool down) override;
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics& g, int width, int height, bool is_button_down,
                      int button_x, int button_y, int button_w, int button_h,
                      juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;
    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& editor) override;
    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& editor) override;
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float slider_pos, float min_slider_pos,
                          float max_slider_pos, juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float slider_pos, float rotary_start_angle,
                          float rotary_end_angle, juce::Slider& slider) override;
};
} // namespace juce_theme
