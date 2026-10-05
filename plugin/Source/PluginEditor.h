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
    // PA CANCELLER then BLEED SUPPRESSOR (the two stages) -> OUTPUT (what
    // the canceller is doing, and the mix). Every control is on this one
    // page.
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

    static constexpr int resetButtonWidth = 64; // Defaults, in the BLEED SUPPRESSOR header row

    // The middle column holds two stages, so its second header and
    // sliders sit closer than the other columns' controls to fit.
    static constexpr int stageGap = 14;       // Tail Length to the suppressor's header
    static constexpr int stageHeaderGap = 8;  // suppressor header to Strength
    static constexpr int stageControlGap = 10; // between the suppressor's sliders

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
    static_assert(sectionHeaderRow + headerGap + labelledCombo + stageGap + sectionHeaderRow
                          + stageHeaderGap + 3 * labelledSlider + 2 * stageControlGap
                      <= panelBodyHeight,
                  "PA CANCELLER and BLEED SUPPRESSOR rows must fit the panel");
    static_assert(sectionHeaderRow + headerGap + meterPair + meterBlockGap + labelledSlider
                          + controlGap + 2 * statusRow
                      <= panelBodyHeight,
                  "OUTPUT rows must fit the panel");
    static_assert(logoWidth * 200 / 814 <= footerStripHeight,
                  "the wordmark must fit inside the footer strip");

    struct SectionBounds {
        juce::Rectangle<int> title;
        juce::Rectangle<int> column1, column2, column3;
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
    void updateResetButton();
    void resetSuppressorToDefaults();

    // The bleed suppressor's controls (Strength, Range, Time) -- the one list
    // both the Defaults button's enabled state and the reset itself work from.
    // Tail Length is left out on purpose: it is a venue choice, not a
    // sound-shaping setting someone would want undone with it.
    std::array<juce::RangedAudioParameter*, 3> getSuppressorParameters() const;

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
    juce::Label cancellationSectionLabel{ "cancellationSectionLabel", "PA CANCELLER" };
    juce::Label suppressorSectionLabel{ "suppressorSectionLabel", "BLEED SUPPRESSOR" };
    juce::Label outputSectionLabel{ "outputSectionLabel", "OUTPUT" };

    // Defaults: puts Strength, Range and Time back to their defaults; the
    // canceller's learned filter is untouched. Greyed out when they already
    // are, so it also answers "have I changed anything here?" at a glance.
    juce::TextButton resetButton{ "Defaults" };

    juce::Label tailLengthLabel{ "tailLengthLabel", "Tail Length" };
    juce::ComboBox tailLengthCombo;

    // The bleed suppressor's controls (parameter IDs amount, maxReduction,
    // response). All apply live and are cheap to change, so they commit on
    // every value change like Mix. Range's slider runs 0..24 (more
    // reduction to the right, like Strength) and shows the parameter's
    // negative dB value.
    juce::Label amountLabel{ "amountLabel", "Strength" };
    juce::Slider amountSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label maxReductionLabel{ "maxReductionLabel", "Range" };
    juce::Slider maxReductionSlider{ juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Label responseLabel{ "responseLabel", "Time" };
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
    juce::Label suppressionMeterLabel{ "suppressionMeterLabel", "Reduction" };

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
