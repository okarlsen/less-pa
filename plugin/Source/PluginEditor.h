#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
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
    // multiplied by one uniform factor.
    //
    // Three columns, one panel each, reading left to right as the signal
    // flows: INPUT (what reaches the canceller, and its levels) ->
    // CANCELLATION (the three decisions that matter for a show) -> OUTPUT
    // (what the canceller is doing, and the mix). Response, the one expert
    // control, lives behind "Fine tuning", an overlay across columns 2 and
    // 3 -- still a real, automatable parameter, it just doesn't compete for
    // attention on the main panel.
    static constexpr int designWidth = 768;
    static constexpr int designHeight = 432;

    static constexpr int outerMargin = 10;

    static constexpr int titleStripHeight = 62;
    static constexpr int titleGap = 10;

    static constexpr int helpButtonSize = 24;
    static constexpr int versionHelpGap = 6;
    static constexpr int versionLabelWidth = 40;

    static constexpr int columnWidth = 240;
    static constexpr int columnGap = 14;
    static constexpr int columnAreaHeight = 302;

    static constexpr int footerGap = 12;
    static constexpr int footerStripHeight = 26;

    static constexpr int sectionHeaderRow = 18;
    static constexpr int controlLabelRow = 16;
    static constexpr int comboRow = 28;
    static constexpr int sliderRow = 26;
    static constexpr int buttonRow = 26;
    static constexpr int statusRow = 16;

    // Every panel shares the same top inset (so the three headers sit on one
    // line) and the same bottom inset.
    static constexpr int panelPaddingX = 12;
    static constexpr int panelPaddingTop = 14;
    static constexpr int panelPaddingBottom = 20;
    static constexpr int panelBodyHeight = columnAreaHeight - panelPaddingTop - panelPaddingBottom;

    static constexpr int headerGap = 20;  // header to first control
    static constexpr int controlGap = 20; // between consecutive controls
    static constexpr int meterBlockGap = 24; // controls to a meter block

    static constexpr int meterRowHeight = 18;
    static constexpr int meterRowGap = 6;
    static constexpr int meterBarHeight = 11; // shorter than its row, so stacked bars read as separate bars
    static constexpr int meterLabelWidth = 80;
    static constexpr int meterLabelGap = 8;

    static constexpr int labelledSlider = controlLabelRow + sliderRow;
    static constexpr int labelledCombo = controlLabelRow + comboRow;
    static constexpr int meterPair = 2 * meterRowHeight + meterRowGap;

    static constexpr int fineTuningDoneWidth = 70;
    static constexpr int fineTuningResetWidth = 140;
    static constexpr int fineTuningHeaderButtonGap = 8;

    static constexpr int logoWidth = 70;
    static constexpr float logoAspect = 814.0f / 200.0f;

    static_assert(2 * outerMargin + 3 * columnWidth + 2 * columnGap == designWidth,
                  "column widths, gaps and margins must fill designWidth exactly");
    static_assert(2 * outerMargin + titleStripHeight + titleGap + columnAreaHeight
                          + footerGap + footerStripHeight
                      == designHeight,
                  "title, columns, footer, gaps and margins must fill designHeight exactly");
    static_assert(helpButtonSize + versionHelpGap + versionLabelWidth <= columnWidth,
                  "the version label and help button must leave room for the title");

    // Each panel's rows must fit its body; the remainder is bottom air.
    static_assert(sectionHeaderRow + 2 * (controlGap + labelledSlider) + meterBlockGap + meterPair
                      <= panelBodyHeight,
                  "INPUT rows must fit the panel");
    static_assert(sectionHeaderRow + headerGap + labelledCombo + 2 * (controlGap + labelledSlider)
                          + controlGap + buttonRow
                      <= panelBodyHeight,
                  "CANCELLATION rows must fit the panel");
    static_assert(sectionHeaderRow + headerGap + meterPair + meterBlockGap + labelledSlider
                          + controlGap + 2 * statusRow
                      <= panelBodyHeight,
                  "OUTPUT rows must fit the panel");
    static_assert(sectionHeaderRow + headerGap + labelledSlider + 3 * statusRow <= panelBodyHeight,
                  "FINE TUNING rows must fit the overlay");
    static_assert(logoWidth * 200 / 814 <= footerStripHeight,
                  "the wordmark must fit inside the footer strip");

    struct SectionBounds {
        juce::Rectangle<int> title;
        juce::Rectangle<int> column1, column2, column3;
        juce::Rectangle<int> fineTuning; // spans columns 2 and 3
        juce::Rectangle<int> footer;
    };

    // Single source of truth for the top-level rectangles: resized() lays
    // controls out in them, paint() fills the panels behind them.
    SectionBounds computeSectionBounds() const;

    juce::Rectangle<int> computeLogoBounds() const;
    float getUiScale() const;

    void timerCallback() override;
    void tailLengthComboChanged();
    void updateTailLengthCombo();
    void amountSliderChanged();
    void maxReductionSliderChanged();
    void responseSliderChanged();
    void hpfSliderChanged();
    void referenceGainSliderChanged();
    void dryWetSliderChanged();
    void updateSliders();
    void showHelpDialog();
    void setFineTuningVisible(bool shouldShow);
    void updateFineTuningButton();
    void resetFineTuningToDefaults();

    // The parameters that live on the Fine tuning overlay -- the one list
    // both the "adjusted" indicator and Reset to defaults work from, so the
    // two can never disagree about what the overlay covers.
    std::array<juce::RangedAudioParameter*, 1> getFineTuningParameters() const;

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

    // The running build's version, so a screenshot or a report from someone
    // testing an older/newer copy is unambiguous. Text is set from
    // JucePlugin_VersionString in the constructor -- CMake's single-sourced
    // project version (see plugin/CMakeLists.txt) -- rather than duplicated
    // here as a literal.
    juce::Label versionLabel{ "versionLabel" };

    // Uppercase group headers. They live inside their panel's top row rather
    // than floating above it, so the panel rectangle and the Grid area it is
    // laid out from are one and the same rectangle.
    juce::Label inputSectionLabel{ "inputSectionLabel", "INPUT" };
    juce::Label cancellationSectionLabel{ "cancellationSectionLabel", "CANCELLATION" };
    juce::Label outputSectionLabel{ "outputSectionLabel", "OUTPUT" };
    juce::Label fineTuningSectionLabel{ "fineTuningSectionLabel", "FINE TUNING" };

    // Opens the overlay holding the expert control; its text says when
    // anything in there is away from its default, so nothing tuned in there
    // is ever invisible from the main panel.
    juce::TextButton fineTuningButton{ "Fine tuning..." };
    juce::TextButton fineTuningDoneButton{ "Done" };
    juce::TextButton fineTuningResetButton{ "Reset to defaults" };
    juce::Label fineTuningHintLabel{ "fineTuningHintLabel",
                                     "Shorter follows the PA more tightly but can flutter on crowd "
                                     "noise; longer is smoother. The default suits most shows: "
                                     "Amount and Max Reduction are the controls to reach for first." };

    // Paints the overlay's panel and hosts its controls. Added after every
    // main-panel component, so it sits on top of columns 2 and 3 when shown.
    struct FineTuningOverlay : public juce::Component {
        void paint(juce::Graphics& g) override;
        float scale = 1.0f;
    };
    FineTuningOverlay fineTuningOverlay;

    juce::Label tailLengthLabel{ "tailLengthLabel", "Tail Length" };
    juce::ComboBox tailLengthCombo;

    // The suppressor's two main-panel controls. Both apply live and are
    // cheap to change, so they commit on every value change like Mix.
    // Max Reduction's slider runs 0..24 (more reduction to the right, like
    // Amount) and shows the parameter's negative dB value.
    juce::Label amountLabel{ "amountLabel", "Amount" };
    juce::Slider amountSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label maxReductionLabel{ "maxReductionLabel", "Max Reduction" };
    juce::Slider maxReductionSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // On the Fine tuning overlay.
    juce::Label responseLabel{ "responseLabel", "Response" };
    juce::Slider responseSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label hpfLabel{ "hpfLabel", "Input HPF" };
    juce::Slider hpfSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label referenceGainLabel{ "referenceGainLabel", "PA Reference Trim" };
    juce::Slider referenceGainSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label dryWetLabel{ "dryWetLabel", "Mix" };
    juce::Slider dryWetSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    LevelMeterComponent inputMeter, outputMeter;
    LevelMeterComponent sidechainMeter{ LevelMeterComponent::Style::reference };
    LevelMeterComponent suppressionMeter{ LevelMeterComponent::Style::suppression };
    juce::Label inputMeterLabel{ "inputMeterLabel", "Mic" };
    juce::Label sidechainMeterLabel{ "sidechainMeterLabel", "PA ref" };
    juce::Label outputMeterLabel{ "outputMeterLabel", "Output" };
    juce::Label suppressionMeterLabel{ "suppressionMeterLabel", "Suppression" };

    // The canceller's echo-path delay estimate (see
    // getEstimatedEchoPathDelayMs() in PluginProcessor.h) -- the quickest
    // possible answer to "is my sidechain actually wired?", updated from the
    // same 30Hz timer as the meters. Shows "--" when no estimate is available (no reference
    // routed, or transport stopped).
    juce::Label delayReadoutLabel{ "delayReadoutLabel", "PA delay: --" };

    // One line under the delay readout saying, in words, what state the
    // canceller is in (no PA signal / locking on / cancelling) -- the
    // question an operator actually has when glancing at the plugin.
    juce::Label statusLabel{ "statusLabel", "" };

    // Detects the host having stopped calling processBlock entirely (e.g.
    // transport stopped), as opposed to just a quiet tick -- otherwise the
    // meters hold their last audio-thread reading forever.
    uint32_t lastSeenProcessBlockCount = 0;
    int staleTickCount = 0;
    static constexpr int staleTicksBeforeClear = 5; // ~165ms at the 30Hz timer

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PAEchoCancellerAudioProcessorEditor)
};
