#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

PAEchoCancellerAudioProcessor::PAEchoCancellerAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withInput("Reference", juce::AudioChannelSet::mono(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Every parameter ID below is stable across releases: hosts address
    // automation by it, and get/setStateInformation key saved sessions by
    // it. Tail Length, HPF Frequency, PA Reference Trim, Mix and Bypass
    // keep the IDs they had in 1.0.x, so old sessions and automation carry
    // over. The bleed suppressor's Strength, Range and Time (IDs amount,
    // maxReduction and response) came in 1.1.0: a 1.0.x session starts them
    // at their defaults, and saved values of controls that no longer exist
    // are ignored (see setStateInformation).
    addParameter(tailLengthParam = new juce::AudioParameterChoice(
        "tailLength", "Tail Length",
        juce::StringArray{ "50 ms - small room", "200 ms - club / theatre", "400 ms - hall", "800 ms - arena / outdoor" }, 3));
    // Defaults 80% / -12 dB / 30 ms, chosen by ear on the LS26 and Oslo
    // recordings in Logic (the -12 dB Range caps how far it goes).
    addParameter(amountParam = new juce::AudioParameterFloat(
        "amount", "Suppressor Strength",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 80.0f));
    addParameter(maxReductionParam = new juce::AudioParameterFloat(
        "maxReduction", "Suppressor Range",
        juce::NormalisableRange<float>(-24.0f, 0.0f, 0.1f), -12.0f));
    addParameter(responseParam = new juce::AudioParameterFloat(
        "response", "Suppressor Time",
        juce::NormalisableRange<float>(3.0f, 50.0f, 0.1f), 30.0f));
    auto hpfRange = juce::NormalisableRange<float>(80.0f, 300.0f, 0.1f);
    hpfRange.setSkewForCentre(150.0f);
    addParameter(hpfFrequencyParam = new juce::AudioParameterFloat(
        "hpfFrequency", "HPF Frequency", hpfRange, 150.0f));
    addParameter(referenceGainParam = new juce::AudioParameterFloat(
        "referenceGain", "PA Reference Trim",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    addParameter(dryWetMixParam = new juce::AudioParameterFloat(
        "dryWetMix", "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    addParameter(bypassParam = new juce::AudioParameterBool(
        "bypass", "Bypass", false));
}

PAEchoCancellerAudioProcessor::~PAEchoCancellerAudioProcessor() = default;

void PAEchoCancellerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    preparedBlockSize = juce::jmax(1, samplesPerBlock);

    const int numMicChannels = juce::jmax(1, getChannelCountOfBus(true, 0));

    // Allocated for the longest Tail Length (800 ms), so shorter/longer
    // tails later are a live change without allocation.
    kalman.prepare(sampleRate, numMicChannels, 0.8);
    updateKalmanSettings();
    frameSize = kalman.getBlockSize();

    currentSamplePosition.store(0, std::memory_order_relaxed);
    inputPeakHistoryWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& entry : inputPeakHistory) {
        entry.samplePosition.store(-1, std::memory_order_relaxed);
        entry.peak.store(0.0f, std::memory_order_relaxed);
    }

    const int fifoCapacity = 2 * (frameSize + samplesPerBlock);
    const auto makeFifos = [&](std::vector<FrameFifo>& fifos) {
        fifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
        for (auto& fifo : fifos) fifo.setCapacity(fifoCapacity);
    };
    makeFifos(micInFifos);
    makeFifos(micOutFifos);
    refInFifo.setCapacity(fifoCapacity);
    makeFifos(dryDelayFifos);
    makeFifos(internalDelayFifos);
    makeFifos(bypassInFifos);
    makeFifos(bypassDelayFifos);
    makeFifos(bypassCompFifos);

    const auto perChannel = [&](int length) {
        return std::vector<std::vector<float>>(static_cast<size_t>(numMicChannels),
                                               std::vector<float>(static_cast<size_t>(length), 0.0f));
    };
    micFrameBuffers = perChannel(frameSize);
    micFramePtrs.assign(static_cast<size_t>(numMicChannels), nullptr);
    for (int ch = 0; ch < numMicChannels; ++ch)
        micFramePtrs[static_cast<size_t>(ch)] = micFrameBuffers[static_cast<size_t>(ch)].data();
    refFrameBuffer.assign(static_cast<size_t>(frameSize), 0.0f);
    dryFrameScratch = perChannel(frameSize);
    dryOutputScratch = perChannel(samplesPerBlock);
    bypassFrameScratch = perChannel(frameSize);
    bypassOutputScratch = perChannel(samplesPerBlock);
    inputFilterScratch = perChannel(samplesPerBlock);

    silenceBuffer.assign(static_cast<size_t>(samplesPerBlock), 0.0f);
    referenceIsMainInput.store(false, std::memory_order_relaxed);
    referenceFilterScratch.assign(static_cast<size_t>(samplesPerBlock), 0.0f);
    bypassMixScratch.assign(static_cast<size_t>(samplesPerBlock), 0.0f);
    dryPrefillSilence.assign(static_cast<size_t>(std::max(frameSize, KalmanEchoCanceller::suppressorDelaySamples)), 0.0f);

    inputHpfChains.clear();
    inputHpfChains.resize(static_cast<size_t>(numMicChannels));
    referenceHpfChain.reset();
    appliedHpfFrequency = hpfFrequencyParam->get();
    for (auto& chain : inputHpfChains) {
        chain.reset();
        chain.setCutoff(sampleRate, appliedHpfFrequency);
    }
    referenceHpfChain.setCutoff(sampleRate, appliedHpfFrequency);

    // Jump straight to the parameter's current state at stream start -- the
    // ramp exists to soften live toggles, not to fade in a session that was
    // already bypassed when playback began.
    bypassMixCurrent = bypassParam->get() ? 1.0f : 0.0f;
    bypassRampStep = 1.0f / static_cast<float>(std::max(1.0, 0.005 * sampleRate)); // ~5ms

    // The true input-to-output delay: one frame of FIFO buffering (primed in
    // resyncDryDelay, so it holds at any host block size) plus the
    // suppressor FIR's delay. 192 samples (4.0 ms) at 48 kHz. Fixed for a
    // given sample rate: no control changes it.
    setLatencySamples(kalman.getLatencySamples());
    resyncDryDelay();
}

void PAEchoCancellerAudioProcessor::releaseResources()
{
    kalman.reset(); // keeps its allocation for the next prepareToPlay; clears the delay readout
}

juce::AudioProcessorParameter* PAEchoCancellerAudioProcessor::getBypassParameter() const
{
    return bypassParam;
}

float PAEchoCancellerAudioProcessor::getInputPeakLevelPostDelayed(int delaySamples) const noexcept
{
    // See the header comment. delaySamples (getLatencySamples()) is the
    // true delay, so no extra margin: looking back further would match a
    // transient *after* the output had already returned to quiet.
    const int64_t targetPosition =
        currentSamplePosition.load(std::memory_order_relaxed) - static_cast<int64_t>(delaySamples);

    // Find the oldest recorded block that still covers targetPosition (the
    // smallest recorded position >= targetPosition) -- i.e. the block whose
    // sample range actually contains the delayed instant we want, not one
    // that ended before it.
    int64_t bestPosition = -1;
    float bestPeak = 0.0f;
    for (auto& entry : inputPeakHistory) {
        const int64_t pos = entry.samplePosition.load(std::memory_order_relaxed);
        if (pos < 0) continue; // never written
        if (pos >= targetPosition && (bestPosition < 0 || pos < bestPosition)) {
            bestPosition = pos;
            bestPeak = entry.peak.load(std::memory_order_relaxed);
        }
    }

    // History doesn't reach back far enough -- only possible right after
    // prepareToPlay, or with unusually tiny host block sizes filling the
    // ring buffer faster than delaySamples' worth of audio. Fall back to
    // the latest known peak rather than under-reporting as silence.
    return bestPosition >= 0 ? bestPeak : getInputPeakLevelPost();
}

void PAEchoCancellerAudioProcessor::resyncDryDelay()
{
    // Called with every FIFO freshly allocated (prepareToPlay). dryDelayFifos
    // and micOutFifos receive identical amounts at identical points in the
    // frame-processing loop (see the comment on dryFrameScratch in
    // PluginProcessor.h), so starting both empty keeps their FIFO
    // bookkeeping aligned.
    for (auto& fifo : micInFifos) fifo.reset();
    for (auto& fifo : micOutFifos) fifo.reset();
    for (auto& fifo : bypassInFifos) fifo.reset(); // in lockstep with micInFifos
    refInFifo.reset();
    for (auto& fifo : dryDelayFifos) fifo.reset();
    for (auto& fifo : bypassDelayFifos) fifo.reset();

    // The suppressor FIR's delay gets its own plain fixed delay line, read
    // and written at the same host-block granularity every call. That
    // separation matters: a silence prefill only behaves like a clean,
    // uniform extra delay when every write to the FIFO is the same regular
    // size. In dryDelayFifos, whose writes arrive in frameSize chunks, the
    // leftover prefill would end up spliced into the *middle* of the first
    // real frame instead of cleanly preceding it. The bypass path gets the
    // same stage.
    const int internalDelay = KalmanEchoCanceller::suppressorDelaySamples;
    for (auto& fifo : internalDelayFifos) {
        fifo.reset();
        fifo.write(dryPrefillSilence.data(), internalDelay);
    }
    for (auto& fifo : bypassCompFifos) {
        fifo.reset();
        fifo.write(dryPrefillSilence.data(), internalDelay);
    }

    // Prime the output side of the frame FIFOs with one frame of silence.
    // Without it the FIFO's delay depended on how the host's block size
    // lines up with the frame: zero when blocks are a multiple of the frame
    // (e.g. 128-sample frames in 512-sample blocks), otherwise up to a
    // frame, settling only after a few zero-padded underruns at startup.
    // Primed, the true delay is always exactly frameSize + the FIR delay --
    // what getLatencySamples() reports -- at any block size, and the
    // startup underruns are gone. The dry and bypass delay lines get the
    // same frame so they stay in lockstep.
    for (auto& fifo : micOutFifos) fifo.write(dryPrefillSilence.data(), frameSize);
    for (auto& fifo : dryDelayFifos) fifo.write(dryPrefillSilence.data(), frameSize);
    for (auto& fifo : bypassDelayFifos) fifo.write(dryPrefillSilence.data(), frameSize);
}

void PAEchoCancellerAudioProcessor::updateKalmanSettings()
{
    // Tail Length: the filter length itself.
    static constexpr double tailSeconds[] = { 0.05, 0.2, 0.4, 0.8 };
    kalman.setTailSeconds(tailSeconds[juce::jlimit(0, 3, tailLengthParam->getIndex())]);

    kalman.setSuppressor(KalmanEchoCanceller::settingsForAmount(
        amountParam->get() / 100.0f, maxReductionParam->get(), responseParam->get()));
}

bool PAEchoCancellerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;
    if (mainOut != mainIn)
        return false;

    if (layouts.inputBuses.size() > 1) {
        const auto reference = layouts.inputBuses[1];
        if (!reference.isDisabled() && reference != juce::AudioChannelSet::mono())
            return false;
    }

    return true;
}

bool PAEchoCancellerAudioProcessor::referenceDuplicatesMainInput(const juce::AudioBuffer<float>& mainIn,
                                                                const float* ref, int numSamples) noexcept
{
    // The mono Reference bus may get the left channel, the right, or a
    // mono fold-down of a stereo input; any of them counts. The tolerance
    // only absorbs float rounding in a host's fold-down; a real PA feed
    // differs from the mic by orders of magnitude more.
    const int numChannels = mainIn.getNumChannels();
    if (numChannels == 0)
        return false;

    const float* left = mainIn.getReadPointer(0);
    const float* right = mainIn.getReadPointer(juce::jmin(1, numChannels - 1));
    bool matchesLeft = true, matchesRight = true, matchesAverage = true, matchesSum = true;

    for (int s = 0; s < numSamples; ++s) {
        const float tolerance = 1.0e-5f * std::abs(ref[s]) + 1.0e-10f;
        matchesLeft = matchesLeft && std::abs(ref[s] - left[s]) <= tolerance;
        matchesRight = matchesRight && std::abs(ref[s] - right[s]) <= tolerance;
        matchesAverage = matchesAverage && std::abs(ref[s] - 0.5f * (left[s] + right[s])) <= tolerance;
        matchesSum = matchesSum && std::abs(ref[s] - (left[s] + right[s])) <= tolerance;
        if (!(matchesLeft || matchesRight || matchesAverage || matchesSum))
            return false;
    }
    return true;
}

void PAEchoCancellerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    processBlockCallCount.fetch_add(1, std::memory_order_relaxed);

    const int totalNumSamples = buffer.getNumSamples();
    const int numMicChannels = static_cast<int>(micInFifos.size());

    if (numMicChannels == 0 || totalNumSamples == 0)
        return;

    // Every control applies live and allocation-free: Tail Length keeps
    // the near part of the filter, the suppressor settings take effect
    // from the next block. Cheap enough to just apply every block.
    updateKalmanSettings();

    // HPF frequency changed: swap the IIR coefficients in place. The
    // filter's state persists across the change, so it is click-free.
    const float desiredHpfFrequency = hpfFrequencyParam->get();
    if (std::abs(desiredHpfFrequency - appliedHpfFrequency) > 0.01f) {
        for (auto& chain : inputHpfChains)
            chain.setCutoff(currentSampleRate, desiredHpfFrequency);
        referenceHpfChain.setCutoff(currentSampleRate, desiredHpfFrequency);
        appliedHpfFrequency = desiredHpfFrequency;
    }

    auto mainIn = getBusBuffer(buffer, true, 0);
    auto mainOut = getBusBuffer(buffer, false, 0);
    auto refIn = getBusBuffer(buffer, true, 1);

    // Logic/MainStage feed the main input to the sidechain bus when Side
    // Chain is None. Checked on the whole host block before anything is
    // written (main in/out may alias). A reference that is a copy of the
    // input is treated as no reference: the canceller gets silence and
    // passes the mic through, exactly as with a disconnected bus.
    bool useReference = refIn.getNumChannels() > 0;
    if (useReference) {
        const float* ref = refIn.getReadPointer(0);
        if (!juce::exactlyEqual(juce::FloatVectorOperations::findMaximum(ref, totalNumSamples), 0.0f)
            || !juce::exactlyEqual(juce::FloatVectorOperations::findMinimum(ref, totalNumSamples), 0.0f))
            referenceIsMainInput.store(referenceDuplicatesMainInput(mainIn, ref, totalNumSamples),
                                       std::memory_order_relaxed);
        useReference = !referenceIsMainInput.load(std::memory_order_relaxed);
    } else {
        referenceIsMainInput.store(false, std::memory_order_relaxed);
    }

    const float refGainLinear = juce::Decibels::decibelsToGain(referenceGainParam->get());

    inputPeakLevelPre.store(mainIn.getMagnitude(0, totalNumSamples), std::memory_order_relaxed);
    // The PA-ref meter has a target zone (see PluginEditor), so even its
    // "pre-HPF" reading must be the level after PA Reference Trim -- the level
    // the canceller actually receives -- or turning the trim would not move
    // the bar toward the zone.
    sidechainPeakLevelPre.store(
        useReference ? refIn.getMagnitude(0, totalNumSamples) * refGainLinear : 0.0f,
        std::memory_order_relaxed);

    const float wetMix = juce::jlimit(0.0f, 1.0f, dryWetMixParam->get() / 100.0f);

    // Everything below works on at most preparedBlockSize samples at a time,
    // because that's what the scratch buffers and FIFOs were sized for in
    // prepareToPlay (see preparedBlockSize in PluginProcessor.h). In the
    // normal case the host honours its own promise and this loop runs
    // exactly once, byte-for-byte identical to processing the whole block
    // outright; the loop only exists so an oversized block degrades into
    // several correct passes instead of a buffer overrun.
    float inputPostPeak = 0.0f;
    float sidechainPostPeak = 0.0f;

    for (int chunkStart = 0; chunkStart < totalNumSamples; chunkStart += preparedBlockSize) {
        const int numSamples = juce::jmin(preparedBlockSize, totalNumSamples - chunkStart);

        // Pull everything the host handed us into the FIFOs before writing any
        // output -- main in/out can alias the same underlying memory, so all
        // reads must happen before the first write. Filtered into scratch
        // buffers rather than in place, since the pre-filter peak levels above
        // (and potentially the host's own buffer) still need the original data.
        float chunkInputPeak = 0.0f;
        for (int ch = 0; ch < numMicChannels; ++ch) {
            const int srcCh = juce::jmin(ch, mainIn.getNumChannels() - 1);
            const float* src = mainIn.getReadPointer(srcCh) + chunkStart;
            // Bypass tap: the raw samples, before the HPF touches anything --
            // written here (input stage) so nothing downstream can have
            // clobbered the host buffer yet (main in/out may alias).
            bypassInFifos[static_cast<size_t>(ch)].write(src, numSamples);
            float* scratch = inputFilterScratch[static_cast<size_t>(ch)].data();
            auto& chain = inputHpfChains[static_cast<size_t>(ch)];
            for (int s = 0; s < numSamples; ++s) {
                scratch[s] = chain.processSample(src[s]);
                chunkInputPeak = std::max(chunkInputPeak, std::abs(scratch[s]));
            }
            micInFifos[static_cast<size_t>(ch)].write(scratch, numSamples);
        }
        inputPostPeak = std::max(inputPostPeak, chunkInputPeak);

        // Record this chunk's Input peak against the running sample count, for
        // getInputPeakLevelPostDelayed() to look up later once the matching
        // (delayed) Output has actually come out the other end.
        const int64_t inputSamplePosition =
            currentSamplePosition.fetch_add(numSamples, std::memory_order_relaxed) + numSamples;
        {
            const int idx = inputPeakHistoryWriteIndex.load(std::memory_order_relaxed);
            inputPeakHistory[static_cast<size_t>(idx)].samplePosition.store(inputSamplePosition, std::memory_order_relaxed);
            inputPeakHistory[static_cast<size_t>(idx)].peak.store(chunkInputPeak, std::memory_order_relaxed);
            inputPeakHistoryWriteIndex.store((idx + 1) % peakHistoryCapacity, std::memory_order_relaxed);
        }

        {
            const float* src = useReference ? refIn.getReadPointer(0) + chunkStart
                                                          : silenceBuffer.data();
            float* scratch = referenceFilterScratch.data();
            for (int s = 0; s < numSamples; ++s) {
                scratch[s] = referenceHpfChain.processSample(src[s]) * refGainLinear;
                sidechainPostPeak = std::max(sidechainPostPeak, std::abs(scratch[s]));
            }
            refInFifo.write(scratch, numSamples);
        }

        while (refInFifo.availableToRead() >= frameSize && micInFifos[0].availableToRead() >= frameSize) {
            for (int ch = 0; ch < numMicChannels; ++ch)
                micInFifos[static_cast<size_t>(ch)].read(micFramePtrs[static_cast<size_t>(ch)], frameSize);
            refInFifo.read(refFrameBuffer.data(), frameSize);

            // Stash the still-dry frame before the canceller overwrites
            // micFramePtrs in place with the wet result -- see the
            // dryFrameScratch comment in PluginProcessor.h.
            for (int ch = 0; ch < numMicChannels; ++ch)
                std::copy_n(micFramePtrs[static_cast<size_t>(ch)], frameSize,
                            dryFrameScratch[static_cast<size_t>(ch)].data());

            kalman.processFrame(micFramePtrs.data(), refFrameBuffer.data(), numMicChannels);

            for (int ch = 0; ch < numMicChannels; ++ch) {
                micOutFifos[static_cast<size_t>(ch)].write(micFramePtrs[static_cast<size_t>(ch)], frameSize);
                dryDelayFifos[static_cast<size_t>(ch)].write(dryFrameScratch[static_cast<size_t>(ch)].data(), frameSize);

                // Release one raw frame in lockstep with the wet frame just
                // written -- bypassInFifos necessarily has one available
                // (it received exactly what micInFifos received).
                bypassInFifos[static_cast<size_t>(ch)].read(bypassFrameScratch[static_cast<size_t>(ch)].data(), frameSize);
                bypassDelayFifos[static_cast<size_t>(ch)].write(bypassFrameScratch[static_cast<size_t>(ch)].data(), frameSize);
            }
        }

        // Per-sample bypass crossfade values for this chunk, shared by every
        // channel (advancing the ramp inside the channel loop would fade
        // each channel from a different starting point).
        const float bypassTarget = bypassParam->get() ? 1.0f : 0.0f;
        const bool bypassActive = bypassMixCurrent > 0.0f || bypassTarget > 0.0f;
        if (bypassActive) {
            float mix = bypassMixCurrent;
            for (int s = 0; s < numSamples; ++s) {
                if (mix < bypassTarget)
                    mix = std::min(bypassTarget, mix + bypassRampStep);
                else if (mix > bypassTarget)
                    mix = std::max(bypassTarget, mix - bypassRampStep);
                bypassMixScratch[static_cast<size_t>(s)] = mix;
            }
            bypassMixCurrent = mix;
        }

        for (int ch = 0; ch < mainOut.getNumChannels(); ++ch) {
            const int srcCh = juce::jmin(ch, numMicChannels - 1);
            float* out = mainOut.getWritePointer(ch) + chunkStart;
            micOutFifos[static_cast<size_t>(srcCh)].read(out, numSamples);

            // Always drain the dry delay line at the same pace as the wet
            // output, even at 100% wet -- otherwise it silently backs up and
            // the two paths fall out of alignment for whenever the mix is
            // later turned down.
            float* dry = dryOutputScratch[static_cast<size_t>(srcCh)].data();
            dryDelayFifos[static_cast<size_t>(srcCh)].read(dry, numSamples);

            // Push through the separate fixed delay line too, compensating for
            // the suppressor FIR's delay (see the comment on internalDelayFifos
            // in PluginProcessor.h) -- also drained unconditionally, for the
            // same reason as above.
            internalDelayFifos[static_cast<size_t>(srcCh)].write(dry, numSamples);
            internalDelayFifos[static_cast<size_t>(srcCh)].read(dry, numSamples);

            if (wetMix < 0.999f)
                for (int s = 0; s < numSamples; ++s)
                    out[s] = wetMix * out[s] + (1.0f - wetMix) * dry[s];

            // Bypass delay line: drained unconditionally, same as the dry
            // path above, so its backlog can never drift while bypass is
            // inactive. The crossfade only runs when there's anything to
            // fade (mix at exactly 0 must leave the processed output
            // byte-identical to a build without this feature; mix at
            // exactly 1 hands over the raw delayed input untouched).
            float* byp = bypassOutputScratch[static_cast<size_t>(srcCh)].data();
            bypassDelayFifos[static_cast<size_t>(srcCh)].read(byp, numSamples);
            bypassCompFifos[static_cast<size_t>(srcCh)].write(byp, numSamples);
            bypassCompFifos[static_cast<size_t>(srcCh)].read(byp, numSamples);

            if (bypassActive)
                for (int s = 0; s < numSamples; ++s) {
                    const float b = bypassMixScratch[static_cast<size_t>(s)];
                    out[s] = (1.0f - b) * out[s] + b * byp[s];
                }
        }
    }

    inputPeakLevelPost.store(inputPostPeak, std::memory_order_relaxed);
    sidechainPeakLevelPost.store(sidechainPostPeak, std::memory_order_relaxed);
    outputPeakLevel.store(mainOut.getMagnitude(0, totalNumSamples), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* PAEchoCancellerAudioProcessor::createEditor()
{
    return new PAEchoCancellerAudioProcessorEditor(*this);
}

bool PAEchoCancellerAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String PAEchoCancellerAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PAEchoCancellerAudioProcessor::acceptsMidi() const
{
    return false;
}

bool PAEchoCancellerAudioProcessor::producesMidi() const
{
    return false;
}

bool PAEchoCancellerAudioProcessor::isMidiEffect() const
{
    return false;
}

double PAEchoCancellerAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PAEchoCancellerAudioProcessor::getNumPrograms()
{
    return 1;
}

int PAEchoCancellerAudioProcessor::getCurrentProgram()
{
    return 0;
}

void PAEchoCancellerAudioProcessor::setCurrentProgram(int)
{
}

const juce::String PAEchoCancellerAudioProcessor::getProgramName(int)
{
    return {};
}

void PAEchoCancellerAudioProcessor::changeProgramName(int, const juce::String&)
{
}

void PAEchoCancellerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Keyed by each parameter's own paramID (stable across releases) rather
    // than list position, so adding/reordering parameters later can't shift
    // a saved project's values onto the wrong control.
    juce::ValueTree state("PAEchoCancellerState");
    for (auto* param : getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            if (param != bypassParam) // hosts own bypass state (AU/VST3's own bypass mechanism); persisting our copy would fight theirs on reload
                state.setProperty(withId->paramID, withId->getValue(), nullptr);

    juce::MemoryOutputStream stream(destData, false);
    state.writeToStream(stream);
}

namespace {
// Walks JUCE's binary ValueTree format (ValueTree::writeToStream) and
// accepts it only if every count and length it claims fits in the bytes
// actually there. ValueTree::readFromData trusts those numbers: a damaged
// or crafted session can claim two billion children or a 2 GB binary
// property, and JUCE then tries to allocate that much (or loops that many
// times) while the host loads the session. Our state is one flat tree of
// numbers, so only plain scalar and string properties are accepted.
class StateBlobChecker {
public:
    StateBlobChecker(const void* data, size_t size) : p(static_cast<const uint8_t*>(data)), end(p + size) {}

    bool isWellFormed() { return p != nullptr && tree(0); }

private:
    const uint8_t* p;
    const uint8_t* end;

    size_t remaining() const { return static_cast<size_t>(end - p); }

    // A count or length: non-negative and no larger than the bytes left.
    bool count(int& out)
    {
        if (remaining() < 1) return false;
        const uint8_t sizeByte = *p++;
        const int numBytes = sizeByte & 0x7f;
        if (numBytes > 4 || (sizeByte & 0x80) != 0 || remaining() < static_cast<size_t>(numBytes)) return false;
        uint32_t v = 0;
        for (int i = 0; i < numBytes; ++i) v |= static_cast<uint32_t>(*p++) << (8 * i);
        if (v > remaining()) return false;
        out = static_cast<int>(v);
        return true;
    }

    bool string(bool mustBeNonEmpty)
    {
        const auto* nul = static_cast<const uint8_t*>(std::memchr(p, 0, remaining()));
        if (nul == nullptr || (mustBeNonEmpty && nul == p)) return false;
        p = nul + 1;
        return true;
    }

    bool value()
    {
        int numBytes = 0;
        if (!count(numBytes)) return false;
        if (numBytes == 0) return true; // void
        const uint8_t marker = *p;
        const size_t payload = static_cast<size_t>(numBytes - 1);
        switch (marker) {
            case 1: if (payload != 4) return false; break; // int
            case 2: case 3: case 9: if (payload != 0) return false; break; // true, false, undefined
            case 4: case 6: if (payload != 8) return false; break; // double, int64
            case 5: break; // string
            default: return false; // arrays, binary blobs, objects: never written by this plugin
        }
        p += numBytes;
        return true;
    }

    bool tree(int depth)
    {
        if (depth > 8 || !string(true)) return false;
        int numProps = 0;
        if (!count(numProps)) return false;
        for (int i = 0; i < numProps; ++i)
            if (!string(true) || !value()) return false;
        int numChildren = 0;
        if (!count(numChildren)) return false;
        for (int i = 0; i < numChildren; ++i)
            if (!tree(depth + 1)) return false;
        return true;
    }
};
} // namespace

void PAEchoCancellerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (sizeInBytes <= 0 || !StateBlobChecker(data, static_cast<size_t>(sizeInBytes)).isWellFormed())
        return; // damaged or foreign data: keep the current settings

    auto state = juce::ValueTree::readFromData(data, static_cast<size_t>(sizeInBytes));
    if (!state.isValid())
        return;

    for (auto* param : getParameters()) {
        if (param == bypassParam)
            continue; // never restored, even from a state that (somehow) contains it -- see getStateInformation
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param)) {
            // A key simply absent (older save, made before some parameter
            // existed) leaves that parameter at its constructor default --
            // no special-casing needed for forward/backward compatibility.
            // A damaged session file can hold any number here. JUCE's
            // parameters pass a NaN straight through to the DSP (where it
            // latches in the suppressor's smoothing and silences the mic),
            // so a non-finite value is skipped and the rest clamped to 0..1.
            if (state.hasProperty(withId->paramID)) {
                const auto value = static_cast<double>(state.getProperty(withId->paramID));
                if (std::isfinite(value))
                    param->setValueNotifyingHost(static_cast<float>(juce::jlimit(0.0, 1.0, value)));
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PAEchoCancellerAudioProcessor();
}
