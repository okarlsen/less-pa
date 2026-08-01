#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LessPALookAndFeel.h"

// Peak meter with instant attack and a fixed dB/tick visual decay.
// setLevel() should be called from a UI-thread timer -- this component owns
// the ballistics, the caller just reports facts.
//
// Two styles share one implementation:
//   signalLevel  -- input is a linear amplitude (0..1+), e.g. an audio peak.
//                   Range is -60..0 dB, red/yellow/green like a normal meter.
//   suppression  -- input is already in dB (measured input-to-output level
//                   reduction), range 0..40 dB. A single colour: more
//                   suppression isn't "dangerous", so the red/yellow/green
//                   level-meter semantics don't apply.
// SettableTooltipClient so the bar itself carries the hint -- hovering the
// meter is the natural gesture, and juce::Component alone has no setTooltip().
class LevelMeterComponent : public juce::Component,
                            public juce::SettableTooltipClient {
public:
    enum class Style { signalLevel, suppression };

    // Which way the bar grows. Defaults to vertical -- that is the shape a
    // channel-strip meter is expected to have, and defaulting to it keeps
    // every existing use of this component unaffected by the addition. The
    // editor's meter bank opts into horizontal because four labelled bars
    // stacked as rows fit a narrow column, where four side-by-side vertical
    // bars need the width of the whole window to stay legible.
    enum class Orientation { vertical, horizontal };

    explicit LevelMeterComponent(Style styleIn = Style::signalLevel) : style(styleIn) {
        if (style == Style::suppression) {
            rangeMinDb = 0.0f;
            rangeMaxDb = 40.0f;
        }
        displayedDb = rangeMinDb;
    }

    void setOrientation(Orientation newOrientation) {
        if (orientation == newOrientation)
            return;
        orientation = newOrientation;
        repaint();
    }

    void setLevel(float newValue) {
        const float db = (style == Style::signalLevel)
                              ? juce::Decibels::gainToDecibels(newValue, rangeMinDb)
                              : newValue;
        if (db > displayedDb)
            displayedDb = db;
        else
            displayedDb = juce::jmax(rangeMinDb, displayedDb - decayDbPerTick);
        repaint();
    }

    // Only the fill *axis* varies with orientation -- the dB thresholds, the
    // colour choice and the ballistics are shared, so a horizontal bar reads
    // green/yellow/red at exactly the same levels as a vertical one.
    void paint(juce::Graphics& g) override {
        auto bounds = getLocalBounds().toFloat();
        constexpr float corner = 3.0f;

        // Flat trough, no outline: the translucent-black-plus-white-border
        // version read as an inset hardware meter, which fights the flat style
        // everywhere else.
        //
        // Window background rather than the control surface an unfilled slider
        // track uses. A slider track is a 3px line, so a 1-step-lighter grey is
        // enough to find it; a meter trough is a large area sitting directly on
        // the panel, and at that size the same near-identical grey (#23262C on
        // #1C1F24) made the idle meters effectively disappear. Going darker
        // than the panel instead also matches the recessed-slot convention.
        g.setColour(LessPAColours::windowBackground);
        g.fillRoundedRectangle(bounds, corner);

        const float proportion = juce::jlimit(0.0f, 1.0f, (displayedDb - rangeMinDb) / (rangeMaxDb - rangeMinDb));
        if (proportion > 0.0f) {
            // Vertical grows from the bottom, horizontal from the left --
            // both being "away from the zero end" of the bar.
            auto filled = orientation == Orientation::vertical
                              ? bounds.removeFromBottom(bounds.getHeight() * proportion)
                              : bounds.removeFromLeft(bounds.getWidth() * proportion);
            const juce::Colour colour = style == Style::suppression
                                             ? LessPAColours::meterSuppression
                                             : (displayedDb > -3.0f    ? LessPAColours::meterDanger
                                                : displayedDb > -12.0f ? LessPAColours::meterCaution
                                                                       : LessPAColours::meterSafe);
            g.setColour(colour);
            g.fillRoundedRectangle(filled, corner);
        }
    }

private:
    Style style;
    Orientation orientation = Orientation::vertical;
    float rangeMinDb = -60.0f;
    float rangeMaxDb = 0.0f;
    float displayedDb = -60.0f;
    static constexpr float decayDbPerTick = 1.2f; // ~36dB/s at the editor's 30Hz timer
};
