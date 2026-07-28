#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

PAEchoCancellerAudioProcessorEditor::PAEchoCancellerAudioProcessorEditor(PAEchoCancellerAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(320, 838); // grown 70px for the Near-end Detector row -- see resized()

    helpButton.onClick = [this] { showHelpDialog(); };
    addAndMakeVisible(helpButton);

    tailLengthLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(tailLengthLabel);

    tailLengthCombo.addItem("50ms", 1);
    tailLengthCombo.addItem("200ms", 2);
    tailLengthCombo.addItem("400ms", 3);
    tailLengthCombo.addItem("800ms", 4);
    tailLengthCombo.onChange = [this] { tailLengthComboChanged(); };
    addAndMakeVisible(tailLengthCombo);

    suppressionStrengthLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(suppressionStrengthLabel);

    suppressionStrengthCombo.addItem("Gentle", 1);
    suppressionStrengthCombo.addItem("Moderate", 2);
    suppressionStrengthCombo.addItem("Hard", 3);
    suppressionStrengthCombo.onChange = [this] { suppressionStrengthComboChanged(); };
    addAndMakeVisible(suppressionStrengthCombo);

    limitHfGainToggle.onClick = [this] { limitHfGainToggleChanged(); };
    addAndMakeVisible(limitHfGainToggle);

    nearendDetectorLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(nearendDetectorLabel);

    nearendDetectorCombo.addItem("Classic", 1);
    nearendDetectorCombo.addItem("Subband (2-4kHz)", 2);
    nearendDetectorCombo.onChange = [this] { nearendDetectorComboChanged(); };
    addAndMakeVisible(nearendDetectorCombo);

    nearendSensitivityLabel.setJustificationType(juce::Justification::centred);
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

    protectionHoldTimeLabel.setJustificationType(juce::Justification::centred);
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

    hpfLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(hpfLabel);

    hpfSlider.setRange(80.0, 300.0);
    hpfSlider.setSkewFactorFromMidPoint(150.0);
    hpfSlider.setTextValueSuffix(" Hz");
    hpfSlider.setNumDecimalPlacesToDisplay(0);
    hpfSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    hpfSlider.onValueChange = [this] { hpfSliderChanged(); };
    addAndMakeVisible(hpfSlider);

    referenceGainLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(referenceGainLabel);

    referenceGainSlider.setRange(-24.0, 24.0);
    referenceGainSlider.setTextValueSuffix(" dB");
    referenceGainSlider.setNumDecimalPlacesToDisplay(1);
    referenceGainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    referenceGainSlider.onValueChange = [this] { referenceGainSliderChanged(); };
    addAndMakeVisible(referenceGainSlider);

    dryWetLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(dryWetLabel);

    dryWetSlider.setRange(0.0, 100.0);
    dryWetSlider.setTextValueSuffix(" %");
    dryWetSlider.setNumDecimalPlacesToDisplay(0);
    dryWetSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    dryWetSlider.onValueChange = [this] { dryWetSliderChanged(); };
    addAndMakeVisible(dryWetSlider);

    for (auto* label : { &inputMeterLabel, &sidechainMeterLabel, &outputMeterLabel, &suppressionMeterLabel }) {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::FontOptions(12.0f));
        addAndMakeVisible(label);
    }
    addAndMakeVisible(inputMeter);
    addAndMakeVisible(sidechainMeter);
    addAndMakeVisible(outputMeter);
    addAndMakeVisible(suppressionMeter);

    metersPostFilterToggle.onClick = [this] { metersPostFilterToggleChanged(); };
    addAndMakeVisible(metersPostFilterToggle);

    delayReadoutLabel.setJustificationType(juce::Justification::centred);
    delayReadoutLabel.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(delayReadoutLabel);

    updateTailLengthCombo();
    updateSuppressionStrengthCombo();
    updateLimitHfGainToggle();
    updateNearendDetectorCombo();
    updateNearendSensitivitySlider();
    updateProtectionHoldTimeSlider();
    updateHpfSlider();
    updateReferenceGainSlider();
    updateMetersPostFilterToggle();
    updateDryWetSlider();
    startTimerHz(30); // meter ballistics + keeps controls in sync with host automation
}

PAEchoCancellerAudioProcessorEditor::~PAEchoCancellerAudioProcessorEditor()
{
    stopTimer();
}

void PAEchoCancellerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
    g.setFont(16.0f);
    g.drawFittedText("Less PA (AEC3)", getLocalBounds().removeFromTop(40).reduced(28, 0),
                      juce::Justification::centred, 2);
}

void PAEchoCancellerAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(10);
    auto titleArea = bounds.removeFromTop(40); // space for the title painted in paint()
    helpButton.setBounds(titleArea.removeFromRight(24).withSizeKeepingCentre(24, 24));

    tailLengthLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(6);

    auto comboArea = bounds.removeFromTop(28);
    tailLengthCombo.setBounds(comboArea.withSizeKeepingCentre(150, comboArea.getHeight()));

    bounds.removeFromTop(16);

    suppressionStrengthLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(6);

    auto suppressionComboArea = bounds.removeFromTop(28);
    suppressionStrengthCombo.setBounds(suppressionComboArea.withSizeKeepingCentre(150, suppressionComboArea.getHeight()));

    bounds.removeFromTop(10);
    auto hfToggleArea = bounds.removeFromTop(24);
    limitHfGainToggle.setBounds(hfToggleArea.withSizeKeepingCentre(160, hfToggleArea.getHeight()));

    bounds.removeFromTop(16);

    nearendDetectorLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(6);
    auto detectorComboArea = bounds.removeFromTop(28);
    nearendDetectorCombo.setBounds(detectorComboArea.withSizeKeepingCentre(170, detectorComboArea.getHeight()));

    bounds.removeFromTop(16);

    nearendSensitivityLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(4);
    nearendSensitivitySlider.setBounds(bounds.removeFromTop(28));

    bounds.removeFromTop(16);

    protectionHoldTimeLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(4);
    protectionHoldTimeSlider.setBounds(bounds.removeFromTop(28));

    bounds.removeFromTop(16);

    hpfLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(4);
    hpfSlider.setBounds(bounds.removeFromTop(28));

    bounds.removeFromTop(16);

    referenceGainLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(4);
    referenceGainSlider.setBounds(bounds.removeFromTop(28));

    bounds.removeFromTop(16);

    dryWetLabel.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(4);
    dryWetSlider.setBounds(bounds.removeFromTop(28));

    bounds.removeFromTop(16);

    auto meterLabelRow = bounds.removeFromTop(16);
    bounds.removeFromTop(4);
    auto meterRow = bounds.removeFromTop(120);

    // The meter bar itself is narrow (like a real channel-strip meter), but
    // its label needs more room than that to read ("PA-source" doesn't fit
    // in 24px) -- give each meter a wider column and centre the narrow bar
    // within it, rather than sizing the label to the meter's width.
    const int meterWidth = 22;
    const int columnWidth = 68; // fits "PA-source" at 12pt on one line
    const int columnGap = 6;
    const int numMeters = 4;
    const int groupWidth = numMeters * columnWidth + (numMeters - 1) * columnGap;

    meterLabelRow = meterLabelRow.withSizeKeepingCentre(groupWidth, meterLabelRow.getHeight());
    meterRow = meterRow.withSizeKeepingCentre(groupWidth, meterRow.getHeight());

    juce::Label* meterLabels[] = { &inputMeterLabel, &sidechainMeterLabel, &suppressionMeterLabel, &outputMeterLabel };
    LevelMeterComponent* meters[] = { &inputMeter, &sidechainMeter, &suppressionMeter, &outputMeter };

    for (int i = 0; i < numMeters; ++i) {
        meterLabels[i]->setBounds(meterLabelRow.removeFromLeft(columnWidth));

        auto meterColumn = meterRow.removeFromLeft(columnWidth);
        meters[i]->setBounds(meterColumn.withSizeKeepingCentre(meterWidth, meterColumn.getHeight()));

        if (i < numMeters - 1) {
            meterLabelRow.removeFromLeft(columnGap);
            meterRow.removeFromLeft(columnGap);
        }
    }

    bounds.removeFromTop(14);
    auto toggleArea = bounds.removeFromTop(24);
    metersPostFilterToggle.setBounds(toggleArea.withSizeKeepingCentre(160, toggleArea.getHeight()));

    bounds.removeFromTop(6);
    delayReadoutLabel.setBounds(bounds.removeFromTop(18));
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

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(content);
    options.dialogTitle = "Less PA -- Help";
    options.dialogBackgroundColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}
