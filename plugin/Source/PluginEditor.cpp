#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <BinaryData.h>

#include <cmath>
#include <initializer_list>

namespace {

// juce::Grid wants its templateRows list and every item's row index to agree,
// and hand-written indices drift the instant a row is inserted or a spacer
// changes height -- which is exactly the failure mode the old manual
// removeFromTop() chain had. This builder appends the track and the item that
// lives in it in one call, so indices are generated rather than maintained.
//
// Row spacing is expressed as explicit zero-item spacer tracks instead of
// Grid::rowGap, because rowGap is uniform and this layout deliberately isn't:
// a control label sits tight against its own slider (4-6px) while consecutive
// controls need noticeably more air (10-12px) for the pairing to read.
//
// All sizes are design pixels; the scale factor handed in converts them.
class SectionGrid
{
public:
    SectionGrid(int numColumnsIn, float scaleIn, int columnGapDesignPx)
        : numColumns(numColumnsIn), scale(scaleIn)
    {
        grid.rowGap = juce::Grid::Px(0);
        grid.columnGap = juce::Grid::Px(scaled(columnGapDesignPx));

        for (int i = 0; i < numColumns; ++i)
            grid.templateColumns.add(juce::Grid::TrackInfo(juce::Grid::Fr(1)));
    }

    // Empty row -- vertical spacing only.
    void gap(int designPx) { addTrack(designPx); }

    // Row holding one component across the full width of the section.
    void row(int designPx, juce::Component& component)
    {
        const int line = addTrack(designPx);
        grid.items.add(juce::GridItem(component).withArea(line, 1, line + 1, numColumns + 1));
    }

    // Row holding one component per column, left to right. A non-zero
    // itemWidthDesignPx makes each item that fixed width, centred in its
    // column (the meter bars are narrow like a real channel strip, but their
    // labels need the full column to read).
    void row(int designPx, std::initializer_list<juce::Component*> components, int itemWidthDesignPx = 0)
    {
        const int line = addTrack(designPx);
        int column = 1;

        for (auto* component : components) {
            auto item = juce::GridItem(*component).withArea(line, column);

            if (itemWidthDesignPx > 0)
                item = item.withWidth(static_cast<float>(scaled(itemWidthDesignPx)))
                           .withJustifySelf(juce::GridItem::JustifySelf::center);

            grid.items.add(item);
            ++column;
        }
    }

    void performLayout(juce::Rectangle<int> area) { grid.performLayout(area); }

private:
    // Returns the 1-based grid line the new row starts on, which is also the
    // row's index once appended.
    int addTrack(int designPx)
    {
        grid.templateRows.add(juce::Grid::TrackInfo(juce::Grid::Px(scaled(designPx))));
        return grid.templateRows.size();
    }

    int scaled(int designPx) const { return juce::roundToInt(scale * static_cast<float>(designPx)); }

    juce::Grid grid;
    int numColumns;
    float scale;
};

// Row heights, shared between the sections so the section-height constants in
// PluginEditor.h can be checked against them by eye.
constexpr int sectionHeaderRow = 18;
constexpr int controlLabelRow = 16;
constexpr int comboRow = 28;
constexpr int sliderRow = 26;
constexpr int toggleRow = 24;
constexpr int meterBarRow = 94;
constexpr int readoutRow = 18;

// Horizontal clearance between the delay readout and the corner wordmark.
constexpr int readoutLogoGap = 10;

} // namespace

PAEchoCancellerAudioProcessorEditor::PAEchoCancellerAudioProcessorEditor(PAEchoCancellerAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&lookAndFeel);

    logoImage = juce::ImageCache::getFromMemory(Assets::sgtm_logo_png, Assets::sgtm_logo_pngSize);

    setSize(designWidth, designHeight);

    // Aspect-locked rather than freely resizable: locking it means the whole
    // window scales by one factor (see getUiScale()), so there is no general
    // reflow problem to solve -- no row has to decide whether to wrap.
    //
    // useBottomRightCornerResizer is false: JUCE's stock diagonal grip would
    // draw over the bottom-right corner, which is exactly where the wordmark
    // now sits. Hosts supply their own window handle for a resizable editor.
    setResizable(true, false);
    // Derived from the design size rather than picked, so the stated limits
    // are the ones the aspect-ratio constrainer will actually allow (the old
    // literals implied two different aspect ratios).
    const auto scaledHeight = [](int width) {
        return juce::roundToInt(static_cast<float>(width)
                                * static_cast<float>(designHeight) / static_cast<float>(designWidth));
    };
    constexpr int minWidth = 300;                   // ~0.88x
    constexpr int maxWidth = 480;                   // ~1.41x
    setResizeLimits(minWidth, scaledHeight(minWidth), maxWidth, scaledHeight(maxWidth));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(static_cast<double>(designWidth) / static_cast<double>(designHeight));

    helpButton.onClick = [this] { showHelpDialog(); };
    addAndMakeVisible(helpButton);

    for (auto* label : { &inputSectionLabel, &adaptiveSectionLabel,
                         &residualSectionLabel, &doubleTalkSectionLabel }) {
        label->setJustificationType(juce::Justification::centredLeft);
        // Secondary text: a group header names the region, it isn't a control,
        // so it should sit behind the labels it groups in the reading order.
        label->setColour(juce::Label::textColourId, LessPAColours::secondaryText);
        addAndMakeVisible(label);
    }

    // Left-aligned, not centred: every control below is either full-width or
    // column-width and starts at its panel's left edge, so a centred label
    // would sit visibly off from the thing it names -- and the sliders'
    // right-hand value boxes push their visual centre left of the row's.
    for (auto* label : { &tailLengthLabel, &suppressionStrengthLabel, &nearendDetectorLabel,
                         &nearendSensitivityLabel, &protectionHoldTimeLabel, &transitionSmoothingLabel,
                         &hpfLabel, &referenceGainLabel, &dryWetLabel })
        label->setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(tailLengthLabel);

    tailLengthCombo.addItem("50ms", 1);
    tailLengthCombo.addItem("200ms", 2);
    tailLengthCombo.addItem("400ms", 3);
    tailLengthCombo.addItem("800ms", 4);
    tailLengthCombo.onChange = [this] { tailLengthComboChanged(); };
    addAndMakeVisible(tailLengthCombo);

    addAndMakeVisible(suppressionStrengthLabel);

    suppressionStrengthCombo.addItem("Gentle", 1);
    suppressionStrengthCombo.addItem("Moderate", 2);
    suppressionStrengthCombo.addItem("Hard", 3);
    suppressionStrengthCombo.onChange = [this] { suppressionStrengthComboChanged(); };
    addAndMakeVisible(suppressionStrengthCombo);

    limitHfGainToggle.onClick = [this] { limitHfGainToggleChanged(); };
    addAndMakeVisible(limitHfGainToggle);

    addAndMakeVisible(nearendDetectorLabel);

    nearendDetectorCombo.addItem("Classic", 1);
    nearendDetectorCombo.addItem("Subband (2-4kHz)", 2);
    nearendDetectorCombo.onChange = [this] { nearendDetectorComboChanged(); };
    addAndMakeVisible(nearendDetectorCombo);

    addAndMakeVisible(nearendSensitivityLabel);

    nearendSensitivitySlider.setRange(0.0, 100.0);
    nearendSensitivitySlider.setTextValueSuffix(" %");
    nearendSensitivitySlider.setNumDecimalPlacesToDisplay(0);
    nearendSensitivitySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    // Applies live (see PluginProcessor.h) -- still only commit at drag-end
    // or on a discrete text entry (guarded onValueChange, which also covers
    // keyboard/text-box edits that never trigger onDragEnd), never
    // per-pixel mid-drag: no rebuild either way now, but reconstructing
    // SuppressionGain on every pixel of a drag would still be wasteful.
    nearendSensitivitySlider.onDragEnd = [this] { nearendSensitivitySliderChanged(); };
    nearendSensitivitySlider.onValueChange = [this] {
        if (!nearendSensitivitySlider.isMouseButtonDown()) nearendSensitivitySliderChanged();
    };
    addAndMakeVisible(nearendSensitivitySlider);

    addAndMakeVisible(protectionHoldTimeLabel);

    protectionHoldTimeSlider.setRange(40.0, 800.0);
    protectionHoldTimeSlider.setTextValueSuffix(" ms");
    protectionHoldTimeSlider.setNumDecimalPlacesToDisplay(0);
    protectionHoldTimeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    protectionHoldTimeSlider.onDragEnd = [this] { protectionHoldTimeSliderChanged(); };
    protectionHoldTimeSlider.onValueChange = [this] {
        if (!protectionHoldTimeSlider.isMouseButtonDown()) protectionHoldTimeSliderChanged();
    };
    addAndMakeVisible(protectionHoldTimeSlider);

    addAndMakeVisible(transitionSmoothingLabel);

    transitionSmoothingSlider.setRange(0.0, 200.0);
    transitionSmoothingSlider.setTextValueSuffix(" ms");
    transitionSmoothingSlider.setNumDecimalPlacesToDisplay(0);
    transitionSmoothingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    transitionSmoothingSlider.onDragEnd = [this] { transitionSmoothingSliderChanged(); };
    transitionSmoothingSlider.onValueChange = [this] {
        if (!transitionSmoothingSlider.isMouseButtonDown()) transitionSmoothingSliderChanged();
    };
    addAndMakeVisible(transitionSmoothingSlider);

    addAndMakeVisible(hpfLabel);

    hpfSlider.setRange(80.0, 300.0);
    hpfSlider.setSkewFactorFromMidPoint(150.0);
    hpfSlider.setTextValueSuffix(" Hz");
    hpfSlider.setNumDecimalPlacesToDisplay(0);
    hpfSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    hpfSlider.onValueChange = [this] { hpfSliderChanged(); };
    addAndMakeVisible(hpfSlider);

    addAndMakeVisible(referenceGainLabel);

    referenceGainSlider.setRange(-24.0, 24.0);
    referenceGainSlider.setTextValueSuffix(" dB");
    referenceGainSlider.setNumDecimalPlacesToDisplay(1);
    referenceGainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    referenceGainSlider.onValueChange = [this] { referenceGainSliderChanged(); };
    addAndMakeVisible(referenceGainSlider);

    addAndMakeVisible(dryWetLabel);

    dryWetSlider.setRange(0.0, 100.0);
    dryWetSlider.setTextValueSuffix(" %");
    dryWetSlider.setNumDecimalPlacesToDisplay(0);
    dryWetSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    dryWetSlider.onValueChange = [this] { dryWetSliderChanged(); };
    addAndMakeVisible(dryWetSlider);

    // Centred, unlike the control labels: each of these sits directly above
    // its own narrow meter bar, which is itself centred in its column.
    // Font sizes for every label are set in resized(), so they track the
    // window scale.
    for (auto* label : { &inputMeterLabel, &sidechainMeterLabel, &outputMeterLabel, &suppressionMeterLabel }) {
        label->setJustificationType(juce::Justification::centred);
        label->setColour(juce::Label::textColourId, LessPAColours::secondaryText);
        addAndMakeVisible(label);
    }
    addAndMakeVisible(inputMeter);
    addAndMakeVisible(sidechainMeter);
    addAndMakeVisible(outputMeter);
    addAndMakeVisible(suppressionMeter);

    metersPostFilterToggle.onClick = [this] { metersPostFilterToggleChanged(); };
    addAndMakeVisible(metersPostFilterToggle);

    // Left-aligned rather than centred on the window: it shares the bottom
    // strip with the corner wordmark (see computeLogoBounds()), so it is laid
    // out in the space to the left of the mark and lines up with the control
    // column above it instead of sitting visibly off-centre.
    delayReadoutLabel.setJustificationType(juce::Justification::centredLeft);
    delayReadoutLabel.setColour(juce::Label::textColourId, LessPAColours::secondaryText);
    addAndMakeVisible(delayReadoutLabel);

    // A few words each, deliberately much shorter than the corresponding
    // section of showHelpDialog(): a tooltip answers "what is this?" while the
    // mouse is already on the control, the dialog answers "how should I set
    // it?". Duplicating the paragraphs here would make the tooltips too slow
    // to read to be any use mid-show -- and the window is only 340px wide, so
    // a sentence-length tip is a multi-line block sitting over the controls.
    // (LessPALookAndFeel::getTooltipBounds now wraps rather than clips, so
    // length is a readability choice here rather than a correctness one.)
    helpButton.setTooltip("Full control reference");
    tailLengthCombo.setTooltip("Match to venue reverb tail");
    suppressionStrengthCombo.setTooltip("Residual echo cleanup");
    limitHfGainToggle.setTooltip("Clamp HF while converging");
    nearendDetectorCombo.setTooltip("How audience is detected");
    nearendSensitivitySlider.setTooltip("Bias toward keeping audience");
    protectionHoldTimeSlider.setTooltip("How long protection lasts");
    transitionSmoothingSlider.setTooltip("Anti-pump crossfade");
    hpfSlider.setTooltip("Cutoff on mic + reference");
    referenceGainSlider.setTooltip("Reference gain only");
    dryWetSlider.setTooltip("Blend cancelled vs original");
    metersPostFilterToggle.setTooltip("Meter before or after HPF");
    inputMeter.setTooltip("Mic input");
    sidechainMeter.setTooltip("PA reference input");
    suppressionMeter.setTooltip("Reduction, in vs out");
    outputMeter.setTooltip("After cancellation");
    delayReadoutLabel.setTooltip("Echo path delay estimate");

    updateTailLengthCombo();
    updateSuppressionStrengthCombo();
    updateLimitHfGainToggle();
    updateNearendDetectorCombo();
    updateNearendSensitivitySlider();
    updateProtectionHoldTimeSlider();
    updateTransitionSmoothingSlider();
    updateHpfSlider();
    updateReferenceGainSlider();
    updateMetersPostFilterToggle();
    updateDryWetSlider();
    startTimerHz(30); // meter ballistics + keeps controls in sync with host automation
}

PAEchoCancellerAudioProcessorEditor::~PAEchoCancellerAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr); // JUCE asserts if a component is destroyed still holding one
}

float PAEchoCancellerAudioProcessorEditor::getUiScale() const
{
    // One factor is enough because the constrainer locks the aspect ratio.
    // Taken from height rather than width: every layout constant is a row
    // height, and the horizontal axis is mostly fractional/stretch anyway.
    return static_cast<float>(getHeight()) / static_cast<float>(designHeight);
}

PAEchoCancellerAudioProcessorEditor::SectionBounds
PAEchoCancellerAudioProcessorEditor::computeSectionBounds() const
{
    const float scale = getUiScale();
    const auto sc = [scale](int designPx) { return juce::roundToInt(scale * static_cast<float>(designPx)); };

    auto content = getLocalBounds().reduced(sc(outerMargin));

    SectionBounds bounds;
    bounds.title = content.removeFromTop(sc(titleStripHeight));
    content.removeFromTop(sc(titleGap));
    bounds.input = content.removeFromTop(sc(inputSectionHeight));
    content.removeFromTop(sc(sectionGap));
    bounds.adaptive = content.removeFromTop(sc(adaptiveSectionHeight));
    content.removeFromTop(sc(sectionGap));
    bounds.residual = content.removeFromTop(sc(residualSectionHeight));
    content.removeFromTop(sc(sectionGap));
    bounds.doubleTalk = content.removeFromTop(sc(doubleTalkSectionHeight));
    content.removeFromTop(sc(sectionGap));
    bounds.ungrouped = content.removeFromTop(sc(ungroupedHeight));
    return bounds;
}

juce::Rectangle<int> PAEchoCancellerAudioProcessorEditor::computeLogoBounds() const
{
    const float scale = getUiScale();
    const int width = juce::roundToInt(scale * static_cast<float>(logoWidth));
    const int height = juce::roundToInt(static_cast<float>(width) / logoAspect);

    // Hard against the bottom-right of the content area. The delay readout
    // shares this strip and is laid out around this rectangle in resized(),
    // so the two cannot overlap at any point in the resize range -- the whole
    // window scales by one factor, so their relative widths never change.
    const auto content = getLocalBounds().reduced(juce::roundToInt(scale * static_cast<float>(outerMargin)));
    return { content.getRight() - width, content.getBottom() - height, width, height };
}

void PAEchoCancellerAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float scale = getUiScale();
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    const auto sections = computeSectionBounds();

    // Panels, no border: an outline on top of a fill would read as busier
    // without adding any information the fill doesn't already carry, which
    // works against the point of the style. The fill alone is enough contrast
    // against #14161A to separate the four stages.
    //
    // sections.ungrouped is deliberately absent: the output stage is left on
    // the bare background so that "these four boxes are the processing chain"
    // stays readable at a glance.
    g.setColour(LessPAColours::panel);
    for (const auto& panel : { sections.input, sections.adaptive, sections.residual, sections.doubleTalk })
        g.fillRoundedRectangle(panel.toFloat(), 6.0f * scale);

    // A meter bridge, not a fifth section: LevelMeterComponent draws its idle
    // trough in the *window* background (deliberately darker than a panel, so
    // a large idle meter reads as a recessed slot rather than disappearing),
    // which needs something lighter behind it -- and with the metering panel
    // gone the meters would otherwise sit on that exact same colour and
    // vanish. Sized to the bars themselves rather than the full strip width so
    // it reads as an instrument rather than a headerless section panel.
    //
    // Taken from the meters' own bounds rather than re-deriving the row
    // arithmetic here, so it cannot drift from where the Grid actually put
    // them. Degenerate (and harmless) only before the first resized().
    const auto meterBars = inputMeter.getBounds().getUnion(outputMeter.getBounds());
    if (!meterBars.isEmpty())
        g.fillRoundedRectangle(meterBars.expanded(juce::roundToInt(10.0f * scale),
                                                  juce::roundToInt(6.0f * scale))
                                   .toFloat(),
                               6.0f * scale);

    // Left-aligned on the same x as every section header below it, and trimmed
    // clear of the help button on the right.
    auto titleArea = sections.title.reduced(juce::roundToInt(scale * static_cast<float>(panelPaddingX)), 0)
                         .withTrimmedRight(juce::roundToInt(30.0f * scale));

    g.setColour(LessPAColours::primaryText);
    g.setFont(juce::FontOptions(23.0f * scale).withStyle("Bold"));
    g.drawFittedText("Less PA", titleArea.removeFromTop(juce::roundToInt(38.0f * scale))
                                    .withTrimmedTop(juce::roundToInt(6.0f * scale)),
                     juce::Justification::centredLeft, 1);

    // The subtitle, not the wordmark, is what tells someone opening the plugin
    // cold what it does -- the help dialog covers the engine behind it.
    g.setColour(LessPAColours::secondaryText);
    g.setFont(juce::FontOptions(11.5f * scale));
    g.drawFittedText("PA bleed removal for audience mics", titleArea,
                     juce::Justification::centredLeft, 1);

    // The company mark, small and last in the reading order. Placement::centred
    // scales to fit while preserving aspect, so the ~4:1 wordmark fills the
    // rectangle computeLogoBounds() already sized to that ratio.
    if (logoImage.isValid())
        g.drawImage(logoImage, computeLogoBounds().toFloat(), juce::RectanglePlacement::centred);
}

void PAEchoCancellerAudioProcessorEditor::resized()
{
    const float scale = getUiScale();
    const auto sc = [scale](int designPx) { return juce::roundToInt(scale * static_cast<float>(designPx)); };

    const auto sections = computeSectionBounds();

    auto titleArea = sections.title;
    helpButton.setBounds(titleArea.removeFromRight(sc(24)).withSizeKeepingCentre(sc(24), sc(24)));

    // Fonts are the one part of the layout Grid can't scale for us, and a
    // stale text-box size would silently cap the value text at the default
    // size. Both are skipped when unchanged so the common
    // "same size, re-laid out" path doesn't rebuild the sliders' text boxes.
    for (auto* label : { &inputSectionLabel, &adaptiveSectionLabel,
                         &residualSectionLabel, &doubleTalkSectionLabel })
        label->setFont(juce::FontOptions(scale * 11.0f).withStyle("Bold"));

    for (auto* label : { &tailLengthLabel, &suppressionStrengthLabel, &nearendDetectorLabel,
                         &nearendSensitivityLabel, &protectionHoldTimeLabel, &transitionSmoothingLabel,
                         &hpfLabel, &referenceGainLabel, &dryWetLabel })
        label->setFont(juce::FontOptions(scale * 12.5f));

    for (auto* label : { &inputMeterLabel, &sidechainMeterLabel, &outputMeterLabel,
                         &suppressionMeterLabel, &delayReadoutLabel })
        label->setFont(juce::FontOptions(scale * 12.0f));

    juce::Slider* const sliders[] = { &nearendSensitivitySlider, &protectionHoldTimeSlider,
                                      &transitionSmoothingSlider, &hpfSlider,
                                      &referenceGainSlider, &dryWetSlider };
    const int textBoxWidth = sc(60);
    const int textBoxHeight = sc(24);
    for (auto* slider : sliders)
        if (slider->getTextBoxWidth() != textBoxWidth || slider->getTextBoxHeight() != textBoxHeight)
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, textBoxWidth, textBoxHeight);

    // A labelled slider is always a 16px label sitting directly on top of its
    // own 26px slider, with the separating air above the pair rather than
    // between them -- that pairing is what makes a column of sliders readable.
    struct LabelledSlider { juce::Label* label; juce::Slider* slider; };
    const auto addSliderRows = [](SectionGrid& grid, std::initializer_list<LabelledSlider> rows) {
        for (auto& labelled : rows) {
            grid.gap(10); // separates this pair from whatever precedes it
            grid.row(controlLabelRow, *labelled.label);
            grid.row(sliderRow, *labelled.slider); // no gap: the label belongs to this slider
        }
    };

    // INPUT CONDITIONING -- 18 + 2 * (10 + 16 + 26) = 122 rows,
    // + 2 * panelPaddingY = inputSectionHeight (142). No separate gap after
    // the header here: the first pair's own leading 10px already provides it.
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, inputSectionLabel);
        addSliderRows(grid, { { &hpfLabel, &hpfSlider },
                              { &referenceGainLabel, &referenceGainSlider } });
        grid.performLayout(sections.input.reduced(sc(panelPaddingX), sc(panelPaddingY)));
    }

    // ADAPTIVE FILTER -- 18 + 6 + 16 + 28 = 68 rows,
    // + 2 * panelPaddingY = adaptiveSectionHeight (88).
    //
    // One control on purpose: Tail Length *is* the linear filter length. It is
    // the only thing in the plugin that touches the adaptive filter, and the
    // only one that interrupts audio when changed, so folding it into a
    // neighbouring section would misrepresent two distinct DSP stages.
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, adaptiveSectionLabel);
        grid.gap(6);
        grid.row(controlLabelRow, tailLengthLabel);
        grid.row(comboRow, tailLengthCombo);
        grid.performLayout(sections.adaptive.reduced(sc(panelPaddingX), sc(panelPaddingY)));
    }

    // RESIDUAL SUPPRESSION -- 18 + 6 + 16 + 28 + 12 + 24 = 104 rows,
    // + 2 * panelPaddingY = residualSectionHeight (124).
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, residualSectionLabel);
        grid.gap(6);
        grid.row(controlLabelRow, suppressionStrengthLabel);
        grid.row(comboRow, suppressionStrengthCombo);
        grid.gap(12);
        grid.row(toggleRow, limitHfGainToggle);
        grid.performLayout(sections.residual.reduced(sc(panelPaddingX), sc(panelPaddingY)));
    }

    // DOUBLE-TALK PROTECTION -- 18 + 6 + 16 + 28 + 3 * (10 + 16 + 26) = 224
    // rows, + 2 * panelPaddingY = doubleTalkSectionHeight (244).
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, doubleTalkSectionLabel);
        grid.gap(6);
        grid.row(controlLabelRow, nearendDetectorLabel);
        grid.row(comboRow, nearendDetectorCombo);
        addSliderRows(grid, { { &nearendSensitivityLabel, &nearendSensitivitySlider },
                              { &protectionHoldTimeLabel, &protectionHoldTimeSlider },
                              { &transitionSmoothingLabel, &transitionSmoothingSlider } });
        grid.performLayout(sections.doubleTalk.reduced(sc(panelPaddingX), sc(panelPaddingY)));
    }

    // OUTPUT STAGE, ungrouped -- (16 + 26) + 12 + 16 + 4 + 94 + 12 + 24 + 8 + 18
    // = 230 rows = ungroupedHeight. No panel and no header: a heading over a
    // row of meters is noise to the operator this is built for.
    {
        auto ungrouped = sections.ungrouped;

        // Bottom strip first, because the readout has to be fitted around the
        // corner wordmark rather than laid out independently of it.
        auto bottomStrip = ungrouped.removeFromBottom(sc(readoutRow));
        delayReadoutLabel.setBounds(bottomStrip
                                        .withTrimmedLeft(sc(panelPaddingX))
                                        .withRight(computeLogoBounds().getX() - sc(readoutLogoGap)));
        ungrouped.removeFromBottom(sc(8));

        // Four columns rather than hand-computed column arithmetic: the meter
        // bar stays narrow (22px, like a real channel-strip meter) while its
        // label gets the whole column, which is what "PA-ref(sc)" and
        // "Suppression" need to read at 12pt. Full-width rows simply span all
        // four columns.
        SectionGrid grid(4, scale, 8);
        grid.row(controlLabelRow, dryWetLabel);
        grid.row(sliderRow, dryWetSlider);
        grid.gap(12);
        grid.row(controlLabelRow, { &inputMeterLabel, &sidechainMeterLabel,
                                    &suppressionMeterLabel, &outputMeterLabel });
        grid.gap(4); // clears the meter bridge's top edge (see paint())
        grid.row(meterBarRow, { &inputMeter, &sidechainMeter, &suppressionMeter, &outputMeter }, 22);
        grid.gap(12); // ditto, its bottom edge
        grid.row(toggleRow, metersPostFilterToggle);
        grid.performLayout(ungrouped.reduced(sc(panelPaddingX), 0));
    }
}

void PAEchoCancellerAudioProcessorEditor::tailLengthComboChanged()
{
    const int index = tailLengthCombo.getSelectedId() - 1; // JUCE item IDs are 1-based
    auto* param = processor.getTailLengthParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(index));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateTailLengthCombo()
{
    const int currentId = processor.getTailLengthParameter()->getIndex() + 1;
    if (tailLengthCombo.getSelectedId() != currentId)
        tailLengthCombo.setSelectedId(currentId, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::suppressionStrengthComboChanged()
{
    const int index = suppressionStrengthCombo.getSelectedId() - 1; // JUCE item IDs are 1-based
    auto* param = processor.getSuppressionStrengthParameter();
    const float normalized = static_cast<float>(index) / 2.0f; // 3 choices: 0, 0.5, 1
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateSuppressionStrengthCombo()
{
    const int currentId = processor.getSuppressionStrengthParameter()->getIndex() + 1;
    if (suppressionStrengthCombo.getSelectedId() != currentId)
        suppressionStrengthCombo.setSelectedId(currentId, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::limitHfGainToggleChanged()
{
    auto* param = processor.getLimitHfGainParameter();
    param->beginChangeGesture();
    param->setValueNotifyingHost(limitHfGainToggle.getToggleState() ? 1.0f : 0.0f);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateLimitHfGainToggle()
{
    const bool current = processor.getLimitHfGainParameter()->get();
    if (limitHfGainToggle.getToggleState() != current)
        limitHfGainToggle.setToggleState(current, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::nearendDetectorComboChanged()
{
    const int index = nearendDetectorCombo.getSelectedId() - 1; // JUCE item IDs are 1-based
    auto* param = processor.getNearendDetectorParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(index));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateNearendDetectorCombo()
{
    const int currentId = processor.getNearendDetectorParameter()->getIndex() + 1;
    if (nearendDetectorCombo.getSelectedId() != currentId)
        nearendDetectorCombo.setSelectedId(currentId, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::nearendSensitivitySliderChanged()
{
    auto* param = processor.getNearendSensitivityParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(nearendSensitivitySlider.getValue()));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateNearendSensitivitySlider()
{
    // Skip while actively dragging: the parameter is deliberately not
    // updated until drag-end (see the constructor wiring), so syncing from
    // it mid-drag would snap the slider back to the pre-drag value 30
    // times a second, fighting the user's own gesture.
    if (nearendSensitivitySlider.isMouseButtonDown())
        return;
    const double currentPercent = static_cast<double>(processor.getNearendSensitivityParameter()->get());
    if (std::abs(nearendSensitivitySlider.getValue() - currentPercent) > 0.05)
        nearendSensitivitySlider.setValue(currentPercent, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::protectionHoldTimeSliderChanged()
{
    auto* param = processor.getProtectionHoldTimeParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(protectionHoldTimeSlider.getValue()));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateProtectionHoldTimeSlider()
{
    // See the same guard in updateNearendSensitivitySlider() -- avoids
    // fighting an in-progress drag whose value hasn't committed yet.
    if (protectionHoldTimeSlider.isMouseButtonDown())
        return;
    const double currentMs = static_cast<double>(processor.getProtectionHoldTimeParameter()->get());
    if (std::abs(protectionHoldTimeSlider.getValue() - currentMs) > 0.5)
        protectionHoldTimeSlider.setValue(currentMs, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::transitionSmoothingSliderChanged()
{
    auto* param = processor.getTransitionSmoothingParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(transitionSmoothingSlider.getValue()));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateTransitionSmoothingSlider()
{
    // See the same guard in updateNearendSensitivitySlider().
    if (transitionSmoothingSlider.isMouseButtonDown())
        return;
    const double currentMs = static_cast<double>(processor.getTransitionSmoothingParameter()->get());
    if (std::abs(transitionSmoothingSlider.getValue() - currentMs) > 0.5)
        transitionSmoothingSlider.setValue(currentMs, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::hpfSliderChanged()
{
    auto* param = processor.getHpfFrequencyParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(hpfSlider.getValue()));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateHpfSlider()
{
    const double currentHz = static_cast<double>(processor.getHpfFrequencyParameter()->get());
    if (std::abs(hpfSlider.getValue() - currentHz) > 0.05)
        hpfSlider.setValue(currentHz, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::referenceGainSliderChanged()
{
    auto* param = processor.getReferenceGainParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(referenceGainSlider.getValue()));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateReferenceGainSlider()
{
    const double currentDb = static_cast<double>(processor.getReferenceGainParameter()->get());
    if (std::abs(referenceGainSlider.getValue() - currentDb) > 0.05)
        referenceGainSlider.setValue(currentDb, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::metersPostFilterToggleChanged()
{
    auto* param = processor.getMetersPostFilterParameter();
    param->beginChangeGesture();
    param->setValueNotifyingHost(metersPostFilterToggle.getToggleState() ? 1.0f : 0.0f);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateMetersPostFilterToggle()
{
    const bool current = processor.getMetersPostFilterParameter()->get();
    if (metersPostFilterToggle.getToggleState() != current)
        metersPostFilterToggle.setToggleState(current, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::dryWetSliderChanged()
{
    auto* param = processor.getDryWetMixParameter();
    const float normalized = param->convertTo0to1(static_cast<float>(dryWetSlider.getValue()));
    param->beginChangeGesture();
    param->setValueNotifyingHost(normalized);
    param->endChangeGesture();
}

void PAEchoCancellerAudioProcessorEditor::updateDryWetSlider()
{
    const double currentPercent = static_cast<double>(processor.getDryWetMixParameter()->get());
    if (std::abs(dryWetSlider.getValue() - currentPercent) > 0.05)
        dryWetSlider.setValue(currentPercent, juce::dontSendNotification);
}

void PAEchoCancellerAudioProcessorEditor::timerCallback()
{
    updateTailLengthCombo();
    updateSuppressionStrengthCombo();
    updateLimitHfGainToggle();
    updateNearendDetectorCombo();
    updateNearendSensitivitySlider();
    updateProtectionHoldTimeSlider();
    updateTransitionSmoothingSlider();
    updateHpfSlider();
    updateReferenceGainSlider();
    updateMetersPostFilterToggle();
    updateDryWetSlider();

    // Two different ways "nothing is happening" can look, both of which
    // need catching:
    //  - the host has stopped calling processBlock at all (e.g. plugin
    //    window closed mid-render) -- the counter stops advancing, and
    //    every getter below just returns whatever it last held.
    //  - the host keeps calling processBlock with silence even when the
    //    transport is stopped (common -- the audio engine keeps running),
    //    so the counter keeps advancing, but there's no real signal.
    const uint32_t currentCount = processor.getProcessBlockCallCount();
    const bool callbackAdvanced = (currentCount != lastSeenProcessBlockCount);
    lastSeenProcessBlockCount = currentCount;

    // Silence detection always uses the pre-filter level, regardless of
    // what the meters are currently displaying -- an aggressive HPF cutoff
    // shouldn't be able to make real signal look like "nothing happening".
    const float inputLevelPre = processor.getInputPeakLevelPre();
    const float sidechainLevelPre = processor.getSidechainPeakLevelPre();
    constexpr float silenceThreshold = 1.0e-6f;
    const bool signalPresent = inputLevelPre > silenceThreshold || sidechainLevelPre > silenceThreshold;

    const bool active = callbackAdvanced && signalPresent;
    staleTickCount = active ? 0 : (staleTickCount + 1);

    if (staleTickCount > staleTicksBeforeClear) {
        inputMeter.setLevel(0.0f);
        sidechainMeter.setLevel(0.0f);
        outputMeter.setLevel(0.0f);
        suppressionMeter.setLevel(0.0f);
        delayReadoutLabel.setText("PA delay: --", juce::dontSendNotification);
        return;
    }

    // AEC3's live echo-path delay estimate. A steady value = the delay
    // estimator has a solid lock on the PA-to-mic relationship; "--" with
    // signal present = the reference likely isn't reaching the plugin at
    // all (e.g. sidechain pins not routed); a value that keeps jumping =
    // the reference/mic timing itself is unstable. The median (1s
    // aggregation) is shown alongside the instantaneous value so a
    // momentary excursion is distinguishable from a genuine change.
    {
        const int delayMs = processor.getEstimatedEchoPathDelayMs();
        const int medianMs = processor.getEchoPathDelayMedianMs();
        juce::String text("PA delay: ");
        if (delayMs < 0)
            text << "--";
        else {
            text << delayMs << " ms";
            if (medianMs >= 0)
                text << "  (median " << medianMs << ")";
        }
        delayReadoutLabel.setText(text, juce::dontSendNotification);
    }

    const bool postFilter = processor.getMetersPostFilterParameter()->get();
    const float displayedInputLevel = postFilter ? processor.getInputPeakLevelPost() : inputLevelPre;
    const float displayedOutputLevel = processor.getOutputPeakLevel();
    const float displayedSidechainLevel = postFilter ? processor.getSidechainPeakLevelPost() : sidechainLevelPre;
    inputMeter.setLevel(displayedInputLevel);
    sidechainMeter.setLevel(displayedSidechainLevel);
    outputMeter.setLevel(displayedOutputLevel);

    // Suppression is measured directly from peak levels (dB reduction from
    // input to output), rather than AEC3's own ERLE statistic. ERLE only
    // reflects the linear adaptive filter's internal echo reduction -- it
    // doesn't account for the nonlinear suppression stage that runs after it
    // (the stage Suppression Strength/Near-end Sensitivity tune) -- so it
    // never lined up with what Input/Output actually showed. Measuring it
    // directly makes it sum correctly by construction, at the cost of also
    // picking up any level change from Dry/Wet Mix or plain near-end
    // dynamics, not only echo removal.
    //
    // The Input side always uses the post-HPF, delay-compensated reading
    // (regardless of what "Meters post HPF" has the Input meter itself
    // showing) -- Output always reflects Input from getLatencySamples()
    // earlier (the internal frame-buffering latency plus AEC3's own
    // internal processing delay -- getLatencySamples() reports the true
    // total, see that getter's comment), so comparing it against the
    // *live* Input peak makes fast transients (kick/snare) look heavily
    // suppressed when they simply haven't reached the output yet. See
    // getInputPeakLevelPostDelayed()'s comment.
    constexpr float levelFloorLinear = 1.0e-6f; // -120dB, matches the silence threshold above
    constexpr float suppressionJitterFloorDb = 0.5f;
    const float delayCompensatedInputLevel =
        processor.getInputPeakLevelPostDelayed(processor.getLatencySamples());
    const float suppressionDb =
        juce::Decibels::gainToDecibels(juce::jmax(delayCompensatedInputLevel, levelFloorLinear))
        - juce::Decibels::gainToDecibels(juce::jmax(displayedOutputLevel, levelFloorLinear));

    // With no PA reference at all, there's nothing to suppress by
    // definition -- forcing a hard 0 here (rather than trusting the
    // measurement to settle there on its own) reads as more trustworthy
    // than the small residual jitter/decay tail the raw calculation can
    // still show right as the reference cuts out.
    const bool referenceSilent = displayedSidechainLevel <= silenceThreshold;
    suppressionMeter.setLevel(!referenceSilent && suppressionDb > suppressionJitterFloorDb ? suppressionDb : 0.0f);
}

void PAEchoCancellerAudioProcessorEditor::showHelpDialog()
{
    // Mirrors the panel exactly -- same four stages, same order within each.
    // The panel is laid out as the real signal chain rather than as a flat
    // list, so the help reads as an explanation of that chain instead of an
    // alphabetical glossary that would send you hunting.
    static const juce::String helpText =
        "LESS PA cancels PA speaker leakage out of an audience microphone, "
        "using the PA feed itself (not a room estimate of it) as a reference "
        "-- wire the PA signal into the Reference sidechain input.\n"
        "\n"
        "The panel follows the signal chain, top to bottom: what reaches the "
        "canceller, then the linear filter that models the leakage and "
        "subtracts it, then the suppressor that cleans up whatever the filter "
        "missed, then the detector that decides when to ease that suppressor "
        "off to protect real audience sound.\n"
        "\n"
        "\n"
        "=== INPUT CONDITIONING ===\n"
        "What the canceller sees, before it does anything else.\n"
        "\n"
        "INPUT HPF\n"
        "A 24dB/octave high-pass filter (80-300Hz), applied identically to "
        "the microphone input and the PA reference before cancellation, to "
        "remove rumble/handling noise that could otherwise confuse the "
        "canceller.\n"
        "\n"
        "PA REFERENCE TRIM\n"
        "A plain gain trim on the reference signal -- use it if your PA "
        "feed is clipping or too quiet for the canceller to get a good "
        "read. Not a substitute for Suppression Strength.\n"
        "\n"
        "\n"
        "=== ADAPTIVE FILTER ===\n"
        "The linear filter that models the PA-to-microphone path and "
        "subtracts it. This is the part that removes leakage cleanly, "
        "without touching anything else in the mic.\n"
        "\n"
        "TAIL LENGTH\n"
        "How long a reverb tail the canceller can model, matched to your "
        "venue: 50ms (dry room) up to 800ms (very large hall). Longer "
        "costs a little more CPU per instance.\n"
        "\n"
        "\n"
        "=== RESIDUAL SUPPRESSION ===\n"
        "Cleans up whatever the filter above could not remove. This stage "
        "works by reducing gain, so it is also where artifacts come from if "
        "pushed too hard.\n"
        "\n"
        "SUPPRESSION STRENGTH\n"
        "Gentle / Moderate / Hard. How aggressively residual echo gets "
        "cleaned up after the main cancellation. Hard removes the most "
        "leakage but can sound artifacty on ambient crowd noise that shares "
        "energy with the PA signal; Gentle sounds more natural but lets a "
        "bit more leakage through. Applies live -- no interruption when "
        "changed.\n"
        "\n"
        "LIMIT HF GAIN\n"
        "An extra safety clamp on high-frequency gain, for when the filter "
        "hasn't fully converged yet. Usually leave off; try it if you hear "
        "excess high-end leakage. Applies live.\n"
        "\n"
        "\n"
        "=== DOUBLE-TALK PROTECTION ===\n"
        "Decides when a moment is genuine audience sound worth keeping, and "
        "eases the suppressor off when it is. This is what stops the crowd "
        "from being gated away along with the PA.\n"
        "\n"
        "NEAR-END DETECTOR\n"
        "Which method decides that a moment is genuine audience content. "
        "Classic watches the overall low-frequency balance and protects "
        "most of the time -- the safe default. Subband (2-4kHz) compares "
        "bass against the 2-4kHz range where crowd sound actually lives, "
        "so it protects less often but much more specifically in the gaps "
        "between PA content. They are different characters, not a "
        "better/worse pair -- A/B them by ear on your material. Applies "
        "live.\n"
        "\n"
        "NEAR-END SENSITIVITY\n"
        "How readily the active detector decides a moment is genuine "
        "audience content worth protecting, rather than echo to remove. "
        "Higher lets more real audience sound through during simultaneous "
        "PA + crowd moments. Works for both detector choices. Applies "
        "live.\n"
        "\n"
        "PROTECTION HOLD TIME\n"
        "How long that protection lasts after it triggers before reverting "
        "to normal suppression. Longer is smoother but can let a bit more "
        "PA leakage through right after the crowd quiets down; shorter "
        "reacts faster but can pulse if crowd noise is intermittent. "
        "Applies live.\n"
        "\n"
        "TRANSITION SMOOTHING\n"
        "How gradually the plugin moves between protecting audience sound "
        "and suppressing PA bleed, when it changes its mind about which one "
        "it's hearing. At 0ms it switches instantly, which can be heard as "
        "pumping; raising it rounds that transition off. The trade-off is "
        "that protection also engages and releases more gradually, so very "
        "long settings can let a little more bleed through right after the "
        "crowd quiets down. Applies live.\n"
        "\n"
        "\n"
        "=== OUTPUT AND METERING ===\n"
        "\n"
        "DRY/WET MIX\n"
        "Blends the cancelled (wet) output with the original, unprocessed "
        "(dry) input. 100% is fully cancelled; lower it to let more of the "
        "original signal (including any residual PA leakage) back in to "
        "taste.\n"
        "\n"
        "METERS\n"
        "Input / PA-ref(sc) / Suppression / Output. Suppression is measured "
        "directly as the dB difference between the Input and Output meters, "
        "so it always matches what you actually hear: near 0dB when there's "
        "no PA leakage to remove, higher when there is. \"Meters post HPF\" "
        "switches Input and PA-ref between showing levels before or after "
        "the high-pass filter.\n"
        "\n"
        "PA DELAY\n"
        "The canceller's own estimate of how far the PA reference leads the "
        "leakage arriving in the mic. Use it as a wiring check and a health "
        "check: a steady number means the reference is arriving and the "
        "canceller has a solid lock; \"--\" with signal present usually "
        "means no reference is reaching the plugin at all (check the "
        "sidechain routing in your host); a value that keeps jumping around "
        "means the reference and mic timing is unstable.\n"
        "\n"
        "\n"
        "Tail Length is the only control that briefly interrupts audio when "
        "changed (it rebuilds the adaptive filter); everything else applies "
        "without any dropout.";

    auto* content = new juce::TextEditor();
    content->setMultiLine(true);
    content->setReadOnly(true);
    content->setScrollbarsShown(true);
    content->setCaretVisible(false);
    content->setPopupMenuEnabled(true);
    content->setFont(juce::FontOptions(14.0f));
    content->setText(helpText, false);
    content->setSize(420, 520);

    // Colours are set on the editor directly rather than by handing it our
    // LookAndFeel: the DialogWindow is a separate top-level window (so it does
    // not inherit ours) and it is launched async, so it can outlive this editor
    // -- pointing it at a member of ours would leave a dangling LookAndFeel.
    content->setColour(juce::TextEditor::backgroundColourId, LessPAColours::panel);
    content->setColour(juce::TextEditor::textColourId, LessPAColours::primaryText);
    content->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    content->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    content->setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    content->setColour(juce::TextEditor::highlightColourId, LessPAColours::accent.withAlpha(0.35f));
    content->setColour(juce::TextEditor::highlightedTextColourId, LessPAColours::primaryText);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(content);
    options.dialogTitle = "Less PA -- Help";
    options.dialogBackgroundColour = LessPAColours::windowBackground;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}
