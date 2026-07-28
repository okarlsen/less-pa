#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Vertical peak meter with instant attack and a fixed dB/tick visual decay.
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
class LevelMeterComponent : public juce::Component {
public:
    enum class Style { signalLevel, suppression };

    explicit LevelMeterComponent(Style styleIn = Style::signalLevel) : style(styleIn) {
        if (style == Style::suppression) {
            rangeMinDb = 0.0f;
            rangeMaxDb = 40.0f;
        }
        displayedDb = rangeMinDb;
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

    void paint(juce::Graphics& g) override {
        auto bounds = getLocalBounds().toFloat();

        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(bounds, 3.0f);

        const float proportion = juce::jlimit(0.0f, 1.0f, (displayedDb - rangeMinDb) / (rangeMaxDb - rangeMinDb));
        if (proportion > 0.0f) {
            auto filled = bounds.removeFromBottom(bounds.getHeight() * proportion);
            const juce::Colour colour = style == Style::suppression
                                             ? juce::Colours::cyan
                                             : (displayedDb > -3.0f    ? juce::Colours::red
                                                : displayedDb > -12.0f ? juce::Colours::yellow
                                                                       : juce::Colours::limegreen);
            g.setColour(colour);
            g.fillRoundedRectangle(filled, 3.0f);
        }

        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.drawRoundedRectangle(getLocalBounds().toFloat(), 3.0f, 1.0f);
    }

private:
    Style style;
    float rangeMinDb = -60.0f;
    float rangeMaxDb = 0.0f;
    float displayedDb = -60.0f;
    static constexpr float decayDbPerTick = 1.2f; // ~36dB/s at the editor's 30Hz timer
};
