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

    for (auto* label : { &inputSectionLabel, &cancellationSectionLabel, &outputSectionLabel }) {
        label->setJustificationType(juce::Justification::centredLeft);
        // Secondary text: a group header names the region, it isn't a control,
        // so it should sit behind the labels it groups in the reading order.
        label->setColour(juce::Label::textColourId, LessPAColours::secondaryText);
    }
    addAndMakeVisible(inputSectionLabel);
    addAndMakeVisible(cancellationSectionLabel);
    addAndMakeVisible(outputSectionLabel);

    // Left-aligned, not centred: every control below is either full-width or
    // column-width and starts at its panel's left edge, so a centred label
    // would sit visibly off from the thing it names -- and the sliders'
    // right-hand value boxes push their visual centre left of the row's.
    for (auto* label : { &tailLengthLabel, &amountLabel, &maxReductionLabel, &responseLabel,
                         &hpfLabel, &referenceGainLabel, &dryWetLabel })
        label->setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(tailLengthLabel);

    // The room a length suits is part of the item text: choosing by venue is
    // the decision someone actually makes at a show.
    tailLengthCombo.addItem("50 ms - small room", 1);
    tailLengthCombo.addItem("200 ms - club / theatre", 2);
    tailLengthCombo.addItem("400 ms - hall", 3);
    tailLengthCombo.addItem("800 ms - arena / outdoor", 4);
    tailLengthCombo.onChange = [this] { tailLengthComboChanged(); };
    addAndMakeVisible(tailLengthCombo);

    addAndMakeVisible(amountLabel);
    amountSlider.setRange(0.0, 100.0);
    amountSlider.setTextValueSuffix(" %");
    amountSlider.setNumDecimalPlacesToDisplay(0);
    amountSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    amountSlider.onValueChange = [this] { amountSliderChanged(); };
    addAndMakeVisible(amountSlider);

    addAndMakeVisible(maxReductionLabel);
    maxReductionSlider.setRange(0.0, 24.0);
    maxReductionSlider.textFromValueFunction = [](double depth) {
        return depth < 0.05 ? juce::String("0 dB") : "-" + juce::String(depth, 1) + " dB";
    };
    maxReductionSlider.valueFromTextFunction = [](const juce::String& text) {
        return std::abs(text.retainCharacters("0123456789.-").getDoubleValue()); // "-12", "12 dB" both mean 12 dB
    };
    maxReductionSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    maxReductionSlider.onValueChange = [this] { maxReductionSliderChanged(); };
    addAndMakeVisible(maxReductionSlider);

    responseSlider.setRange(3.0, 50.0);
    responseSlider.setTextValueSuffix(" ms");
    responseSlider.setNumDecimalPlacesToDisplay(0);
    responseSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    responseSlider.onValueChange = [this] { responseSliderChanged(); };
    addAndMakeVisible(responseLabel);
    addAndMakeVisible(responseSlider);

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

    // Peaks of about -36..-3 dBFS. The Kalman filter scales itself to the
    // reference level, so the zone is about the edges: well below it the PA
    // can drop under the canceller's "PA present" threshold (about -50 dBFS
    // RMS, see KalmanEchoCanceller::startRefPower), and above it a feed risks
    // clipping upstream. Deliberately wide because a PA feed is very dynamic
    // and cannot be held to a narrow band.
    sidechainMeter.setTargetZone(-36.0f, -3.0f);

    // The canceller's state, in words, under the output meters.
    delayReadoutLabel.setJustificationType(juce::Justification::centredLeft);
    delayReadoutLabel.setColour(juce::Label::textColourId, LessPAColours::secondaryText);
    addAndMakeVisible(delayReadoutLabel);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setColour(juce::Label::textColourId, LessPAColours::primaryText);
    addAndMakeVisible(statusLabel);

    resetButton.onClick = [this] { resetCleanupToDefaults(); };
    addAndMakeVisible(resetButton);

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
    tailLengthCombo.setTooltip("Match to the venue's reverb");
    amountSlider.setTooltip("Extra removal of the PA bleed that's left; 0% = none");
    maxReductionSlider.setTooltip("How far the extra removal may turn down any frequency");
    responseSlider.setTooltip("How fast the extra removal reacts; longer = smoother");
    hpfSlider.setTooltip("Cutoff on mic + reference");
    referenceGainSlider.setTooltip("Reference gain only");
    dryWetSlider.setTooltip("Blend cancelled vs original");
    resetButton.setTooltip("Amount, Max Reduction and Response back to defaults");
    inputMeter.setTooltip("Mic, after the HPF");
    sidechainMeter.setTooltip("After HPF and trim; aim for the zone");
    suppressionMeter.setTooltip("Reduction, in vs out");
    outputMeter.setTooltip("After cancellation");
    delayReadoutLabel.setTooltip("Echo path delay estimate");
    statusLabel.setTooltip("What the canceller is doing");

    updateTailLengthCombo();
    updateSliders();
    updateResetButton();
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

    bounds.column1 = content.removeFromLeft(sc(columnWidth));
    content.removeFromLeft(sc(columnGap));
    bounds.column2 = content.removeFromLeft(sc(columnWidth));
    content.removeFromLeft(sc(columnGap));
    // Column 3 takes what is left rather than another sc(columnWidth): five
    // independently rounded slices can be a pixel or two short of the content
    // width, which would otherwise show as a ragged right margin against the
    // corner wordmark, aligned to the content edge.
    bounds.column3 = content;

    return bounds;
}

juce::Rectangle<int> PAEchoCancellerAudioProcessorEditor::computeLogoBounds() const
{
    const float scale = getUiScale();
    const auto sc = [scale](int designPx) { return juce::roundToInt(scale * static_cast<float>(designPx)); };
    const int width = sc(logoWidth);
    const int height = juce::roundToInt(static_cast<float>(width) / logoAspect);

    // Hard against the right of the content area, vertically centred in the
    // footer strip.
    auto content = getLocalBounds().reduced(sc(outerMargin));
    const auto footer = content.removeFromBottom(sc(footerStripHeight));
    return { footer.getRight() - width, footer.getCentreY() - height / 2, width, height };
}

void PAEchoCancellerAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float scale = getUiScale();
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    const auto sections = computeSectionBounds();

    // Panels, no border: the fill alone separates the three stages from the
    // background. The meters sit inside their panels, where the lighter fill
    // is what makes their darker idle trough read as a recessed slot.
    g.setColour(LessPAColours::panel);
    for (const auto& panel : { sections.column1, sections.column2, sections.column3 })
        g.fillRoundedRectangle(panel.toFloat(), 6.0f * scale);

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
    for (auto* label : { &inputSectionLabel, &cancellationSectionLabel, &outputSectionLabel })
        label->setFont(juce::FontOptions(scale * 11.0f).withStyle("Bold"));

    for (auto* label : { &tailLengthLabel, &amountLabel, &maxReductionLabel, &responseLabel,
                         &hpfLabel, &referenceGainLabel, &dryWetLabel })
        label->setFont(juce::FontOptions(scale * 12.5f));

    for (auto* label : { &inputMeterLabel, &sidechainMeterLabel, &outputMeterLabel,
                         &suppressionMeterLabel, &delayReadoutLabel })
        label->setFont(juce::FontOptions(scale * 12.0f));
    statusLabel.setFont(juce::FontOptions(scale * 12.5f).withStyle("Bold"));
    resetButton.getProperties().set("fontHeight", scale * 12.0f);
    resetButton.repaint(); // a property change alone doesn't invalidate it

    juce::Slider* const sliders[] = { &amountSlider, &maxReductionSlider, &responseSlider,
                                      &hpfSlider, &referenceGainSlider, &dryWetSlider };
    const int textBoxWidth = sc(60);
    const int textBoxHeight = sc(24);
    for (auto* slider : sliders)
        if (slider->getTextBoxWidth() != textBoxWidth || slider->getTextBoxHeight() != textBoxHeight)
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, textBoxWidth, textBoxHeight);

    // The area a panel's rows are laid out in. Every panel uses the same
    // insets, which is what puts the three section headers on one line.
    const auto panelBody = [&sc](juce::Rectangle<int> panel) {
        return panel.reduced(sc(panelPaddingX), 0)
            .withTrimmedTop(sc(panelPaddingTop))
            .withTrimmedBottom(sc(panelPaddingBottom));
    };

    // A labelled control is always a 16px label sitting directly on top of
    // its own control, with the separating air above the pair rather than
    // between them -- that pairing is what makes a column readable.
    const auto addLabelled = [](SectionGrid& grid, int gapDesignPx, juce::Label& label,
                                juce::Component& control, int controlRowDesignPx) {
        grid.gap(gapDesignPx);
        grid.row(controlLabelRow, label);
        grid.row(controlRowDesignPx, control);
    };

    // Meters are laid out in their own two-column grid -- a fixed-width
    // column for the names so they line up as a block, and the bars filling
    // the rest -- placed in whatever area the panel's own grid left for them.
    const auto layoutMeters = [&](juce::Rectangle<int> area, juce::Label& label1, LevelMeterComponent& meter1,
                                  juce::Label& label2, LevelMeterComponent& meter2) {
        SectionGrid grid(2, scale, meterLabelGap, meterLabelWidth);
        grid.meterRow(meterRowHeight, label1, meter1, meterBarHeight);
        grid.gap(meterRowGap);
        grid.meterRow(meterRowHeight, label2, meter2, meterBarHeight);
        grid.performLayout(area);
    };

    // COLUMN 1 -- INPUT: the two things that shape what the canceller hears,
    // with the two meters they move directly underneath. Gain-staging the PA
    // reference is a look-at-the-meter-while-turning-the-knob job.
    {
        auto body = panelBody(sections.column1);
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, inputSectionLabel);
        addLabelled(grid, controlGap, referenceGainLabel, referenceGainSlider, sliderRow);
        addLabelled(grid, controlGap, hpfLabel, hpfSlider, sliderRow);
        grid.performLayout(body);

        const int used = sc(sectionHeaderRow + 2 * (controlGap + labelledSlider) + meterBlockGap);
        layoutMeters(body.withTrimmedTop(used).withHeight(sc(meterPair)),
                     inputMeterLabel, inputMeter, sidechainMeterLabel, sidechainMeter);
    }

    // COLUMN 2 -- CANCELLATION: the venue's Tail Length, then the three
    // controls for how much is removed, with their Reset in the header row.
    {
        auto body = panelBody(sections.column2);
        auto header = body.withHeight(sc(sectionHeaderRow));
        resetButton.setBounds(header.removeFromRight(sc(resetButtonWidth))
                                  .withSizeKeepingCentre(sc(resetButtonWidth), sc(buttonRow - 4)));

        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, cancellationSectionLabel);
        addLabelled(grid, headerGap, tailLengthLabel, tailLengthCombo, comboRow);
        addLabelled(grid, controlGap, amountLabel, amountSlider, sliderRow);
        addLabelled(grid, controlGap, maxReductionLabel, maxReductionSlider, sliderRow);
        addLabelled(grid, controlGap, responseLabel, responseSlider, sliderRow);
        grid.performLayout(body);
        cancellationSectionLabel.setBounds(cancellationSectionLabel.getBounds().withRight(resetButton.getX()));
    }

    // COLUMN 3 -- OUTPUT: what the canceller is doing, then the mix.
    {
        auto body = panelBody(sections.column3);
        SectionGrid grid(1, scale, 0);
        grid.row(sectionHeaderRow, outputSectionLabel);
        grid.gap(headerGap + meterPair + meterBlockGap); // the meter block goes here
        grid.row(controlLabelRow, dryWetLabel);
        grid.row(sliderRow, dryWetSlider);
        grid.gap(controlGap);
        grid.row(statusRow, statusLabel);
        grid.row(statusRow, delayReadoutLabel);
        grid.performLayout(body);

        layoutMeters(body.withTrimmedTop(sc(sectionHeaderRow + headerGap)).withHeight(sc(meterPair)),
                     suppressionMeterLabel, suppressionMeter, outputMeterLabel, outputMeter);
    }

}

void PAEchoCancellerAudioProcessorEditor::updateResetButton()
{
    // Greyed out when there is nothing to reset, so the button also answers
    // "have I changed how much is removed?" at a glance.
    bool modified = false;
    for (auto* param : getCleanupParameters())
        modified = modified || std::abs(param->getValue() - param->getDefaultValue()) >= 1.0e-4f;
    if (resetButton.isEnabled() != modified)
        resetButton.setEnabled(modified);
}

std::array<juce::RangedAudioParameter*, 3> PAEchoCancellerAudioProcessorEditor::getCleanupParameters() const
{
    return { processor.getAmountParameter(), processor.getMaxReductionParameter(), processor.getResponseParameter() };
}

void PAEchoCancellerAudioProcessorEditor::resetCleanupToDefaults()
{
    // One gesture per parameter, like any other control edit, so hosts record
    // the reset as ordinary automation/undo steps. The controls themselves
    // pick the new values up from the 30Hz timer sync, and the processor
    // applies them live like any other change.
    for (auto* param : getCleanupParameters()) {
        param->beginChangeGesture();
        param->setValueNotifyingHost(param->getDefaultValue());
        param->endChangeGesture();
    }
    updateResetButton();
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

namespace {

// One host gesture per committed slider value, so hosts record an edit as an
// ordinary automation/undo step.
void setParameterFromUi(juce::RangedAudioParameter& param, float value)
{
    param.beginChangeGesture();
    param.setValueNotifyingHost(param.convertTo0to1(value));
    param.endChangeGesture();
}

void syncSlider(juce::Slider& slider, double value, double tolerance)
{
    if (std::abs(slider.getValue() - value) > tolerance)
        slider.setValue(value, juce::dontSendNotification);
}

} // namespace

void PAEchoCancellerAudioProcessorEditor::amountSliderChanged()
{
    setParameterFromUi(*processor.getAmountParameter(), static_cast<float>(amountSlider.getValue()));
}

void PAEchoCancellerAudioProcessorEditor::maxReductionSliderChanged()
{
    setParameterFromUi(*processor.getMaxReductionParameter(), -static_cast<float>(maxReductionSlider.getValue()));
}

void PAEchoCancellerAudioProcessorEditor::responseSliderChanged()
{
    setParameterFromUi(*processor.getResponseParameter(), static_cast<float>(responseSlider.getValue()));
}

void PAEchoCancellerAudioProcessorEditor::hpfSliderChanged()
{
    setParameterFromUi(*processor.getHpfFrequencyParameter(), static_cast<float>(hpfSlider.getValue()));
}

void PAEchoCancellerAudioProcessorEditor::referenceGainSliderChanged()
{
    setParameterFromUi(*processor.getReferenceGainParameter(), static_cast<float>(referenceGainSlider.getValue()));
}

void PAEchoCancellerAudioProcessorEditor::dryWetSliderChanged()
{
    setParameterFromUi(*processor.getDryWetMixParameter(), static_cast<float>(dryWetSlider.getValue()));
}

void PAEchoCancellerAudioProcessorEditor::updateSliders()
{
    // Keeps every slider in step with host automation and session loads.
    syncSlider(amountSlider, processor.getAmountParameter()->get(), 0.05);
    syncSlider(maxReductionSlider, -processor.getMaxReductionParameter()->get(), 0.05);
    syncSlider(responseSlider, processor.getResponseParameter()->get(), 0.05);
    syncSlider(hpfSlider, processor.getHpfFrequencyParameter()->get(), 0.05);
    syncSlider(referenceGainSlider, processor.getReferenceGainParameter()->get(), 0.05);
    syncSlider(dryWetSlider, processor.getDryWetMixParameter()->get(), 0.05);
}

void PAEchoCancellerAudioProcessorEditor::timerCallback()
{
    updateTailLengthCombo();
    updateSliders();
    updateResetButton();

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
        statusLabel.setText("Waiting for audio", juce::dontSendNotification);
        return;
    }

    // The filter's echo-path delay readout (already held steady over ~2 s
    // in the canceller). "--" with signal present = the reference likely
    // isn't reaching the plugin at all (e.g. sidechain pins not routed).
    {
        const int delayMs = processor.getEstimatedEchoPathDelayMs();
        juce::String text("PA delay: ");
        if (delayMs < 0)
            text << "--";
        else
            text << delayMs << " ms";
        delayReadoutLabel.setText(text, juce::dontSendNotification);

        // The same facts, as the sentence an operator needs: is there a PA
        // feed at all, and has the canceller found it in the mic yet?
        juce::String status;
        if (sidechainLevelPre <= silenceThreshold)
            status = "No PA signal on Reference input";
        else if (delayMs < 0)
            status = "Locking on to the PA...";
        else
            status = "Cancelling PA bleed";
        statusLabel.setText(status, juce::dontSendNotification);
    }

    // Always post-HPF (and, for the PA ref, post-trim): the meters sit next
    // to those two controls so their effect is what the meters should show.
    const float displayedInputLevel = processor.getInputPeakLevelPost();
    const float displayedOutputLevel = processor.getOutputPeakLevel();
    const float displayedSidechainLevel = processor.getSidechainPeakLevelPost();
    inputMeter.setLevel(displayedInputLevel);
    sidechainMeter.setLevel(displayedSidechainLevel);
    outputMeter.setLevel(displayedOutputLevel);

    // Suppression is measured directly from peak levels (dB reduction from
    // input to output), so it matches what Input/Output show by
    // construction, at the cost of also picking up any level change from
    // Mix or plain near-end dynamics, not only echo removal.
    //
    // The Input side uses the post-HPF, delay-compensated reading -- Output
    // always reflects Input from getLatencySamples() earlier, so comparing
    // it against the *live* Input peak makes fast transients (kick/snare)
    // look heavily suppressed when they simply haven't reached the output
    // yet. See getInputPeakLevelPostDelayed()'s comment.
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
    // Mirrors the panel exactly: the three columns left to right, then how
    // to use it live and in post. The order is the thing that has to match
    // the panel, and does.
    static const juce::String helpText =
        "LESS PA removes PA speaker bleed from an audience microphone, using "
        "the PA feed itself as a reference -- wire the PA signal into the "
        "Reference sidechain input. It works by acoustic echo cancellation "
        "(AEC), the same technique phones and conferencing systems use to "
        "remove speaker bleed.\n"
        "\n"
        "It works in two steps. First it learns how the PA sounds by the "
        "time it reaches the mic, makes a copy of that from the PA feed, "
        "and subtracts it. That copy is never perfect, so some bleed is "
        "left. Second, an extra removal step turns down the frequencies "
        "where that leftover bleed is still audible. Amount, Max Reduction "
        "and Response set the second step.\n"
        "\n"
        "The panel follows the signal, left to right: what reaches the "
        "canceller, how it cancels, and what comes out.\n"
        "\n"
        "\n"
        "=== INPUT ===\n"
        "\n"
        "PA REFERENCE TRIM\n"
        "A plain gain trim on the reference signal. The PA ref meter has a "
        "wide marked zone (peaks of about -36 to -3dBFS) because a PA feed "
        "is very dynamic: keep the loud parts inside it. Well below the "
        "zone the canceller may not register the feed as PA, so raise the "
        "trim. Above the zone nothing breaks, but "
        "a feed that hot risks clipping, so there is no benefit in going "
        "further. Not a substitute for Amount.\n"
        "\n"
        "INPUT HPF\n"
        "A 24dB/octave high-pass filter (80-300Hz), applied identically to "
        "the mic and the PA reference before cancellation, to keep rumble "
        "and handling noise from confusing the canceller.\n"
        "\n"
        "MIC / PA REF METERS\n"
        "Levels after the HPF (and, for the PA ref, after the trim) -- i.e. "
        "exactly what the canceller is being fed. The PA ref meter has a "
        "marked target zone: amber means too quiet, green means in the "
        "zone, red means above it.\n"
        "\n"
        "\n"
        "=== CANCELLATION ===\n"
        "\n"
        "TAIL LENGTH\n"
        "How long a reverb tail the canceller models. Pick by venue: 50 ms "
        "for a small room, up to 800 ms for an arena or outdoor rig. Longer "
        "costs a little more CPU. Applies live.\n"
        "\n"
        "AMOUNT\n"
        "How much of the bleed that is left after the subtraction gets "
        "removed on top. 0% turns the extra removal off: only the "
        "subtraction, the most natural sound. Higher removes more PA, and "
        "more of the crowd with it. 25% is a good start; raise it if PA "
        "still comes through, lower it if the crowd sounds thin or watery. "
        "Applies live.\n"
        "\n"
        "MAX REDUCTION\n"
        "How far the extra removal may turn down any frequency. Closer to "
        "0 dB keeps more of the room under the PA; -24 dB lets it go "
        "furthest. Has no effect at 0% Amount. Applies live.\n"
        "\n"
        "RESPONSE\n"
        "How quickly the extra removal follows the sound (3-50 ms). "
        "Shorter tracks the PA more tightly but can make crowd noise "
        "flutter; longer is smoother but lets a little more PA through on "
        "fast changes. Applies live.\n"
        "\n"
        "RESET\n"
        "Puts Amount, Max Reduction and Response back to their defaults. "
        "Greyed out when they already are.\n"
        "\n"
        "\n"
        "=== OUTPUT ===\n"
        "\n"
        "SUPPRESSION / OUTPUT METERS\n"
        "Suppression is the dB difference between mic in and output, so it "
        "always matches what you hear: near 0dB with no PA to remove, higher "
        "when there is.\n"
        "\n"
        "MIX\n"
        "Blends the cancelled output with the original mic. 100% is fully "
        "cancelled; lower it to bring some of the original back.\n"
        "\n"
        "STATUS AND PA DELAY\n"
        "\"No PA signal\" means nothing is arriving on the Reference input -- "
        "check the sidechain routing in your host. \"Locking on\" means the "
        "PA is there but the canceller hasn't found it in the mic yet. PA "
        "delay is how far the PA reference leads the bleed in the mic; a "
        "steady number means a solid lock, a jumping one means the reference "
        "and mic timing is unstable.\n"
        "\n"
        "\n"
        "=== LIVE AND IN POST ===\n"
        "\n"
        "Live (e.g. LiveProfessor, MainStage): the plugin adds about 4 ms "
        "of latency (192 samples at 44.1/48kHz, 320 at 96kHz) and reports "
        "it to the host. Give it a few seconds of PA signal to lock on "
        "before relying on it. Every control applies without interruption.\n"
        "\n"
        "In post: insert it on the mic track with the PA feed on the "
        "sidechain. An offline bounce sounds the same as playing back from "
        "the same start point, including when the session's Tail Length "
        "differs from the default. The canceller learns the room from where "
        "playback or the bounce starts, so start about 10 seconds before the "
        "part you need.\n"
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
