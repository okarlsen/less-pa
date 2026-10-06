#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The palette, sampled from the SGTM logo rather than picked by eye: the
// accent, and three of the four meter colours, are the literal fill colours
// used in the wordmark, so the UI and the logo can't drift apart.
//
// Exposed outside the LookAndFeel because two callers need the colours
// directly rather than through a JUCE colour ID:
//   - LevelMeterComponent, which is custom-drawn and has no stock IDs to map
//     its four semantic states onto.
//   - the help dialog's TextEditor, which lives in a DialogWindow (a separate
//     top-level window, so it does not inherit the editor's LookAndFeel) and
//     can outlive the editor, which rules out pointing it at ours.
namespace LessPAColours
{
    inline const juce::Colour windowBackground { 0xff14161a };
    inline const juce::Colour panel            { 0xff1c1f24 };
    inline const juce::Colour border           { 0xff2a2e35 };
    inline const juce::Colour primaryText      { 0xfff2f3f5 };
    inline const juce::Colour secondaryText    { 0xff9ba1ac };
    inline const juce::Colour accent           { 0xff39c7f4 }; // brand cyan (the logo's "T")
    inline const juce::Colour accentHover      { 0xff5dd3f7 };
    inline const juce::Colour controlSurface   { 0xff23262c };

    // Meter colours keep their existing semantics. Caution/safe/suppression
    // are brand orange/green/mauve; danger stays a designed red because the
    // brand has none. Suppression is deliberately the one brand hue *not*
    // used as the accent, so "suppression is working" never reads as "this is
    // the clickable thing".
    inline const juce::Colour meterDanger      { 0xffe85b5b };
    inline const juce::Colour meterCaution     { 0xfff89e43 };
    inline const juce::Colour meterSafe        { 0xff6ebe44 };
    inline const juce::Colour meterSuppression { 0xffc895c3 };
}

// Flat, modern-minimal styling: no gradients, no bevels, no drop shadows
// anywhere. That absence is the whole signature -- LookAndFeel_V4's stock
// controls are shaded, and simply recolouring them would still look like a
// stock JUCE panel.
//
// Deliberately does not override drawRotarySlider: every slider in this
// plugin is LinearHorizontal (see PluginEditor.h), so a rotary override would
// be untested code that only ever runs if someone changes a slider style.
class LessPALookAndFeel : public juce::LookAndFeel_V4
{
public:
    LessPALookAndFeel();

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    void drawLabel(juce::Graphics&, juce::Label&) override;
    juce::Font getLabelFont(juce::Label&) override;

    // Overriding drawComboBox only themes the *closed* box -- the dropdown is
    // a separate PopupMenu draw path. Without these two the combos would pop
    // open into a stock grey menu.
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                           bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // Both halves of the tooltip are overridden, and both have to be.
    // LookAndFeel_V2::getTooltipBounds lays the text out at up to 400px wide
    // and only *then* constrains the result into the parent -- so in a
    // window narrower than that, a long tip is measured too wide and then
    // squeezed, losing text off the end. LookAndFeel_V4
    // overrides only drawTooltip, so that behaviour would be inherited.
    //
    // drawTooltip is not optional here: it re-lays the text out itself, again
    // at the stock 400px, so overriding the bounds alone would give a
    // correctly-sized box with clipped text inside it. Both go through
    // layoutTooltip() in the .cpp at the same wrap width, which is what makes
    // the measured box and the drawn text agree.
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText,
                                          juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;
};
