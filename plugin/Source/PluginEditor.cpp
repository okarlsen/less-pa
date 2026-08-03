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
    // A non-zero firstColumnWidthDesignPx pins the first column to that fixed
    // width and lets the rest share what is left. That is what a row of
    // "label: bar" meters needs -- the four labels have to line up as a block
    // whatever the bar lengths are, which equal fr columns cannot promise.
    SectionGrid(int numColumnsIn, float scaleIn, int columnGapDesignPx,
                int firstColumnWidthDesignPx = 0)
        : numColumns(numColumnsIn), scale(scaleIn)
    {
        grid.rowGap = juce::Grid::Px(0);
        grid.columnGap = juce::Grid::Px(scaled(columnGapDesignPx));

        for (int i = 0; i < numColumns; ++i)
            grid.templateColumns.add(i == 0 && firstColumnWidthDesignPx > 0
                                         ? juce::Grid::TrackInfo(juce::Grid::Px(scaled(firstColumnWidthDesignPx)))
                                         : juce::Grid::TrackInfo(juce::Grid::Fr(1)));
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

    // One meter: its name in the fixed first column, its bar filling the rest
    // of the row. The bar is deliberately shorter than the row it sits in --
    // four full-height bars stacked with only a gap between them read as one
    // striped block rather than four instruments.
    void meterRow(int designPx, juce::Component& label, juce::Component& bar, int barHeightDesignPx)
    {
        const int line = addTrack(designPx);
        grid.items.add(juce::GridItem(label).withArea(line, 1));
        grid.items.add(juce::GridItem(bar)
                           .withArea(line, 2, line + 1, numColumns + 1)
                           .withHeight(static_cast<float>(scaled(barHeightDesignPx)))
                           .withAlignSelf(juce::GridItem::AlignSelf::center));
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
    constexpr int minWidth = 640;                   // ~0.83x
    constexpr int maxWidth = 1150;                  // ~1.50x
    static_assert(minWidth <= designWidth && designWidth <= maxWidth,
                  "the default design size must be inside its own resize limits");
    setResizeLimits(minWidth, scaledHeight(minWidth), maxWidth, scaledHeight(maxWidth));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(static_cast<double>(designWidth) / static_cast<double>(designHeight));

    helpButton.onClick = [this] { showHelpDialog(); };
    addAndMakeVisible(helpButton);

    versionLabel.setText("v" JucePlugin_VersionString, juce::dontSendNotification);
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setColour(juce::Label::textColourId, LessPAColours::secondaryText);
    addAndMakeVisible(versionLabel);

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

    // Left-aligned like the control labels: each meter name now sits to the
    // left of its own horizontal bar in a fixed-width column, so the four read
    // as a list. Font sizes for every label are set in resized(), so they
    // track the window scale.
    for (auto* label : { &inputMeterLabel, &sidechainMeterLabel, &outputMeterLabel, &suppressionMeterLabel }) {
        label->setJustificationType(juce::Justification::centredLeft);
        label->setColour(juce::Label::textColourId, LessPAColours::secondaryText);
        addAndMakeVisible(label);
    }

    // Horizontal rather than the component's default vertical: four
    // side-by-side vertical bars needed the whole window width to keep
    // "PA-ref(sc)" and "Suppression" legible, and column 1 is a third of it.
    for (auto* meter : { &inputMeter, &sidechainMeter, &outputMeter, &suppressionMeter }) {
        meter->setOrientation(LevelMeterComponent::Orientation::horizontal);
        addAndMakeVisible(meter);
    }

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
    // to read to be any use mid-show -- and a sentence-length tip becomes a
    // multi-line block sitting over the controls it is meant to explain.
    // (LessPALookAndFeel::getTooltipBounds now wraps rather than clips, so
    // length is a readability choice here rather than a correctness one.)
    helpButton.setTooltip("Full control reference");
    versionLabel.setTooltip("Less PA v" JucePlugin_VersionString);
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

    // Title off the top and footer off the bottom first: both span the full
    // width, so what is left is exactly the three-column area.
    bounds.title = content.removeFromTop(sc(titleStripHeight));
    content.removeFromTop(sc(titleGap));
    bounds.footer = content.removeFromBottom(sc(footerStripHeight));
    content.removeFromBottom(sc(footerGap));

    auto column1 = content.removeFromLeft(sc(columnWidth));
    content.removeFromLeft(sc(columnGap));
    auto column2 = content.removeFromLeft(sc(columnWidth));
    content.removeFromLeft(sc(columnGap));
    // Column 3 takes what is left rather than another sc(columnWidth): five
    // independently rounded slices can be a pixel or two short of the content
    // width, and that error would otherwise show up as a ragged right margin
    // against the corner wordmark, which is aligned to the content edge.
    auto column3 = content;

    bounds.column1Input = column1.removeFromTop(sc(inputSectionHeight));
    column1.removeFromTop(sc(column1BlockGap));
    bounds.column1Output = column1.removeFromTop(sc(outputBlockHeight));

    bounds.column2Adaptive = column2.removeFromTop(sc(adaptiveSectionHeight));
    column2.removeFromTop(sc(column2PanelGap));
    bounds.column2Residual = column2.removeFromTop(sc(residualSectionHeight));

    bounds.column3DoubleTalk = column3.removeFromTop(sc(doubleTalkSectionHeight));
    return bounds;
}

juce::Rectangle<int> PAEchoCancellerAudioProcessorEditor::computeLogoBounds() const
{
    const float scale = getUiScale();
    const auto sc = [scale](int designPx) { return juce::roundToInt(scale * static_cast<float>(designPx)); };
    const int width = sc(logoWidth);
    const int height = juce::roundToInt(static_cast<float>(width) / logoAspect);

    // Hard against the right of the content area, vertically centred in the
    // footer strip. Centred rather than bottom-pinned because the strip is now
    // a slider tall and holds three things -- the readout, the inline Dry/Wet
    // control and this mark all have to sit on one line.
    //
    // The delay readout and the Dry/Wet control are laid out around this
    // rectangle in resized(), and the static_asserts in PluginEditor.h prove
    // the three fit; the whole window scales by one factor, so proving it once
    // proves it across the resize range.
    auto content = getLocalBounds().reduced(sc(outerMargin));
    const auto footer = content.removeFromBottom(sc(footerStripHeight));
    return { footer.getRight() - width, footer.getCentreY() - height / 2, width, height };
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
    // sections.column1Output is deliberately absent: the output stage is left
    // on the bare background so that "these four boxes are the processing
    // chain" stays readable at a glance -- now left to right across the three
    // columns rather than top to bottom down one.
    g.setColour(LessPAColours::panel);
    for (const auto& panel : { sections.column1Input, sections.column2Adaptive,
                               sections.column2Residual, sections.column3DoubleTalk })
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
    //
    // Now that the meters are stacked rows rather than a side-by-side bank,
    // the first meter's *label* is what sets the bridge's left edge -- the
    // labels sit to the left of every bar, and leaving them outside the
    // lighter rectangle would split one instrument across two backgrounds.
    // The post-HPF toggle is the bank's last row and is unioned in for the
    // same reason: it selects what the bars above it are showing.
    const auto meterBars = inputMeterLabel.getBounds()
                               .getUnion(inputMeter.getBounds())
                               .getUnion(outputMeter.getBounds())
                               .getUnion(metersPostFilterToggle.getBounds());
    if (!meterBars.isEmpty())
        g.fillRoundedRectangle(meterBars.expanded(juce::roundToInt(8.0f * scale),
                                                  juce::roundToInt(6.0f * scale))
                                   .toFloat(),
                               6.0f * scale);

    // Left-aligned on the same x as every section header below it, and trimmed
    // clear of the version label and help button on the right.
    auto titleArea = sections.title.reduced(juce::roundToInt(scale * static_cast<float>(panelPaddingX)), 0)
                         .withTrimmedRight(juce::roundToInt(
                             scale * static_cast<float>(helpButtonSize + versionHelpGap + versionLabelWidth)));

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
    helpButton.setBounds(titleArea.removeFromRight(sc(helpButtonSize))
                             .withSizeKeepingCentre(sc(helpButtonSize), sc(helpButtonSize)));
    titleArea.removeFromRight(sc(versionHelpGap));
    versionLabel.setBounds(titleArea.removeFromRight(sc(versionLabelWidth))
                                .withSizeKeepingCentre(sc(versionLabelWidth), sc(helpButtonSize)));
    versionLabel.setFont(juce::FontOptions(scale * 10.0f));

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

    // The post-HPF toggle is the meter bank's last row, so its text is set to
    // the meter-label size rather than the height-proportional size every
    // other toggle gets -- it names part of the instrument, not a control.
    // juce::ToggleButton has no setFont(), so it is requested by property and
    // honoured in LessPALookAndFeel::drawToggleButton; the explicit repaint is
    // because a property change alone does not invalidate the component.
    metersPostFilterToggle.getProperties().set("fontHeight", scale * 12.0f);
    metersPostFilterToggle.repaint();

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
    // The leading gap is passed in rather than fixed: columns 2 and 3 use a
    // deliberately larger one (see the constants in PluginEditor.h).
    // The area a panel's rows are laid out in. The top inset is always
    // panelPaddingY, whatever column the panel is in -- that is what puts the
    // three top-row section headers on one line. Only the bottom inset varies,
    // which is how columns 2 and 3 get their extra breathing room.
    //
    // Deliberately not Rectangle::reduced(x, y): that insets top and bottom by
    // the same amount, so giving columns 2 and 3 more vertical padding also
    // pushed their headers down relative to column 1's. That bug shipped to
    // the user's host and was visible immediately as three misaligned headers.
    const auto panelBody = [&sc](juce::Rectangle<int> panel, int bottomPaddingDesignPx) {
        return panel.reduced(sc(panelPaddingX), 0)
            .withTrimmedTop(sc(panelPaddingY))
            .withTrimmedBottom(sc(bottomPaddingDesignPx));
    };

    struct LabelledSlider { juce::Label* label; juce::Slider* slider; };
    const auto addSliderRows = [](SectionGrid& grid, int gapDesignPx,
                                  std::initializer_list<LabelledSlider> rows) {
        for (auto& labelled : rows) {
            grid.gap(gapDesignPx); // separates this pair from whatever precedes it
            grid.row(controlLabelRow, *labelled.label);
            grid.row(sliderRow, *labelled.slider); // no gap: the label belongs to this slider
        }
    };

    // COLUMN 1 -- INPUT CONDITIONING. No separate gap after the header here:
    // the first pair's own leading column1ControlGap already provides it.
    // (Every row list below is proved against its section-height constant by
    // the static_asserts in PluginEditor.h.)
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, inputSectionLabel);
        addSliderRows(grid, column1ControlGap, { { &hpfLabel, &hpfSlider },
                                                 { &referenceGainLabel, &referenceGainSlider } });
        grid.performLayout(panelBody(sections.column1Input, panelPaddingY));
    }

    // COLUMN 1 -- METER BANK, ungrouped. No panel and no header: a heading
    // over a bank of meters is noise to the operator this is built for.
    //
    // Two grid columns: a fixed-width one for the meter names and a stretchy
    // one for their bars. The "Meters post HPF" toggle is the bank's last row
    // and simply spans both, so it lines up with the meter names rather than
    // floating below the instrument it belongs to. The four meters are
    // horizontal rows here rather than the old side-by-side vertical bank --
    // see LevelMeterComponent::Orientation.
    //
    // The leading and trailing gaps are the room paint() needs to draw the
    // meter bridge around the bank without overhanging the column.
    {
        SectionGrid grid(2, scale, meterLabelGap, meterLabelWidth);
        grid.gap(meterBridgeClearance);
        grid.meterRow(meterRowHeight, inputMeterLabel, inputMeter, meterBarHeight);
        grid.gap(meterRowGap);
        grid.meterRow(meterRowHeight, sidechainMeterLabel, sidechainMeter, meterBarHeight);
        grid.gap(meterRowGap);
        grid.meterRow(meterRowHeight, suppressionMeterLabel, suppressionMeter, meterBarHeight);
        grid.gap(meterRowGap);
        grid.meterRow(meterRowHeight, outputMeterLabel, outputMeter, meterBarHeight);
        grid.gap(meterToggleGap);
        grid.row(toggleRow, metersPostFilterToggle);
        grid.gap(meterBridgeClearance);
        grid.performLayout(sections.column1Output.reduced(sc(panelPaddingX), 0));
    }

    // COLUMN 2 -- ADAPTIVE FILTER.
    //
    // One control on purpose: Tail Length *is* the linear filter length. It is
    // the only thing in the plugin that touches the adaptive filter, and the
    // only one that interrupts audio when changed, so folding it into a
    // neighbouring section would misrepresent two distinct DSP stages.
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, adaptiveSectionLabel);
        grid.gap(column2HeaderGap);
        grid.row(controlLabelRow, tailLengthLabel);
        grid.row(comboRow, tailLengthCombo);
        grid.performLayout(panelBody(sections.column2Adaptive, widePanelPaddingBottom));
    }

    // COLUMN 2 -- RESIDUAL SUPPRESSION.
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, residualSectionLabel);
        grid.gap(column2HeaderGap);
        grid.row(controlLabelRow, suppressionStrengthLabel);
        grid.row(comboRow, suppressionStrengthCombo);
        grid.gap(column2ControlGap);
        grid.row(toggleRow, limitHfGainToggle);
        grid.performLayout(panelBody(sections.column2Residual, widePanelPaddingBottom));
    }

    // COLUMN 3 -- DOUBLE-TALK PROTECTION, still stacked top to bottom.
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, doubleTalkSectionLabel);
        grid.gap(column3HeaderGap);
        grid.row(controlLabelRow, nearendDetectorLabel);
        grid.row(comboRow, nearendDetectorCombo);
        addSliderRows(grid, column3ControlGap,
                      { { &nearendSensitivityLabel, &nearendSensitivitySlider },
                        { &protectionHoldTimeLabel, &protectionHoldTimeSlider },
                        { &transitionSmoothingLabel, &transitionSmoothingSlider } });
        grid.performLayout(panelBody(sections.column3DoubleTalk, widePanelPaddingBottom));
    }

    // FOOTER -- full width under all three columns: delay readout, Dry/Wet
    // Mix, wordmark, left to right on one line.
    //
    // Dry/Wet is centred on the window and the readout is then fitted into
    // what is left to its *left*, rather than the two being placed
    // independently -- that is what makes a collision impossible rather than
    // merely unlikely, and it is the same trick the readout already used
    // against the wordmark. The static_asserts in PluginEditor.h prove the
    // remaining space clears both neighbours' worst cases.
    {
        const auto dryWetArea = sections.footer.withSizeKeepingCentre(sc(dryWetFooterWidth),
                                                                     sections.footer.getHeight());

        // Label beside the slider rather than above it: the footer is one
        // slider tall, so the label-above-slider pairing the panels use does
        // not fit here.
        SectionGrid grid(2, scale, dryWetLabelGap, dryWetLabelWidth);
        grid.row(sliderRow, { &dryWetLabel, &dryWetSlider });
        grid.performLayout(dryWetArea);

        // Indented by panelPaddingX so it lines up with the section header
        // text above it rather than with the panel edge.
        delayReadoutLabel.setBounds(sections.footer
                                        .withTrimmedLeft(sc(panelPaddingX))
                                        .withRight(dryWetArea.getX() - sc(readoutLogoGap)));
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
    // alphabetical glossary that would send you hunting. The dialog reads the
    // stages top to bottom while the panel now reads them left to right across
    // its three columns; the *order* is the thing that has to match, and does.
    static const juce::String helpText =
        "LESS PA cancels PA speaker leakage out of an audience microphone, "
        "using the PA feed itself (not a room estimate of it) as a reference "
        "-- wire the PA signal into the Reference sidechain input. It works "
        "by applying acoustic echo cancellation (AEC) -- the same technique "
        "phones and conferencing systems use to remove speaker bleed.\n"
        "\n"
        "The panel follows the signal chain, left to right: what reaches the "
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
        "costs a little more CPU per instance. Tail Length is the only "
        "control that briefly interrupts audio when changed. It rebuilds "
        "the adaptive filter.\n"
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
        "DRY/WET MIX\n"
        "Blends the cancelled (wet) output with the original, unprocessed "
        "(dry) input. 100% is fully cancelled; lower it to let more of the "
        "original signal (including any residual PA leakage) back in to "
        "taste.\n"
        "\n"
        "\n"
        "Less PA is free to use, provided as-is with no warranty of any "
        "kind. Use it at your own risk.";

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
