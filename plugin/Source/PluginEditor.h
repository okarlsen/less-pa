#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "LessPALookAndFeel.h"
#include "LevelMeterComponent.h"

class PAEchoCancellerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                             private juce::Timer
{
public:
    explicit PAEchoCancellerAudioProcessorEditor(PAEchoCancellerAudioProcessor&);
    ~PAEchoCancellerAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // The window is resizable with a locked aspect ratio (see the
    // ComponentBoundsConstrainer setup in the constructor), so every layout
    // constant below is expressed in "design pixels" at the default size and
    // multiplied by one uniform factor. Without that, resizing would just
    // leave a growing empty strip at the bottom instead of scaling.
    static constexpr int designWidth = 340;
    static constexpr int designHeight = 860;

    // Hoisted out of paint()/resized() because both need it and they drifted
    // apart in the previous hand-chained layout (paint() removed 40px, and a
    // separate 40 in resized() had to be kept in sync by hand).
    static constexpr int titleStripHeight = 56;

    static constexpr int outerMargin = 10;
    static constexpr int titleGap = 10;
    static constexpr int sectionGap = 12;

    // These three are not free parameters -- each equals its section's row
    // heights plus 2 * panelPaddingY, and together with the constants above
    // they sum to exactly designHeight. See resized() for the row lists.
    static constexpr int echoSectionHeight = 124;
    static constexpr int processingSectionHeight = 400;
    static constexpr int meteringSectionHeight = 226;

    static constexpr int panelPaddingX = 12;
    static constexpr int panelPaddingY = 10;

    struct SectionBounds {
        juce::Rectangle<int> title, echo, processing, metering;
    };

    // Single source of truth for the four top-level rectangles: resized()
    // feeds each one to a Grid, paint() fills the three section ones as
    // panels. Deriving both from the same function is the point -- the panel
    // backgrounds can't drift out of alignment with the controls in them.
    SectionBounds computeSectionBounds() const;
    float getUiScale() const;

    void timerCallback() override;
    void tailLengthComboChanged();
    void updateTailLengthCombo();
    void suppressionStrengthComboChanged();
    void updateSuppressionStrengthCombo();
    void limitHfGainToggleChanged();
    void updateLimitHfGainToggle();
    void nearendDetectorComboChanged();
    void updateNearendDetectorCombo();
    void nearendSensitivitySliderChanged();
    void updateNearendSensitivitySlider();
    void protectionHoldTimeSliderChanged();
    void updateProtectionHoldTimeSlider();
    void transitionSmoothingSliderChanged();
    void updateTransitionSmoothingSlider();
    void hpfSliderChanged();
    void updateHpfSlider();
    void referenceGainSliderChanged();
    void updateReferenceGainSlider();
    void metersPostFilterToggleChanged();
    void updateMetersPostFilterToggle();
    void dryWetSliderChanged();
    void updateDryWetSlider();
    void showHelpDialog();

    PAEchoCancellerAudioProcessor& processor;

    // Declared before every component below on purpose: members are destroyed
    // in reverse declaration order, so this outlives the components that are
    // still pointing at it via setLookAndFeel().
    LessPALookAndFeel lookAndFeel;

    // Short per-control hints. Complements the help dialog rather than
    // replacing it -- one clause each, where the dialog has a paragraph.
    juce::TooltipWindow tooltipWindow{ this };

    // The SGTM wordmark, drawn in place of a text title (see paint()). A raster
    // PNG, so juce::Image rather than Drawable.
    juce::Image logoImage;

    juce::TextButton helpButton{ "?" };

    // Uppercase group headers. They live inside their panel's top row rather
    // than floating above it, so the panel rectangle and the Grid area it is
    // laid out from are one and the same rectangle.
    juce::Label echoSectionLabel{ "echoSectionLabel", "ECHO CANCELLATION" };
    juce::Label processingSectionLabel{ "processingSectionLabel", "SIGNAL PROCESSING" };
    juce::Label meteringSectionLabel{ "meteringSectionLabel", "METERING" };

    juce::Label tailLengthLabel{ "tailLengthLabel", "Tail Length" };
    juce::ComboBox tailLengthCombo;

    juce::Label suppressionStrengthLabel{ "suppressionStrengthLabel", "Suppression Strength" };
    juce::ComboBox suppressionStrengthCombo;

    juce::ToggleButton limitHfGainToggle{ "Limit HF Gain" };

    // Which detector decides "this moment is genuine audience content" at
    // all -- Classic (protects most of the time) vs the venue-measured
    // Subband (protects specifically in PA gaps). An explicit A/B ComboBox
    // rather than a toggle so both choices are visible by name; applies
    // live like the other suppressor controls.
    juce::Label nearendDetectorLabel{ "nearendDetectorLabel", "Near-end Detector" };
    juce::ComboBox nearendDetectorCombo;

    // Both apply live (see applySuppressorConfigLive in PluginProcessor.h),
    // but still only commit their value -- via nearendSensitivitySliderChanged()/
    // protectionHoldTimeSliderChanged() -- at drag-end or on a discrete text
    // entry, never per-pixel mid-drag (see the onDragEnd/onValueChange
    // wiring in the constructor): applying live avoids a rebuild, but
    // reconstructing SuppressionGain on every pixel of a drag would still
    // be wasteful and pointless.
    juce::Label nearendSensitivityLabel{ "nearendSensitivityLabel", "Near-end Sensitivity" };
    juce::Slider nearendSensitivitySlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label protectionHoldTimeLabel{ "protectionHoldTimeLabel", "Protection Hold Time" };
    juce::Slider protectionHoldTimeSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // How long AEC3 takes to crossfade between its two suppressor tunings
    // when the near-end detector flips, rather than swapping them within a
    // single 4ms block. 0ms is AEC3 stock (instant swap); the shipped 40ms
    // rounds the transition off. Commits at drag-end only, like the two
    // sliders above -- it rebuilds SuppressionGain via the live path.
    juce::Label transitionSmoothingLabel{ "transitionSmoothingLabel", "Transition Smoothing" };
    juce::Slider transitionSmoothingSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label hpfLabel{ "hpfLabel", "Input HPF" };
    juce::Slider hpfSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label referenceGainLabel{ "referenceGainLabel", "PA Reference Trim" };
    juce::Slider referenceGainSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label dryWetLabel{ "dryWetLabel", "Dry/Wet Mix" };
    juce::Slider dryWetSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    LevelMeterComponent inputMeter, sidechainMeter, outputMeter;
    LevelMeterComponent suppressionMeter{ LevelMeterComponent::Style::suppression };
    juce::Label inputMeterLabel{ "inputMeterLabel", "Input" };
    juce::Label sidechainMeterLabel{ "sidechainMeterLabel", "PA-ref(sc)" };
    juce::Label outputMeterLabel{ "outputMeterLabel", "Output" };
    juce::Label suppressionMeterLabel{ "suppressionMeterLabel", "Suppression" };

    juce::ToggleButton metersPostFilterToggle{ "Meters post HPF" };

    // AEC3's live echo-path delay estimate (see
    // getEstimatedEchoPathDelayMs() in PluginProcessor.h) -- the quickest
    // possible answer to "is my sidechain actually wired?" and "is the
    // delay estimate stable?", updated from the same 30Hz timer as the
    // meters. Shows "--" when no estimate is available (no reference
    // routed, or transport stopped).
    juce::Label delayReadoutLabel{ "delayReadoutLabel", "PA delay: --" };

    // Detects the host having stopped calling processBlock entirely (e.g.
    // transport stopped), as opposed to just a quiet tick -- otherwise the
    // meters hold their last audio-thread reading forever.
    uint32_t lastSeenProcessBlockCount = 0;
    int staleTickCount = 0;
    static constexpr int staleTicksBeforeClear = 5; // ~165ms at the 30Hz timer

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PAEchoCancellerAudioProcessorEditor)
};
