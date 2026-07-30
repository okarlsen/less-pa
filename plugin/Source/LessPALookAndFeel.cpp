#include "LessPALookAndFeel.h"

#include <cmath>

namespace {

// Tooltip metrics. The font size matches JUCE's stock tooltip; the wrap width
// is the part that differs -- 240 design px sits comfortably inside the 340px
// editor the TooltipWindow is parented to, so long text wraps onto a second
// line instead of running past the window edge and being clipped.
constexpr float tooltipFontSize = 13.0f;
constexpr int tooltipMaxWidth = 240;
constexpr int tooltipPaddingX = 14;
constexpr int tooltipPaddingY = 6;

// Plain createLayout rather than createLayoutWithBalancedLineLengths (which is
// what JUCE uses): the balanced version picks a width by iterating, so laying
// the same string out a second time at the width it returned can break
// differently. getTooltipBounds and drawTooltip are two separate calls that
// must agree exactly on the line breaks, so a deterministic layout matters
// more here than prettier line balance on the rare two-line tip.
juce::TextLayout layoutTooltip(const juce::LookAndFeel& lookAndFeel, const juce::String& text,
                               juce::Colour colour, float maxWidth)
{
    juce::AttributedString attributed;
    attributed.setJustification(juce::Justification::centred);
    attributed.append(text, lookAndFeel.withDefaultMetrics(juce::FontOptions(tooltipFontSize,
                                                                            juce::Font::bold)),
                      colour);

    juce::TextLayout layout;
    layout.createLayout(attributed, maxWidth);
    return layout;
}

} // namespace

LessPALookAndFeel::LessPALookAndFeel()
    // The base ColourScheme is set as well as the individual IDs below,
    // because it is what every widget this plugin does *not* use falls back
    // to -- scrollbars in the help dialog, alert boxes, the resizer corner.
    // Without it those would still render against LookAndFeel_V4's stock dark
    // grey and look like a different application.
    : juce::LookAndFeel_V4({ 0xff14161a,   // windowBackground
                             0xff1c1f24,   // widgetBackground
                             0xff1c1f24,   // menuBackground
                             0xff2a2e35,   // outline
                             0xfff2f3f5,   // defaultText
                             0xff23262c,   // defaultFill
                             0xff14161a,   // highlightedText (dark-on-accent)
                             0xff39c7f4,   // highlightedFill
                             0xfff2f3f5 }) // menuText
{
    setColour(juce::ResizableWindow::backgroundColourId, LessPAColours::windowBackground);
    setColour(juce::DocumentWindow::textColourId, LessPAColours::primaryText);

    // Labels default to transparent backgrounds and no outline. This matters
    // beyond aesthetics: ComboBox and Slider both build their internal text
    // labels by copying colours off themselves, and a non-transparent default
    // would paint an opaque rectangle over the box we just drew.
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textColourId, LessPAColours::primaryText);
    setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::backgroundWhenEditingColourId, LessPAColours::controlSurface);
    setColour(juce::Label::textWhenEditingColourId, LessPAColours::primaryText);
    setColour(juce::Label::outlineWhenEditingColourId, LessPAColours::accent);

    setColour(juce::Slider::backgroundColourId, LessPAColours::controlSurface); // unfilled track
    setColour(juce::Slider::trackColourId, LessPAColours::accent);              // filled track
    setColour(juce::Slider::thumbColourId, LessPAColours::primaryText);
    setColour(juce::Slider::textBoxTextColourId, LessPAColours::primaryText);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxHighlightColourId, LessPAColours::accent.withAlpha(0.35f));

    setColour(juce::ComboBox::backgroundColourId, LessPAColours::controlSurface);
    setColour(juce::ComboBox::textColourId, LessPAColours::primaryText);
    setColour(juce::ComboBox::outlineColourId, LessPAColours::border);
    setColour(juce::ComboBox::buttonColourId, LessPAColours::controlSurface);
    setColour(juce::ComboBox::arrowColourId, LessPAColours::secondaryText);
    setColour(juce::ComboBox::focusedOutlineColourId, LessPAColours::accent);

    setColour(juce::PopupMenu::backgroundColourId, LessPAColours::panel);
    setColour(juce::PopupMenu::textColourId, LessPAColours::primaryText);
    setColour(juce::PopupMenu::headerTextColourId, LessPAColours::secondaryText);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, LessPAColours::accent);
    setColour(juce::PopupMenu::highlightedTextColourId, LessPAColours::windowBackground);

    setColour(juce::ToggleButton::textColourId, LessPAColours::primaryText);
    setColour(juce::ToggleButton::tickColourId, LessPAColours::accent);
    setColour(juce::ToggleButton::tickDisabledColourId, LessPAColours::border);

    setColour(juce::TextButton::buttonColourId, LessPAColours::controlSurface);
    setColour(juce::TextButton::buttonOnColourId, LessPAColours::accent);
    setColour(juce::TextButton::textColourOffId, LessPAColours::secondaryText);
    setColour(juce::TextButton::textColourOnId, LessPAColours::windowBackground);

    // The help dialog's TextEditor, and any editable slider value box.
    setColour(juce::TextEditor::backgroundColourId, LessPAColours::panel);
    setColour(juce::TextEditor::textColourId, LessPAColours::primaryText);
    setColour(juce::TextEditor::highlightColourId, LessPAColours::accent.withAlpha(0.35f));
    setColour(juce::TextEditor::highlightedTextColourId, LessPAColours::primaryText);
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, LessPAColours::accent);
    setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, LessPAColours::accent);

    setColour(juce::TooltipWindow::backgroundColourId, LessPAColours::controlSurface);
    setColour(juce::TooltipWindow::textColourId, LessPAColours::primaryText);
    setColour(juce::TooltipWindow::outlineColourId, LessPAColours::border);

    setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, LessPAColours::border);
    setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    setColour(juce::AlertWindow::backgroundColourId, LessPAColours::panel);
    setColour(juce::AlertWindow::textColourId, LessPAColours::primaryText);
    setColour(juce::AlertWindow::outlineColourId, LessPAColours::border);
}

void LessPALookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPos, float minSliderPos, float maxSliderPos,
                                         juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // Only the horizontal single-value case is exercised here (every slider in
    // this plugin is LinearHorizontal). Anything else defers to the base class
    // rather than being drawn wrong by a path never designed for it -- a
    // two-value slider would otherwise silently lose one of its thumbs.
    const bool isSingleValueHorizontal = slider.isHorizontal()
                                         && !slider.isBar()
                                         && style != juce::Slider::TwoValueHorizontal
                                         && style != juce::Slider::ThreeValueHorizontal;

    if (!isSingleValueHorizontal) {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                               minSliderPos, maxSliderPos, style, slider);
        return;
    }

    constexpr float trackThickness = 3.0f;
    const float centreY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const juce::Rectangle<float> track(static_cast<float>(x), centreY - trackThickness * 0.5f,
                                       static_cast<float>(width), trackThickness);

    g.setColour(slider.findColour(juce::Slider::backgroundColourId));
    g.fillRoundedRectangle(track, trackThickness * 0.5f);

    // jlimit rather than trusting sliderPos: it is clamped to the *thumb*
    // travel, which is inset from the track by the thumb radius, so at the
    // extremes it can land marginally outside this rectangle.
    const auto filled = track.withRight(juce::jlimit(track.getX(), track.getRight(), sliderPos));
    if (filled.getWidth() > 0.0f) {
        g.setColour(slider.findColour(juce::Slider::trackColourId));
        g.fillRoundedRectangle(filled, trackThickness * 0.5f);
    }

    // Flat filled circle -- no bevel, no gradient, no shadow.
    const auto thumbDiameter = static_cast<float>(getSliderThumbRadius(slider));
    g.setColour(slider.findColour(juce::Slider::thumbColourId));
    g.fillEllipse(juce::Rectangle<float>(thumbDiameter, thumbDiameter)
                      .withCentre({ sliderPos, centreY }));
}

void LessPALookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                     int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                                     juce::ComboBox& box)
{
    const juce::Rectangle<float> bounds(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    constexpr float corner = 4.0f;

    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(bounds, corner);

    // The only border drawn anywhere in this LookAndFeel. A combo is the one
    // control whose affordance depends on looking like a field you can open --
    // without an edge, a flat filled box next to a flat panel reads as a
    // static value readout.
    g.setColour(box.hasKeyboardFocus(false) ? box.findColour(juce::ComboBox::focusedOutlineColourId)
                                            : box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);

    // Hand-drawn slim chevron instead of the stock filled triangle. Placed to
    // match LookAndFeel_V4::positionComboBoxText, which reserves the rightmost
    // 30px of the box for it -- keeping inside that keeps the text clear of it.
    const juce::Rectangle<float> arrowZone(static_cast<float>(width) - 24.0f, 0.0f, 16.0f,
                                           static_cast<float>(height));
    constexpr float chevronHalfWidth = 4.0f;
    constexpr float chevronHeight = 3.0f;

    juce::Path chevron;
    chevron.startNewSubPath(arrowZone.getCentreX() - chevronHalfWidth,
                            arrowZone.getCentreY() - chevronHeight * 0.5f);
    chevron.lineTo(arrowZone.getCentreX(), arrowZone.getCentreY() + chevronHeight * 0.5f);
    chevron.lineTo(arrowZone.getCentreX() + chevronHalfWidth,
                   arrowZone.getCentreY() - chevronHeight * 0.5f);

    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 1.0f : 0.3f));
    g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

juce::Font LessPALookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    // Proportional to the box rather than a fixed size, so it follows the
    // editor's uniform window scale. LookAndFeel_V4's version clamps at 16pt,
    // which would stop scaling partway through the resize range.
    return juce::FontOptions(juce::jlimit(9.0f, 18.0f, static_cast<float>(box.getHeight()) * 0.46f));
}

void LessPALookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                         bool shouldDrawButtonAsHighlighted,
                                         bool /*shouldDrawButtonAsDown*/)
{
    // A pill switch, not LookAndFeel_V4's tick box. Both toggles here are
    // on/off switches over a running process rather than checkboxes in a list,
    // and the switch metaphor makes the current state readable at a glance --
    // which matters when the operator is checking it mid-show.
    // Proportional to the button rather than a literal 18px, so the switch
    // follows the editor's uniform window scale instead of staying pinned at
    // the default size while the text around it grows. 0.75 lands on the
    // designed 18px at the 24px row height resized() uses.
    const float pillHeight = juce::jlimit(12.0f, 28.0f, static_cast<float>(button.getHeight()) * 0.75f);
    const float pillWidth = pillHeight * (34.0f / 18.0f); // holds the designed 34x18 shape at any scale
    const juce::Rectangle<float> pill(0.0f,
                                      (static_cast<float>(button.getHeight()) - pillHeight) * 0.5f,
                                      pillWidth, pillHeight);

    const bool on = button.getToggleState();
    const float radius = pillHeight * 0.5f;

    // There is no stock colour ID meaning "switch track, off", so the palette
    // is used directly here rather than inventing a misleading mapping onto
    // tickDisabledColourId (which means *disabled*, not off).
    auto trackColour = on ? button.findColour(juce::ToggleButton::tickColourId)
                          : LessPAColours::controlSurface;
    if (shouldDrawButtonAsHighlighted)
        trackColour = on ? LessPAColours::accentHover : trackColour.brighter(0.12f);

    g.setColour(button.isEnabled() ? trackColour : trackColour.withMultipliedAlpha(0.5f));
    g.fillRoundedRectangle(pill, radius);

    if (!on) {
        // Off, the track is barely lighter than the panel behind it -- the
        // edge is what keeps the control from disappearing entirely.
        g.setColour(LessPAColours::border);
        g.drawRoundedRectangle(pill.reduced(0.5f), radius, 1.0f);
    }

    constexpr float knobInset = 2.0f;
    const float knobDiameter = pillHeight - knobInset * 2.0f;
    const float knobX = on ? pill.getRight() - knobInset - knobDiameter : pill.getX() + knobInset;

    g.setColour(on ? LessPAColours::windowBackground : LessPAColours::secondaryText);
    g.fillEllipse(knobX, pill.getY() + knobInset, knobDiameter, knobDiameter);

    auto textColour = button.findColour(juce::ToggleButton::textColourId);
    g.setColour(button.isEnabled() ? textColour : textColour.withMultipliedAlpha(0.5f));
    g.setFont(juce::FontOptions(juce::jlimit(9.0f, 18.0f,
                                             static_cast<float>(button.getHeight()) * 0.55f)));
    g.drawFittedText(button.getButtonText(),
                     button.getLocalBounds()
                         .withTrimmedLeft(juce::roundToInt(pillWidth) + 10)
                         .withTrimmedRight(2),
                     juce::Justification::centredLeft, 1);
}

void LessPALookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.fillAll(label.findColour(juce::Label::backgroundColourId));

    if (label.isBeingEdited())
        return; // a TextEditor is overlaid in this state and draws the text itself

    const float alpha = label.isEnabled() ? 1.0f : 0.5f;
    const juce::Font font(getLabelFont(label));

    g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
    g.setFont(font);

    const auto textArea = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
    g.drawFittedText(label.getText(), textArea, label.getJustificationType(),
                     juce::jmax(1, static_cast<int>(static_cast<float>(textArea.getHeight()) / font.getHeight())),
                     label.getMinimumHorizontalScale());

    // No outline pass. LookAndFeel_V2::drawLabel unconditionally ends with a
    // drawRect() in the outline colour; nothing in this UI wants a boxed
    // label, so the pass is dropped rather than relying on every label
    // remembering to keep Label::outlineColourId transparent.
}

juce::Font LessPALookAndFeel::getLabelFont(juce::Label& label)
{
    const auto font = label.getFont();

    // Slider value boxes are built by JUCE itself
    // (LookAndFeel::createSliderTextBox), so the editor never gets a handle to
    // call setFont() on them and they arrive here at the stock 15pt -- heavier
    // than the 12.5pt row label directly above. Sizing from the box's own
    // height also makes them track the window scale, which a literal size
    // would not. Every label the editor owns is sized explicitly in resized(),
    // so keying off "is this inside a Slider?" only catches the ones that
    // cannot be reached any other way.
    if (label.findParentComponentOfClass<juce::Slider>() != nullptr)
        return font.withHeight(juce::jlimit(9.0f, 18.0f,
                                            static_cast<float>(label.getHeight()) * 0.5f));

    return font;
}

void LessPALookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    const juce::Rectangle<float> bounds(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    constexpr float corner = 4.0f;

    g.setColour(findColour(juce::PopupMenu::backgroundColourId));
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(LessPAColours::border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);
}

void LessPALookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive, bool isHighlighted,
                                          bool isTicked, bool hasSubMenu, const juce::String& text,
                                          const juce::String& shortcutKeyText,
                                          const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator) {
        g.setColour(LessPAColours::border);
        g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }

    auto itemTextColour = textColour != nullptr ? *textColour
                                               : findColour(juce::PopupMenu::textColourId);
    auto remaining = area.reduced(3, 1);

    if (isHighlighted && isActive) {
        g.setColour(findColour(juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRoundedRectangle(remaining.toFloat(), 3.0f);
        itemTextColour = findColour(juce::PopupMenu::highlightedTextColourId);
    }

    g.setColour(isActive ? itemTextColour : itemTextColour.withMultipliedAlpha(0.4f));
    g.setFont(juce::FontOptions(juce::jlimit(9.0f, 18.0f,
                                             static_cast<float>(area.getHeight()) * 0.55f)));

    const auto gutter = remaining.removeFromLeft(juce::roundToInt(static_cast<float>(remaining.getHeight()) * 0.9f))
                                 .toFloat();

    if (icon != nullptr) {
        icon->drawWithin(g, gutter.reduced(2.0f), juce::RectanglePlacement::centred, 1.0f);
    } else if (isTicked) {
        // A dot, not a tick glyph: every menu here is a ComboBox's one-of-N
        // choice list, and a dot reads as "this is the current one" without
        // the independent-checkbox connotation a tick carries.
        const float diameter = juce::jmin(6.0f, gutter.getHeight() * 0.4f);
        g.fillEllipse(juce::Rectangle<float>(diameter, diameter).withCentre(gutter.getCentre()));
    }

    if (hasSubMenu) {
        const float arrowHeight = static_cast<float>(remaining.getHeight()) * 0.5f;
        const auto arrowZone = remaining.removeFromRight(juce::roundToInt(arrowHeight)).toFloat();

        juce::Path arrow;
        arrow.startNewSubPath(arrowZone.getX(), arrowZone.getCentreY() - arrowHeight * 0.3f);
        arrow.lineTo(arrowZone.getX() + arrowHeight * 0.4f, arrowZone.getCentreY());
        arrow.lineTo(arrowZone.getX(), arrowZone.getCentreY() + arrowHeight * 0.3f);
        g.strokePath(arrow, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    if (shortcutKeyText.isNotEmpty()) {
        const auto shortcutWidth =
            juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), shortcutKeyText) + 8;
        const auto shortcutArea = remaining.removeFromRight(juce::jmin(remaining.getWidth() / 2, shortcutWidth));
        g.setOpacity(0.6f);
        g.drawFittedText(shortcutKeyText, shortcutArea, juce::Justification::centredRight, 1);
        g.setOpacity(1.0f);
    }

    g.drawFittedText(text, remaining.withTrimmedRight(4), juce::Justification::centredLeft, 1);
}

void LessPALookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& backgroundColour,
                                             bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat();

    // Square buttons (the "?" badge) become fully round; anything else gets a
    // modest radius.
    const float corner = juce::approximatelyEqual(bounds.getWidth(), bounds.getHeight())
                             ? bounds.getHeight() * 0.5f
                             : juce::jmin(6.0f, bounds.getHeight() * 0.35f);

    // Flat: no LookAndFeel_V4-style contrasting()/saturation shading and no
    // outline. Hover and press are a brightness step on the same colour, which
    // keeps the button reading as one solid shape at every state.
    auto colour = backgroundColour;
    if (shouldDrawButtonAsDown)
        colour = colour.brighter(0.25f);
    else if (shouldDrawButtonAsHighlighted)
        colour = colour.brighter(0.12f);

    if (!button.isEnabled())
        colour = colour.withMultipliedAlpha(0.5f);

    g.setColour(colour);
    g.fillRoundedRectangle(bounds, corner);
}

juce::Rectangle<int> LessPALookAndFeel::getTooltipBounds(const juce::String& tipText,
                                                        juce::Point<int> screenPos,
                                                        juce::Rectangle<int> parentArea)
{
    // Never wider than the parent can actually show, so the constrainedWithin()
    // below only ever *moves* the box rather than shrinking it -- shrinking is
    // what clipped the text before, because the layout had already been made
    // at the larger width by then.
    const float wrapWidth = static_cast<float>(juce::jmax(80, juce::jmin(tooltipMaxWidth,
                                                                        parentArea.getWidth()
                                                                            - 2 * tooltipPaddingX)));

    const auto layout = layoutTooltip(*this, tipText, juce::Colours::black, wrapWidth);

    // ceil, not truncate: drawTooltip re-derives its wrap width as
    // (width - tooltipPaddingX), and that has to be >= the width measured here
    // or the text would break onto an extra line that the box has no room for.
    const int width = static_cast<int>(std::ceil(layout.getWidth())) + tooltipPaddingX;
    const int height = static_cast<int>(std::ceil(layout.getHeight())) + tooltipPaddingY;

    // Placement follows LookAndFeel_V2: offset away from whichever quadrant of
    // the parent the pointer is in, so the tip never covers the control it
    // describes.
    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (width + 12)
                                                                     : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (height + 6)
                                                                      : screenPos.y + 6,
                                width, height)
        .constrainedWithin(parentArea);
}

void LessPALookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height)
{
    const juce::Rectangle<float> bounds(static_cast<float>(width), static_cast<float>(height));
    constexpr float corner = 5.0f;

    g.setColour(findColour(juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(findColour(juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);

    // The same wrap width getTooltipBounds measured at (it added exactly
    // tooltipPaddingX to the layout width), so the drawn breaks match the box.
    layoutTooltip(*this, text, findColour(juce::TooltipWindow::textColourId),
                  static_cast<float>(juce::jmax(1, width - tooltipPaddingX)))
        .draw(g, bounds);
}
