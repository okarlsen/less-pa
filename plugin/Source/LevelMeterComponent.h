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
//   reference    -- like signalLevel, but for a signal with a *target* level
//                   (the PA feed: the canceller adapts slowly if it is too
//                   quiet). Colour says where it is relative to the zone set
//                   with setTargetZone(): amber below, green inside, red above.
// SettableTooltipClient so the bar itself carries the hint -- hovering the
// meter is the natural gesture, and juce::Component alone has no setTooltip().
class LevelMeterComponent : public juce::Component,
                            public juce::SettableTooltipClient {
public:
    enum class Style { signalLevel, suppression, reference };

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

    // The level range (dBFS) worth aiming for. Drawn as a faint band with a
    // tick at each end, and what a Style::reference meter colours against.
    void setTargetZone(float lowDb, float highDb) {
        zoneLowDb = lowDb;
        zoneHighDb = highDb;
        hasZone = true;
        repaint();
    }

    void setLevel(float newValue) {
        const float db = (style != Style::suppression)
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

        // The target zone sits under the fill so the bar passes over it; its
        // edge ticks are drawn after the fill so they stay visible on a full bar.
        const auto fullBounds = bounds;
        const auto positionOf = [this](float db) {
            return juce::jlimit(0.0f, 1.0f, (db - rangeMinDb) / (rangeMaxDb - rangeMinDb));
        };
        float zoneStart = 0.0f, zoneEnd = 0.0f;
        if (hasZone) {
            zoneStart = positionOf(zoneLowDb);
            zoneEnd = positionOf(zoneHighDb);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            if (orientation == Orientation::vertical)
                g.fillRect(fullBounds.getX(), fullBounds.getBottom() - fullBounds.getHeight() * zoneEnd,
                           fullBounds.getWidth(), fullBounds.getHeight() * (zoneEnd - zoneStart));
            else
                g.fillRect(fullBounds.getX() + fullBounds.getWidth() * zoneStart, fullBounds.getY(),
                           fullBounds.getWidth() * (zoneEnd - zoneStart), fullBounds.getHeight());
        }

        const float proportion = juce::jlimit(0.0f, 1.0f, (displayedDb - rangeMinDb) / (rangeMaxDb - rangeMinDb));
        if (proportion > 0.0f) {
            // Vertical grows from the bottom, horizontal from the left --
            // both being "away from the zero end" of the bar.
            auto filled = orientation == Orientation::vertical
                              ? bounds.removeFromBottom(bounds.getHeight() * proportion)
                              : bounds.removeFromLeft(bounds.getWidth() * proportion);
            juce::Colour colour;
            if (style == Style::suppression)
                colour = LessPAColours::meterSuppression;
            else if (style == Style::reference && hasZone)
                colour = displayedDb < zoneLowDb    ? LessPAColours::meterCaution  // too quiet
                         : displayedDb <= zoneHighDb ? LessPAColours::meterSafe     // in the zone
                                                     : LessPAColours::meterDanger;  // too hot
            else
                colour = displayedDb > -3.0f    ? LessPAColours::meterDanger
                         : displayedDb > -12.0f ? LessPAColours::meterCaution
                                                : LessPAColours::meterSafe;
            g.setColour(colour);
            g.fillRoundedRectangle(filled, corner);
        }

        if (hasZone) {
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            for (const float edge : { zoneStart, zoneEnd }) {
                if (orientation == Orientation::vertical) {
                    const float y = fullBounds.getBottom() - fullBounds.getHeight() * edge;
                    g.drawLine(fullBounds.getX(), y, fullBounds.getRight(), y, 1.0f);
                } else {
                    const float x = fullBounds.getX() + fullBounds.getWidth() * edge;
                    g.drawLine(x, fullBounds.getY(), x, fullBounds.getBottom(), 1.0f);
                }
            }
        }
    }

private:
    Style style;
    Orientation orientation = Orientation::vertical;
    float rangeMinDb = -60.0f;
    float rangeMaxDb = 0.0f;
    float displayedDb = -60.0f;
    bool hasZone = false;
    float zoneLowDb = 0.0f;
    float zoneHighDb = 0.0f;
    static constexpr float decayDbPerTick = 1.2f; // ~36dB/s at the editor's 30Hz timer
};
