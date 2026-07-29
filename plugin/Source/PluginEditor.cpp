#include "PluginProcessor.h"
#include "PluginEditor.h"

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

// Row heights, shared between the three sections so the section-height
// constants in PluginEditor.h can be checked against them by eye.
constexpr int sectionHeaderRow = 18;
constexpr int controlLabelRow = 16;
constexpr int comboRow = 28;
constexpr int sliderRow = 26;
constexpr int toggleRow = 24;
constexpr int meterBarRow = 108;
constexpr int readoutRow = 18;

} // namespace

PAEchoCancellerAudioProcessorEditor::PAEchoCancellerAudioProcessorEditor(PAEchoCancellerAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&lookAndFeel);

    setSize(designWidth, designHeight);

    // Aspect-locked rather than freely resizable: locking it means the whole
    // window scales by one factor (see getUiScale()), so there is no general
    // reflow problem to solve -- no row has to decide whether to wrap.
    setResizable(true, true);
    setResizeLimits(300, 720, 480, 1152);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(static_cast<double>(designWidth) / static_cast<double>(designHeight));

    helpButton.onClick = [this] { showHelpDialog(); };
    addAndMakeVisible(helpButton);

    for (auto* label : { &echoSectionLabel, &processingSectionLabel, &meteringSectionLabel }) {
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

    delayReadoutLabel.setJustificationType(juce::Justification::centred);
    delayReadoutLabel.setColour(juce::Label::textColourId, LessPAColours::secondaryText);
    addAndMakeVisible(delayReadoutLabel);

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
    bounds.echo = content.removeFromTop(sc(echoSectionHeight));
    content.removeFromTop(sc(sectionGap));
    bounds.processing = content.removeFromTop(sc(processingSectionHeight));
    content.removeFromTop(sc(sectionGap));
    bounds.metering = content.removeFromTop(sc(meteringSectionHeight));
    return bounds;
}

void PAEchoCancellerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    const auto sections = computeSectionBounds();

    // Panels, no border: an outline on top of a fill would read as busier
    // without adding any information the fill doesn't already carry, which
    // works against the point of the style. The fill alone is enough contrast
    // against #14161A to separate the three groups.
    g.setColour(LessPAColours::panel);
    for (const auto& panel : { sections.echo, sections.processing, sections.metering })
        g.fillRoundedRectangle(panel.toFloat(), 6.0f * getUiScale());

    g.setColour(LessPAColours::primaryText);
    g.setFont(juce::FontOptions(16.0f * getUiScale()));
    g.drawFittedText("Less PA (AEC3)", sections.title.reduced(28, 0), juce::Justification::centred, 2);
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
    for (auto* label : { &echoSectionLabel, &processingSectionLabel, &meteringSectionLabel })
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

    // ECHO CANCELLATION -- 18 + 6 + 16 + 28 + 12 + 24 = 104 rows,
    // + 2 * panelPaddingY = echoSectionHeight (124).
    //
    // The two combos share a row: both are short dropdowns in the same
    // section, and pairing them reclaims ~65px of the height the taller title
    // strip and the panel padding cost.
    {
        SectionGrid grid(2, scale, 12);
        grid.row(sectionHeaderRow, echoSectionLabel);
        grid.gap(6);
        grid.row(controlLabelRow, { &tailLengthLabel, &suppressionStrengthLabel });
        grid.row(comboRow, { &tailLengthCombo, &suppressionStrengthCombo });
        grid.gap(12);
        grid.row(toggleRow, limitHfGainToggle);
        grid.performLayout(sections.echo.reduced(sc(panelPaddingX), sc(panelPaddingY)));
    }

    // SIGNAL PROCESSING -- 18 + 6 + 16 + 28 + 10 + 6 * (16 + 26) + 5 * 10
    // = 380 rows, + 2 * panelPaddingY = processingSectionHeight (400).
    {
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, processingSectionLabel);
        grid.gap(6);
        grid.row(controlLabelRow, nearendDetectorLabel);
        grid.row(comboRow, nearendDetectorCombo);

        struct LabelledSlider { juce::Label* label; juce::Slider* slider; };
        const LabelledSlider sliderRows[] = {
            { &nearendSensitivityLabel, &nearendSensitivitySlider },
            { &protectionHoldTimeLabel, &protectionHoldTimeSlider },
            { &transitionSmoothingLabel, &transitionSmoothingSlider },
            { &hpfLabel, &hpfSlider },
            { &referenceGainLabel, &referenceGainSlider },
            { &dryWetLabel, &dryWetSlider },
        };

        for (auto& labelled : sliderRows) {
            grid.gap(10); // separates this pair from whatever precedes it
            grid.row(controlLabelRow, *labelled.label);
            grid.row(sliderRow, *labelled.slider); // no gap: the label belongs to this slider
        }

        grid.performLayout(sections.processing.reduced(sc(panelPaddingX), sc(panelPaddingY)));
    }

    // METERING -- 18 + 6 + 16 + 108 + 10 + 24 + 6 + 18 = 206 rows,
    // + 2 * panelPaddingY = meteringSectionHeight (226).
    //
    // Four columns rather than the old hand-computed column arithmetic: the
    // meter bar stays narrow (22px, like a real channel-strip meter) while
    // its label gets the whole column, which is what "PA-ref(sc)" and
    // "Suppression" need to read at 12pt.
    {
        SectionGrid grid(4, scale, 8);
        grid.row(sectionHeaderRow, meteringSectionLabel);
        grid.gap(6);
        grid.row(controlLabelRow, { &inputMeterLabel, &sidechainMeterLabel,
                                    &suppressionMeterLabel, &outputMeterLabel });
        grid.row(meterBarRow, { &inputMeter, &sidechainMeter, &suppressionMeter, &outputMeter }, 22);
        grid.gap(10);
        grid.row(toggleRow, metersPostFilterToggle);
        grid.gap(6);
        grid.row(readoutRow, delayReadoutLabel);
        grid.performLayout(sections.metering.reduced(sc(panelPaddingX), sc(panelPaddingY)));
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
    static const juce::String helpText =
        "LESS PA cancels PA speaker leakage out of an audience microphone, "
        "using the PA feed itself (not a room estimate of it) as a reference "
        "-- wire the PA signal into the Reference sidechain input.\n"
        "\n"
        "TAIL LENGTH\n"
        "How long a reverb tail the canceller can model, matched to your "
        "venue: 50ms (dry room) up to 800ms (very large hall). Longer "
        "costs a little more CPU per instance.\n"
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
