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
    static constexpr int designHeight = 960;

    // Hoisted out of paint()/resized() because both need it and they drifted
    // apart in the previous hand-chained layout (paint() removed 40px, and a
    // separate 40 in resized() had to be kept in sync by hand).
    // Two lines of text: the product name plus a one-line subtitle.
    static constexpr int titleStripHeight = 62;

    static constexpr int outerMargin = 10;
    static constexpr int titleGap = 10;
    static constexpr int sectionGap = 10;

    // These five are not free parameters -- each panel's height equals its
    // section's row heights plus 2 * panelPaddingY (the ungrouped strip has no
    // panel, so no padding), and together with the constants above they sum to
    // exactly designHeight. See resized() for the row lists.
    //
    // The four named sections are the real AEC3 signal chain in signal order,
    // not a cosmetic grouping: everything that touches the samples before AEC3
    // sees them, then the linear adaptive filter, then the nonlinear residual
    // suppressor, then the detector that gates that suppressor. ADAPTIVE
    // FILTER holding a single control is correct rather than wasteful -- Tail
    // Length *is* the linear filter length, and nothing else touches it.
    static constexpr int inputSectionHeight = 142;
    static constexpr int adaptiveSectionHeight = 88;
    static constexpr int residualSectionHeight = 124;
    static constexpr int doubleTalkSectionHeight = 244;
    static constexpr int ungroupedHeight = 230;

    static constexpr int panelPaddingX = 12;
    static constexpr int panelPaddingY = 10;

    // Makes "they sum to exactly designHeight" a compile error rather than a
    // comment: computeSectionBounds() slices the window strictly top-down, so
    // any drift here would silently push the last section off the bottom.
    static_assert(2 * outerMargin + titleStripHeight + titleGap
                          + inputSectionHeight + sectionGap
                          + adaptiveSectionHeight + sectionGap
                          + residualSectionHeight + sectionGap
                          + doubleTalkSectionHeight + sectionGap
                          + ungroupedHeight
                      == designHeight,
                  "section heights, gaps and margins must fill designHeight exactly");

    // The corner wordmark. Width is the design-pixel size; the height follows
    // from the asset's own 814x200 proportions so the mark can never be
    // stretched by a change to one number.
    static constexpr int logoWidth = 70;
    static constexpr float logoAspect = 814.0f / 200.0f;

    struct SectionBounds {
        juce::Rectangle<int> title, input, adaptive, residual, doubleTalk, ungrouped;
    };

    // Single source of truth for the six top-level rectangles: resized()
    // feeds each one to a Grid, paint() fills the four section ones as
    // panels. Deriving both from the same function is the point -- the panel
    // backgrounds can't drift out of alignment with the controls in them.
    //
    // `ungrouped` deliberately gets no panel and no header: Dry/Wet Mix and
    // the meters are the output stage, and a "METERING" header over a row of
    // meters tells a professional operator nothing they can't already see.
    SectionBounds computeSectionBounds() const;

    // Needed by both paint() (to draw the mark) and resized() (to keep the
    // delay readout out from under it), so it is derived from the window
    // geometry rather than stored by whichever ran last.
    juce::Rectangle<int> computeLogoBounds() const;
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

    // The SGTM wordmark, drawn small in the bottom-right corner (see paint()).
    // It is the company mark, not the product identity -- the title strip
    // carries the product name. A raster PNG, so juce::Image rather than
    // Drawable.
    juce::Image logoImage;

    juce::TextButton helpButton{ "?" };

    // Uppercase group headers. They live inside their panel's top row rather
    // than floating above it, so the panel rectangle and the Grid area it is
    // laid out from are one and the same rectangle.
    juce::Label inputSectionLabel{ "inputSectionLabel", "INPUT CONDITIONING" };
    juce::Label adaptiveSectionLabel{ "adaptiveSectionLabel", "ADAPTIVE FILTER" };
    juce::Label residualSectionLabel{ "residualSectionLabel", "RESIDUAL SUPPRESSION" };
    juce::Label doubleTalkSectionLabel{ "doubleTalkSectionLabel", "DOUBLE-TALK PROTECTION" };

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
