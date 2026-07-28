#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
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

    juce::TextButton helpButton{ "?" };

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
