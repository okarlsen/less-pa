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
    //
    // The panel is widescreen and three columns wide. The four AEC3
    // signal-chain stages now read left to right rather than top to bottom
    // (Input Conditioning -> Adaptive Filter -> Residual Suppression ->
    // Double-Talk Protection), with the meter bank stacked under column 1 and
    // a full-width footer under all three columns.
    static constexpr int designWidth = 768;
    static constexpr int designHeight = 432;

    static constexpr int outerMargin = 10;

    // Hoisted out of paint()/resized() because both need it and they drifted
    // apart in the previous hand-chained layout (paint() removed 40px, and a
    // separate 40 in resized() had to be kept in sync by hand).
    // Two lines of text: the product name plus a one-line subtitle.
    static constexpr int titleStripHeight = 62;
    static constexpr int titleGap = 10;

    // The "?" help button and, to its left, a small version readout -- so
    // whoever's looking at the plugin (or a screenshot of it) can tell which
    // build they're running without opening the help dialog. Named and
    // asserted like everything else here so a future title-strip change
    // can't silently let the version text run under the subtitle.
    static constexpr int helpButtonSize = 24;
    static constexpr int versionHelpGap = 6;
    static constexpr int versionLabelWidth = 40;

    // The three columns are equal width. Column 3 absorbs the sub-pixel
    // remainder in computeSectionBounds() so the right-hand margin stays
    // exactly outerMargin at every scale factor -- the corner wordmark is
    // aligned to that same edge.
    static constexpr int columnWidth = 240;
    static constexpr int columnGap = 14;
    static constexpr int columnAreaHeight = 302;

    // Full-width strip under all three columns: the PA delay readout on the
    // left, Dry/Wet Mix centred, the corner wordmark on the right. Tall enough
    // for a slider rather than the single text line it used to be -- Dry/Wet
    // lives here rather than in column 1 because column 1's height is what
    // sets columnAreaHeight, so every pixel taken out of it comes off all
    // three columns and off the window.
    static constexpr int footerGap = 12;
    static constexpr int footerStripHeight = 26;

    // Row heights, shared by every section. They live here rather than in
    // PluginEditor.cpp's anonymous namespace specifically so the
    // static_asserts below can prove the section heights against them instead
    // of the old arrangement, where the row arithmetic was only a comment.
    static constexpr int sectionHeaderRow = 18;
    static constexpr int controlLabelRow = 16;
    static constexpr int comboRow = 28;
    static constexpr int sliderRow = 26;
    static constexpr int toggleRow = 24;

    static constexpr int panelPaddingX = 12;
    static constexpr int panelPaddingY = 14;

    // Horizontal clearance between the footer's three tenants.
    static constexpr int readoutLogoGap = 10;

    // The inline Dry/Wet control: label, slider and value box on one row
    // rather than the label-above-slider pairing every panel uses. The footer
    // is one slider tall, so the pairing does not fit -- and a single row is
    // what makes it read as a footer readout rather than a fifth section.
    static constexpr int dryWetLabelWidth = 78;
    static constexpr int dryWetLabelGap = 8;
    static constexpr int dryWetFooterWidth = 266;

    // The worst-case delay readout ("PA delay: 1234 ms  (median 1234)")
    // measures ~175 design px at the 12pt the label is set to in resized().
    // Asserted below against the space the centred Dry/Wet control leaves it.
    static constexpr int readoutMinWidth = 180;

    // -- Column 1: INPUT CONDITIONING above the ungrouped meter bank ---------
    //
    // Column 1 is still the tallest of the three (a panel plus the meter
    // bank), so its natural height is what sets columnAreaHeight; columns 2
    // and 3 are spaced out to match it.
    static constexpr int column1ControlGap = 12;
    static constexpr int inputSectionHeight = 154;

    static constexpr int column1BlockGap = 14;

    // paint() draws the meter bridge from the meters' own bounds, expanded by
    // this much vertically. Reserving it inside the block rather than letting
    // the bridge overhang keeps column 1 exactly as tall as the other two --
    // an overhang would make column 1 visibly deeper than columns 2 and 3.
    static constexpr int meterBridgeClearance = 6;
    static constexpr int outputBlockHeight = 134;

    // Four stacked horizontal meters (label left, bar right) rather than the
    // old row of four vertical bars: four side-by-side bars needed the whole
    // window width to keep "PA-ref(sc)" and "Suppression" legible, which a
    // 240px column no longer has. See LevelMeterComponent::Orientation.
    //
    // "Meters post HPF" is the last row *inside* the bank rather than a
    // control below it: it changes what the four bars above it are showing,
    // not what the plugin does to audio, so it belongs to the instrument. Its
    // text is set to the meter-label size in resized() for the same reason.
    static constexpr int meterRowHeight = 18;
    static constexpr int meterRowGap = 6;
    static constexpr int meterToggleGap = 8;
    static constexpr int meterBankHeight = 122;
    static constexpr int meterBarHeight = 11; // shorter than its row, so the four read as separate bars
    static constexpr int meterLabelWidth = 80;
    static constexpr int meterLabelGap = 8;

    // -- Columns 2 and 3: deliberately roomier ------------------------------
    //
    // These two hold much less than column 1 (three controls and four
    // controls, against column 1's two plus the whole meter bank), so at
    // column 1's spacing they would end well short of the shared column height
    // and leave visible dead background below them. The fix is more generous
    // fixed spacing -- extra bottom padding and bigger gaps between control
    // pairs -- not a runtime stretch: every other number in this file is a
    // fixed design constant proved by a static_assert, and a stretch would put
    // these two outside that guarantee.
    //
    // They are re-tuned whenever column 1's height changes, which is the whole
    // point of moving Dry/Wet into the footer and the toggle into the bank:
    // 54px off column 1 came off all three columns and off the window.
    //
    // The extra room is BOTTOM padding, never top. Every panel in every column
    // is inset by exactly panelPaddingY at the top (see the panelBody() lambda
    // in resized()), so the three top-row section headers land on one line.
    // Splitting it evenly with a symmetric Rectangle::reduced() is what put
    // ADAPTIVE FILTER's and DOUBLE-TALK PROTECTION's headers 6 scaled px below
    // INPUT CONDITIONING's, which read as a broken layout in a real host.
    static constexpr int widePanelPaddingBottom = 20;

    static constexpr int column2HeaderGap = 24;
    static constexpr int column2ControlGap = 24;
    static constexpr int column2PanelGap = 14;
    static constexpr int adaptiveSectionHeight = 120;
    static constexpr int residualSectionHeight = 168;

    static constexpr int column3HeaderGap = 20;
    static constexpr int column3ControlGap = 20;
    static constexpr int doubleTalkSectionHeight = 302;

    // The four named sections are the real AEC3 signal chain in signal order,
    // not a cosmetic grouping: everything that touches the samples before AEC3
    // sees them, then the linear adaptive filter, then the nonlinear residual
    // suppressor, then the detector that gates that suppressor. ADAPTIVE
    // FILTER holding a single control is correct rather than wasteful -- Tail
    // Length *is* the linear filter length, and nothing else touches it.

    // Makes "these all sum exactly" a compile error rather than a comment:
    // computeSectionBounds() slices strictly top-down and left-to-right, so
    // any drift here would silently push content off an edge.
    static_assert(2 * outerMargin + 3 * columnWidth + 2 * columnGap == designWidth,
                  "column widths, gaps and margins must fill designWidth exactly");
    static_assert(2 * outerMargin + titleStripHeight + titleGap + columnAreaHeight
                          + footerGap + footerStripHeight
                      == designHeight,
                  "title, columns, footer, gaps and margins must fill designHeight exactly");
    static_assert(helpButtonSize + versionHelpGap + versionLabelWidth
                      <= columnWidth,
                  "the help button and version label must fit inside the title strip "
                  "without crowding the product name/subtitle to their left");

    static_assert(inputSectionHeight + column1BlockGap + outputBlockHeight == columnAreaHeight,
                  "column 1 must fill the shared column height exactly");
    static_assert(adaptiveSectionHeight + column2PanelGap + residualSectionHeight == columnAreaHeight,
                  "column 2 must fill the shared column height exactly");
    static_assert(doubleTalkSectionHeight == columnAreaHeight,
                  "column 3 is a single panel, so it is the shared column height");

    // ...and each section's height against the row list resized() actually
    // lays out inside it.
    static_assert(sectionHeaderRow + 2 * (column1ControlGap + controlLabelRow + sliderRow)
                          + 2 * panelPaddingY
                      == inputSectionHeight,
                  "INPUT CONDITIONING rows + padding must equal inputSectionHeight");
    static_assert(2 * meterBridgeClearance + meterBankHeight == outputBlockHeight,
                  "the meter bank plus its bridge clearance must equal outputBlockHeight");
    static_assert(4 * meterRowHeight + 3 * meterRowGap + meterToggleGap + toggleRow
                      == meterBankHeight,
                  "four meter rows, their gaps and the post-HPF toggle must equal meterBankHeight");
    // Columns 2 and 3 pad as panelPaddingY (top) + widePanelPaddingBottom
    // (bottom), not 2 * one number -- the asymmetry is the point, so it is
    // spelled out here rather than hidden behind a doubled constant.
    static_assert(sectionHeaderRow + column2HeaderGap + controlLabelRow + comboRow
                          + panelPaddingY + widePanelPaddingBottom
                      == adaptiveSectionHeight,
                  "ADAPTIVE FILTER rows + padding must equal adaptiveSectionHeight");
    static_assert(sectionHeaderRow + column2HeaderGap + controlLabelRow + comboRow
                          + column2ControlGap + toggleRow
                          + panelPaddingY + widePanelPaddingBottom
                      == residualSectionHeight,
                  "RESIDUAL SUPPRESSION rows + padding must equal residualSectionHeight");
    static_assert(sectionHeaderRow + column3HeaderGap + controlLabelRow + comboRow
                          + 3 * (column3ControlGap + controlLabelRow + sliderRow)
                          + panelPaddingY + widePanelPaddingBottom
                      == doubleTalkSectionHeight,
                  "DOUBLE-TALK PROTECTION rows + padding must equal doubleTalkSectionHeight");

    // The invariant the top-row headers depend on: columns 2 and 3 may only
    // ever be roomier than column 1 *below* their header, never above it.
    static_assert(widePanelPaddingBottom >= panelPaddingY,
                  "columns 2 and 3 take their extra breathing room as bottom padding");

    // The corner wordmark. Width is the design-pixel size; the height follows
    // from the asset's own 814x200 proportions so the mark can never be
    // stretched by a change to one number.
    static constexpr int logoWidth = 70;
    static constexpr float logoAspect = 814.0f / 200.0f;

    // The footer now has three tenants, so "they fit" is worth proving rather
    // than eyeballing at one window size: the strip has to be tall enough for
    // the inline slider and the wordmark, and the centred Dry/Wet control has
    // to leave its two neighbours their worst-case widths. All three are
    // fixed design constants scaled by one factor, so proving it here proves
    // it at every point in the resize range.
    static_assert(footerStripHeight >= sliderRow,
                  "the footer strip must be tall enough for the inline Dry/Wet slider");
    static_assert(logoWidth * 200 / 814 <= footerStripHeight,
                  "the wordmark must fit inside the footer strip");
    static_assert(dryWetFooterWidth + 2 * (logoWidth + readoutLogoGap)
                      <= designWidth - 2 * outerMargin,
                  "the centred Dry/Wet control must clear the corner wordmark");
    static_assert(dryWetFooterWidth + 2 * (panelPaddingX + readoutMinWidth + readoutLogoGap)
                      <= designWidth - 2 * outerMargin,
                  "the centred Dry/Wet control must leave the delay readout its worst case");
    static_assert(dryWetLabelWidth + dryWetLabelGap < dryWetFooterWidth,
                  "the inline Dry/Wet label and gap must leave room for the slider");

    struct SectionBounds {
        juce::Rectangle<int> title;
        juce::Rectangle<int> column1Input, column1Output;
        juce::Rectangle<int> column2Adaptive, column2Residual;
        juce::Rectangle<int> column3DoubleTalk;
        juce::Rectangle<int> footer;
    };

    // Single source of truth for the seven top-level rectangles: resized()
    // feeds each one to a Grid, paint() fills the four section ones as
    // panels. Deriving both from the same function is the point -- the panel
    // backgrounds can't drift out of alignment with the controls in them.
    //
    // `column1Output` deliberately gets no panel and no header: the meters are
    // the output stage, and a "METERING" header over a bank of meters tells a
    // professional operator nothing they can't already see. paint() gives it
    // the meter bridge instead, derived from the meters' own bounds.
    SectionBounds computeSectionBounds() const;

    // Needed by both paint() (to draw the mark) and resized() (to keep the
    // delay readout out from under it), so it is derived from the window
    // geometry rather than stored by whichever ran last. Vertically centred in
    // the footer strip rather than pinned to its bottom, so it sits on the
    // same line as the readout and the inline Dry/Wet control beside it.
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

    // The running build's version, so a screenshot or a report from someone
    // testing an older/newer copy is unambiguous. Text is set from
    // JucePlugin_VersionString in the constructor -- CMake's single-sourced
    // project version (see plugin/CMakeLists.txt) -- rather than duplicated
    // here as a literal.
    juce::Label versionLabel{ "versionLabel" };

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
