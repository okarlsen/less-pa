#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace {

// AEC3/APM has its own internal processing delay (subband analysis/
// synthesis framing), entirely separate from -- and in addition to --
// the plugin's own FrameFifo buffering. Since the dry path bypasses AEC3
// completely, it needs to be pushed back by exactly this much on top of
// its own (now sample-accurate, see dryFrameScratch in PluginProcessor.h)
// FIFO-level sync with the wet path, or the two comb-filter when blended.
// Measured empirically to be ~9-10ms but NOT a clean, sample-rate-scalable
// constant (APM's own internal resampling for non-48kHz host rates adds
// its own rate-dependent delay) -- so rather than hardcode a fragile
// magic number, this measures it directly, once per rebuild, using a
// disposable AudioProcessing instance built from the same config: a short
// impulse through a silent reference (AEC3 stays transparent, gain ~1) and
// a peak-position comparison reveals exactly how far AEC3 shifts the
// signal internally.
int measureAec3InternalDelaySamples(const webrtc::EchoCanceller3Config& config, int sampleRate, int frameSize)
{
    auto calibrationApm = webrtc::AudioProcessingBuilder()
                               .SetEchoControlFactory(std::make_unique<TailLengthEchoControlFactory>(config))
                               .Create();
    webrtc::AudioProcessing::Config apmConfig;
    apmConfig.echo_canceller.enabled = true;
    apmConfig.echo_canceller.mobile_mode = false;
    apmConfig.gain_controller1.enabled = false;
    apmConfig.gain_controller2.enabled = false;
    apmConfig.high_pass_filter.enabled = false;
    apmConfig.noise_suppression.enabled = false;
    calibrationApm->ApplyConfig(apmConfig);

    // Prime with silence well past AEC3's ~2.5s initial-state bootstrap
    // first -- measuring this delay while AEC3 is still in its initial
    // ramp-up gave a noticeably different (and less accurate) reading than
    // measuring it once settled, which is also more representative: real
    // audio spends the overwhelming majority of its time in this settled
    // state, not the first ~2.5s.
    constexpr int primingFrames = 300; // 3s at 10ms/frame, past AEC3's ~2.5s initial-state bootstrap
    const std::vector<float> silence(static_cast<size_t>(frameSize), 0.0f);
    std::vector<float> micFrame(static_cast<size_t>(frameSize));
    float* refPtr = const_cast<float*>(silence.data());
    float* micPtr = micFrame.data();
    const webrtc::StreamConfig streamConfig(sampleRate, 1);

    for (int f = 0; f < primingFrames; ++f) {
        std::copy_n(silence.data(), frameSize, micFrame.data());
        calibrationApm->ProcessReverseStream(&refPtr, streamConfig, streamConfig, &refPtr);
        calibrationApm->ProcessStream(&micPtr, streamConfig, streamConfig, &micPtr);
    }

    constexpr int numFrames = 6;
    const int n = numFrames * frameSize;
    std::vector<float> impulse(static_cast<size_t>(n), 0.0f);
    const int impulseStart = (numFrames / 2) * frameSize;
    // Match the shape/duration used to actually diagnose and verify this
    // fix (see testDryWetCombFiltering) -- a short (~1ms) decaying random
    // click, not a longer flat burst. AEC3's internal subband filtering can
    // shift where the apparent peak of a *shaped* transient ends up
    // differently than a flat one, so matching the shape avoids a
    // methodology mismatch between calibration and verification.
    std::mt19937 clickRng(81);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    const int clickLenSamples = std::max(1, static_cast<int>(sampleRate * 0.001));
    for (int i = 0; i < clickLenSamples && impulseStart + i < n; ++i) {
        const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(clickLenSamples);
        impulse[static_cast<size_t>(impulseStart + i)] = 0.8f * envelope * dist(clickRng);
    }

    std::vector<float> output(static_cast<size_t>(n), 0.0f);
    for (int f = 0; f < numFrames; ++f) {
        std::copy_n(impulse.data() + f * frameSize, frameSize, micFrame.data());
        calibrationApm->ProcessReverseStream(&refPtr, streamConfig, streamConfig, &refPtr);
        calibrationApm->ProcessStream(&micPtr, streamConfig, streamConfig, &micPtr);
        std::copy_n(micFrame.data(), frameSize, output.data() + f * frameSize);
    }

    int inputPeakPos = 0, outputPeakPos = 0;
    float inputPeakVal = 0.0f, outputPeakVal = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float inAbs = std::abs(impulse[static_cast<size_t>(i)]);
        const float outAbs = std::abs(output[static_cast<size_t>(i)]);
        if (inAbs > inputPeakVal) { inputPeakVal = inAbs; inputPeakPos = i; }
        if (outAbs > outputPeakVal) { outputPeakVal = outAbs; outputPeakPos = i; }
    }
    return std::max(0, outputPeakPos - inputPeakPos);
}

// The one way an AudioProcessing instance gets constructed for the live
// path, shared by the prepareToPlay rebuild and the background Tail Length
// rebuild thread so the two can never drift out of sync on the APM-level
// config. Returns the factory raw pointer alongside (owned by the returned
// apm -- same non-owning arrangement as activeEchoControlFactory).
rtc::scoped_refptr<webrtc::AudioProcessing> makeConfiguredApm(const webrtc::EchoCanceller3Config& echoConfig,
                                                              TailLengthEchoControlFactory** factoryOut)
{
    auto factory = std::make_unique<TailLengthEchoControlFactory>(echoConfig);
    if (factoryOut != nullptr)
        *factoryOut = factory.get();
    auto apm = webrtc::AudioProcessingBuilder()
                   .SetEchoControlFactory(std::move(factory))
                   .Create();

    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false; // AEC3, not the mobile AECM
    // Everything else off: this plugin does one job (echo cancellation),
    // and every extra submodule is extra latency/CPU for no benefit here.
    config.gain_controller1.enabled = false;
    config.gain_controller2.enabled = false;
    config.high_pass_filter.enabled = false;
    config.noise_suppression.enabled = false;
    apm->ApplyConfig(config);
    return apm;
}

} // namespace

PAEchoCancellerAudioProcessor::PAEchoCancellerAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withInput("Reference", juce::AudioChannelSet::mono(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("Less PA APM rebuild")
{
    addParameter(tailLengthParam = new juce::AudioParameterChoice(
        "tailLength", "Tail Length",
        juce::StringArray{ "50 ms - small room", "200 ms - club / theatre", "400 ms - hall", "800 ms - arena / outdoor" }, 3));
    addParameter(suppressionStrengthParam = new juce::AudioParameterChoice(
        "suppressionStrength", "Suppression Strength", juce::StringArray{ "Gentle", "Moderate", "Hard" }, 1));
    auto hpfRange = juce::NormalisableRange<float>(80.0f, 300.0f, 0.1f);
    hpfRange.setSkewForCentre(150.0f);
    addParameter(hpfFrequencyParam = new juce::AudioParameterFloat(
        "hpfFrequency", "HPF Frequency", hpfRange, 150.0f));
    // "metersPostFilter" (a pre/post-HPF meter toggle) lived here until the
    // panel redesign: the meters now always read post-HPF next to the
    // controls that shape them, so a view setting no longer occupies a host
    // automation slot. Sessions that saved it simply ignore the stale key.
    addParameter(referenceGainParam = new juce::AudioParameterFloat(
        "referenceGain", "PA Reference Trim",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    addParameter(limitHfGainParam = new juce::AudioParameterBool(
        "limitHfGain", "Limit HF Gain", false));
    addParameter(nearendSensitivityParam = new juce::AudioParameterFloat(
        "nearendSensitivity", "Crowd Protection",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 75.0f));
    addParameter(protectionHoldTimeParam = new juce::AudioParameterFloat(
        "protectionHoldTime", "Protection Hold Time",
        juce::NormalisableRange<float>(40.0f, 800.0f, 1.0f), 100.0f)); // 75%/100ms confirmed as a good real-world setting
    addParameter(dryWetMixParam = new juce::AudioParameterFloat(
        "dryWetMix", "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f)); // 100% wet: no change from AEC3's raw output by default
    // The parameters from here down were each appended strictly LAST at the
    // time they were added: verify_main.cpp addresses Tail Length and
    // Suppression Strength by position (getParameters()[0]/[1]), so new
    // parameters must never be inserted above existing ones -- always append.
    addParameter(bypassParam = new juce::AudioParameterBool(
        "bypass", "Bypass", false));
    // Classic (index 0) stays the default deliberately: the subband detector
    // changes protection character (Classic protects ~88% of the time on the
    // reference material, Subband ~32%, specifically in PA gaps), and the
    // listening test -- not the probe's selectivity numbers -- is the
    // acceptance gate for ever changing that default. See
    // applyNearendDetectorChoice in TailLengthEchoControl.h. Unlike bypass
    // above, this IS a real setting and rides the generic paramID
    // save/restore loop in get/setStateInformation.
    addParameter(nearendDetectorParam = new juce::AudioParameterChoice(
        "nearendDetector", "Near-end Detector", juce::StringArray{ "Classic", "Subband (2-4kHz)" }, 0));

    // Appended last, like every parameter added after the initial set:
    // verify_main.cpp addresses Tail Length/Suppression Strength by
    // positional index (getParameters()[0]/[1]), so only appending is safe.
    // 40ms default: long enough to round off the transition, short enough
    // that protection still engages promptly when the crowd comes up. 0
    // restores AEC3's instant swap exactly, so the A/B is a knob turn.
    addParameter(transitionSmoothingParam = new juce::AudioParameterFloat(
        "transitionSmoothing", "Transition Smoothing",
        juce::NormalisableRange<float>(0.0f, 200.0f, 1.0f), 40.0f));

    startThread(); // background Tail Length rebuilds -- see run()
}

PAEchoCancellerAudioProcessor::~PAEchoCancellerAudioProcessor()
{
    // Stop the rebuild thread before any member it touches is destroyed --
    // relying on the juce::Thread base destructor would run it after this
    // class's members are already gone.
    stopThread(4000);
    delete stagedApmSwap.exchange(nullptr);
    delete retiredApmSwap.exchange(nullptr);
}

void PAEchoCancellerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    frameSize = static_cast<int>(std::round(sampleRate / 100.0));
    preparedBlockSize = juce::jmax(1, samplesPerBlock);

    const int numMicChannels = juce::jmax(1, getChannelCountOfBus(true, 0));

    currentSamplePosition.store(0, std::memory_order_relaxed);
    inputPeakHistoryWriteIndex.store(0, std::memory_order_relaxed);
    for (auto& entry : inputPeakHistory) {
        entry.samplePosition.store(-1, std::memory_order_relaxed);
        entry.peak.store(0.0f, std::memory_order_relaxed);
    }

    micInFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
    micOutFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());

    const int fifoCapacity = 2 * (frameSize + samplesPerBlock);
    for (auto& fifo : micInFifos) fifo.setCapacity(fifoCapacity);
    for (auto& fifo : micOutFifos) fifo.setCapacity(fifoCapacity);
    refInFifo.setCapacity(fifoCapacity);

    micFrameBuffers.assign(static_cast<size_t>(numMicChannels), std::vector<float>(static_cast<size_t>(frameSize), 0.0f));
    micFramePtrs.assign(static_cast<size_t>(numMicChannels), nullptr);
    for (int ch = 0; ch < numMicChannels; ++ch)
        micFramePtrs[static_cast<size_t>(ch)] = micFrameBuffers[static_cast<size_t>(ch)].data();

    refFrameBuffer.assign(static_cast<size_t>(frameSize), 0.0f);
    refFramePtrs.assign(1, refFrameBuffer.data());

    dryFrameScratch.assign(static_cast<size_t>(numMicChannels), std::vector<float>(static_cast<size_t>(frameSize), 0.0f));

    silenceBuffer.assign(static_cast<size_t>(samplesPerBlock), 0.0f);

    inputFilterScratch.assign(static_cast<size_t>(numMicChannels), std::vector<float>(static_cast<size_t>(samplesPerBlock), 0.0f));
    referenceFilterScratch.assign(static_cast<size_t>(samplesPerBlock), 0.0f);

    inputHpfChains.clear();
    inputHpfChains.resize(static_cast<size_t>(numMicChannels));
    referenceHpfChain.reset();
    appliedHpfFrequency = hpfFrequencyParam->get();
    for (auto& chain : inputHpfChains) {
        chain.reset();
        chain.setCutoff(sampleRate, appliedHpfFrequency);
    }
    referenceHpfChain.setCutoff(sampleRate, appliedHpfFrequency);

    dryDelayFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
    for (auto& fifo : dryDelayFifos) fifo.setCapacity(fifoCapacity);
    aec3DelayCompensationFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
    for (auto& fifo : aec3DelayCompensationFifos) fifo.setCapacity(fifoCapacity);
    dryOutputScratch.assign(static_cast<size_t>(numMicChannels), std::vector<float>(static_cast<size_t>(samplesPerBlock), 0.0f));

    bypassInFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
    for (auto& fifo : bypassInFifos) fifo.setCapacity(fifoCapacity);
    bypassDelayFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
    for (auto& fifo : bypassDelayFifos) fifo.setCapacity(fifoCapacity);
    bypassCompFifos.assign(static_cast<size_t>(numMicChannels), FrameFifo());
    for (auto& fifo : bypassCompFifos) fifo.setCapacity(fifoCapacity);
    bypassFrameScratch.assign(static_cast<size_t>(numMicChannels), std::vector<float>(static_cast<size_t>(frameSize), 0.0f));
    bypassOutputScratch.assign(static_cast<size_t>(numMicChannels), std::vector<float>(static_cast<size_t>(samplesPerBlock), 0.0f));
    bypassMixScratch.assign(static_cast<size_t>(samplesPerBlock), 0.0f);
    // Jump straight to the parameter's current state at stream start -- the
    // ramp exists to soften live toggles, not to fade in a session that was
    // already bypassed when playback began.
    bypassMixCurrent = bypassParam->get() ? 1.0f : 0.0f;
    bypassRampStep = 1.0f / static_cast<float>(std::max(1.0, 0.005 * sampleRate)); // ~5ms

    // Invalidate anything the background rebuild thread has in flight from
    // a previous configuration -- a build tagged with the old sample rate
    // must never be adopted after this point (the sampleRate tag in
    // StagedApm double-guards that on the claim side too). Deleting here is
    // fine: prepareToPlay is allowed to block and allocate.
    requestedTailLengthIndex.store(-1, std::memory_order_release);
    delete stagedApmSwap.exchange(nullptr);
    delete retiredApmSwap.exchange(nullptr);
    sampleRateForRebuildThread.store(sampleRate, std::memory_order_release);

    // Measure AEC3's internal processing delay once, here, where a slow call
    // is acceptable -- the measurement simulates 3+ seconds of audio through
    // a disposable AudioProcessing instance (see
    // measureAec3InternalDelaySamples), so it must never run from
    // processBlock. The result is reused unchanged by every live Tail
    // Length swap: the delay is architectural (analysis/synthesis framing +
    // internal resampling), a function of sample rate only, not of filter
    // length -- asserted per-rate across all four Tail Lengths by
    // testLatencyInvariantAcrossTailLengths in verify_main.cpp.
    aec3InternalDelaySamples = measureAec3InternalDelaySamples(
        makeEchoCanceller3Config(tailLengthParam->getIndex(), suppressionStrengthParam->getIndex(),
                                 limitHfGainParam->get(), nearendSensitivityParam->get(),
                                 protectionHoldTimeParam->get(),
                                 4.0f, -96.0f, // the shipped erleMin/comfort-noise defaults, spelled out to reach the trailing detector index
                                 nearendDetectorParam->getIndex(), transitionSmoothingParam->get()),
        static_cast<int>(sampleRate), frameSize);
    dryPrefillSilence.assign(static_cast<size_t>(std::max(0, aec3InternalDelaySamples)), 0.0f);

    // Reports the true worst-case buffering delay (one AEC3 frame, plus its
    // own measured internal processing delay) and resyncs the dry delay
    // line -- see rebuildEchoCanceller's and resyncDryDelay's comments.
    rebuildEchoCanceller(tailLengthParam->getIndex(), suppressionStrengthParam->getIndex(),
                         limitHfGainParam->get(), nearendSensitivityParam->get(),
                         protectionHoldTimeParam->get(), nearendDetectorParam->getIndex(),
                         transitionSmoothingParam->get());
}

void PAEchoCancellerAudioProcessor::releaseResources()
{
    // Cancel and GC any in-flight background rebuild first, so the worker
    // can't stage a build for a configuration that no longer exists.
    requestedTailLengthIndex.store(-1, std::memory_order_release);
    delete stagedApmSwap.exchange(nullptr);
    delete retiredApmSwap.exchange(nullptr);

    apm = nullptr;
    activeEchoControlFactory = nullptr; // apm owned it; now dangling if left set
}

juce::AudioProcessorParameter* PAEchoCancellerAudioProcessor::getBypassParameter() const
{
    return bypassParam;
}

float PAEchoCancellerAudioProcessor::getSuppressionDb() const
{
    if (apm == nullptr)
        return 0.0f;
    return static_cast<float>(apm->GetStatistics().echo_return_loss_enhancement.value_or(0.0));
}

int PAEchoCancellerAudioProcessor::getEstimatedEchoPathDelayMs() const
{
    if (apm == nullptr)
        return -1;
    return apm->GetStatistics().delay_ms.value_or(-1);
}

int PAEchoCancellerAudioProcessor::getEchoPathDelayMedianMs() const
{
    if (apm == nullptr)
        return -1;
    return apm->GetStatistics().delay_median_ms.value_or(-1);
}

void PAEchoCancellerAudioProcessor::run()
{
    while (!threadShouldExit()) {
        wait(30);

        // Free whatever the audio thread parked -- this is where the *old*
        // AudioProcessing instance from an adopted swap actually gets
        // destroyed, safely off the audio thread.
        delete retiredApmSwap.exchange(nullptr);

        const int requested = requestedTailLengthIndex.load(std::memory_order_acquire);
        const int applied = appliedTailLengthIndex.load(std::memory_order_relaxed);
        const double sampleRate = sampleRateForRebuildThread.load(std::memory_order_acquire);

        // Take the staged box out to inspect it -- only this thread ever
        // stores into the slot, so putting a still-valid one straight back
        // can't clobber anything (the audio thread only ever exchanges the
        // slot to null).
        StagedApm* staged = stagedApmSwap.exchange(nullptr, std::memory_order_acq_rel);

        if (requested < 0 || requested == applied || sampleRate <= 0.0) {
            delete staged; // nothing wanted (or a re-prepare superseded it) -- GC any leftover build
            continue;
        }

        if (staged != nullptr && staged->tailLengthIndex == requested
            && juce::exactlyEqual(staged->sampleRate, sampleRate)) {
            stagedApmSwap.store(staged, std::memory_order_release); // still the right build; leave it for the audio thread
            continue;
        }
        delete staged; // stale (older request or rate) -- rebuild below

        auto* fresh = new StagedApm();
        buildStagedApm(*fresh, requested, sampleRate);

        delete stagedApmSwap.exchange(fresh, std::memory_order_acq_rel); // replace anything staged meanwhile (there shouldn't be)
    }
}

void PAEchoCancellerAudioProcessor::buildStagedApm(StagedApm& box, int tailLengthIndex, double sampleRate) const
{
    box.tailLengthIndex = tailLengthIndex;
    box.suppressionStrengthIndex = suppressionStrengthParam->getIndex();
    box.limitHfGain = limitHfGainParam->get();
    box.nearendSensitivity = nearendSensitivityParam->get();
    box.protectionHoldTime = protectionHoldTimeParam->get();
    box.nearendDetectorIndex = nearendDetectorParam->getIndex();
    box.transitionSmoothing = transitionSmoothingParam->get();
    box.sampleRate = sampleRate;
    box.apm = makeConfiguredApm(
        makeEchoCanceller3Config(box.tailLengthIndex, box.suppressionStrengthIndex,
                                 box.limitHfGain, box.nearendSensitivity,
                                 box.protectionHoldTime,
                                 4.0f, -96.0f, // shipped defaults, spelled out to reach the trailing detector index
                                 box.nearendDetectorIndex, box.transitionSmoothing),
        &box.factory);
}

void PAEchoCancellerAudioProcessor::adoptStagedApm(StagedApm& box)
{
    std::swap(apm, box.apm); // old instance moves into the box -- freed by whoever owns the box
    activeEchoControlFactory = box.factory;

    // Same clean-restart treatment as rebuildEchoCanceller: discard
    // in-flight audio from the old instance and re-prime the dry path.
    // Latency does NOT change -- aec3InternalDelaySamples is
    // Tail-Length-invariant (see its comment), so no setLatencySamples()
    // here (which wouldn't be audio-thread-safe anyway).
    for (auto& fifo : micInFifos) fifo.reset();
    for (auto& fifo : micOutFifos) fifo.reset();
    for (auto& fifo : bypassInFifos) fifo.reset(); // in lockstep with micInFifos -- see resyncDryDelay
    refInFifo.reset();
    resyncDryDelay();

    appliedTailLengthIndex.store(box.tailLengthIndex, std::memory_order_relaxed);
    appliedSuppressionStrengthIndex = box.suppressionStrengthIndex;
    appliedLimitHfGain = box.limitHfGain;
    appliedNearendSensitivity = box.nearendSensitivity;
    appliedProtectionHoldTime = box.protectionHoldTime;
    appliedNearendDetectorIndex = box.nearendDetectorIndex;
    appliedTransitionSmoothing = box.transitionSmoothing;
}

float PAEchoCancellerAudioProcessor::getInputPeakLevelPostDelayed(int delaySamples) const noexcept
{
    // See the header comment. getLatencySamples() (the caller's
    // delaySamples) already includes aec3InternalDelaySamples, a
    // precisely-measured quantity, so it's already a safely-conservative
    // (not an under-) estimate of the true delay on its own -- no extra
    // margin needed on top of it. An earlier version of this added a full
    // extra frame's worth of margin (left over from before
    // aec3InternalDelaySamples existed, when delaySamples only covered the
    // FIFO wait and needed real slack), which overshot far enough to match
    // a transient *after* the output had already returned to quiet,
    // reintroducing a false reading in the opposite direction.
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

void PAEchoCancellerAudioProcessor::rebuildEchoCanceller(int tailLengthIndex, int suppressionStrengthIndex,
                                                          bool limitHfGain, float nearendSensitivity,
                                                          float protectionHoldTime, int nearendDetectorIndex,
                                                          float transitionSmoothing)
{
    const auto echoConfig = makeEchoCanceller3Config(tailLengthIndex, suppressionStrengthIndex, limitHfGain,
                                                     nearendSensitivity, protectionHoldTime,
                                                     4.0f, -96.0f, // shipped defaults, spelled out to reach the trailing detector index
                                                     nearendDetectorIndex, transitionSmoothing);

    // aec3InternalDelaySamples is deliberately NOT re-measured here: it was
    // measured once in prepareToPlay (a slow, allocation-heavy simulation
    // that must never run per-rebuild now that live rebuilds exist) and is
    // provably identical across Tail Lengths -- see the member's comment in
    // PluginProcessor.h.
    apm = makeConfiguredApm(echoConfig, &activeEchoControlFactory);

    // Discard any in-flight audio from the old instance -- reads will now
    // zero-pad (see FrameFifo::read) until fresh frames refill, giving a
    // clean, silent ramp rather than mixing old and new filter state.
    for (auto& fifo : micInFifos) fifo.reset();
    for (auto& fifo : micOutFifos) fifo.reset();
    for (auto& fifo : bypassInFifos) fifo.reset(); // in lockstep with micInFifos -- see resyncDryDelay
    refInFifo.reset();

    // Reports the TRUE total input-to-output delay: frameSize (the FIFO's
    // wait to accumulate one full AEC3 frame -- mandated by AEC3/APM's own
    // ProcessStream contract, which only accepts exactly sampleRate/100
    // samples per call) plus aec3InternalDelaySamples (AEC3/APM's own
    // internal frame/block/subband-filterbank buffering, measured above --
    // confirmed via reading the vendored AEC3 source, e.g. frame_blocker.cc
    // and three_band_filter_bank.h, that this is structural to AEC3's own
    // processing pipeline, not anything this plugin's own code adds).
    //
    // This used to deliberately under-report (frameSize only, omitting
    // aec3InternalDelaySamples) after a user report that setting it to the
    // true figure raised "the plugin's latency" from 10ms to 19ms, which
    // was rejected as unacceptable for live use. That was a misdiagnosis on
    // my part: the real DSP delay was always ~19-20ms regardless of what
    // got reported -- reporting a smaller number doesn't make the audio
    // arrive any sooner, it only under-informs the host's own delay
    // compensation, which can leave this track out of time with others
    // when a host aligns tracks using reported plugin latency. Once
    // confirmed (this investigation) that neither the Suppression meter's
    // alignment nor the Dry/Wet delay-matching fix add any latency beyond
    // this same floor, and that the floor itself isn't reducible without
    // patching AEC3's own internals (trading cancellation quality/
    // robustness for a latency win, which is explicitly not wanted), there
    // is no smaller true figure to report -- so report this one honestly.
    reportedLatencySamples = frameSize + aec3InternalDelaySamples;
    setLatencySamples(reportedLatencySamples);

    // Reset the dry path alongside the wet path it's now written in
    // lockstep with, and re-prime it with the freshly-measured AEC3-
    // internal delay (see resyncDryDelay's comment).
    resyncDryDelay();

    appliedTailLengthIndex = tailLengthIndex;
    appliedSuppressionStrengthIndex = suppressionStrengthIndex;
    appliedLimitHfGain = limitHfGain;
    appliedNearendSensitivity = nearendSensitivity;
    appliedProtectionHoldTime = protectionHoldTime;
    appliedNearendDetectorIndex = nearendDetectorIndex;
    appliedTransitionSmoothing = transitionSmoothing;
}

void PAEchoCancellerAudioProcessor::applySuppressorConfigLive(int suppressionStrengthIndex, bool limitHfGain,
                                                                float nearendSensitivity, float protectionHoldTime,
                                                                int nearendDetectorIndex,
                                                                float transitionSmoothing)
{
    if (activeEchoControlFactory == nullptr)
        return; // prepareToPlay hasn't run yet; the next rebuildEchoCanceller will pick up current values anyway

    auto* echoCanceller = activeEchoControlFactory->getActiveInstance();
    if (echoCanceller == nullptr)
        return; // AudioProcessing hasn't asked the factory to create one yet

    // Tail Length doesn't matter here -- only .suppressor is used -- so any
    // index reproduces the same suppressor config for a given set of the
    // other five parameters. The detector toggle rides this same path
    // because use_subband_nearend_detection is read in SuppressionGain's
    // constructor, which UpdateSuppressorConfig reconstructs wholesale --
    // the subtractor, AecState, render buffer and delay estimator are never
    // touched, so switching detectors costs no re-convergence.
    const auto config = makeEchoCanceller3Config(appliedTailLengthIndex.load(std::memory_order_relaxed),
                                                 suppressionStrengthIndex, limitHfGain,
                                                 nearendSensitivity, protectionHoldTime,
                                                 4.0f, -96.0f, // shipped defaults, spelled out to reach the trailing detector index
                                                 nearendDetectorIndex, transitionSmoothing);
    echoCanceller->UpdateSuppressorConfig(config.suppressor);

    appliedSuppressionStrengthIndex = suppressionStrengthIndex;
    appliedLimitHfGain = limitHfGain;
    appliedNearendSensitivity = nearendSensitivity;
    appliedProtectionHoldTime = protectionHoldTime;
    appliedNearendDetectorIndex = nearendDetectorIndex;
    appliedTransitionSmoothing = transitionSmoothing;
}

void PAEchoCancellerAudioProcessor::resyncDryDelay()
{
    // dryDelayFifos and micOutFifos receive identical amounts at identical
    // points in the frame-processing loop (see the comment on
    // dryFrameScratch in PluginProcessor.h), so just resetting both to
    // empty together keeps their *FIFO bookkeeping* aligned -- no prefill
    // here, deliberately (see below for why).
    for (auto& fifo : dryDelayFifos) fifo.reset();

    // aec3InternalDelaySamples (see measureAec3InternalDelaySamples) still
    // needs compensating for separately, in its own dedicated FIFO used
    // purely as a plain fixed delay line, read/written at the same
    // host-block granularity every call. That separation matters: a
    // silence prefill only behaves like a clean, uniform extra delay when
    // every write to the FIFO is the same small, regular size. Applying it
    // directly to dryDelayFifos (as an earlier version of this fix did)
    // breaks that, because dryDelayFifos' writes arrive in large, irregular
    // frameSize chunks -- the leftover prefill silence ends up spliced into
    // the *middle* of the first real frame's data instead of cleanly
    // preceding it, corrupting the alignment instead of fixing it.
    for (auto& fifo : aec3DelayCompensationFifos) fifo.reset();
    if (aec3InternalDelaySamples > 0) {
        // dryPrefillSilence was pre-sized in prepareToPlay: this also runs
        // from processBlock (live Tail Length adoption), where allocating a
        // fresh vector -- as this used to -- is not acceptable.
        for (auto& fifo : aec3DelayCompensationFifos)
            fifo.write(dryPrefillSilence.data(), aec3InternalDelaySamples);
    }

    // The bypass path is structured identically to the dry path (see the
    // bypass FIFOs' comment in PluginProcessor.h), so it resyncs the same
    // way and at the same moments: delay stage reset empty alongside the
    // wet FIFOs it's in lockstep with, compensation stage re-primed with
    // AEC3's internal delay. Both call sites reset bypassInFifos alongside
    // micInFifos for the same lockstep reason.
    for (auto& fifo : bypassDelayFifos) fifo.reset();
    for (auto& fifo : bypassCompFifos) fifo.reset();
    if (aec3InternalDelaySamples > 0)
        for (auto& fifo : bypassCompFifos)
            fifo.write(dryPrefillSilence.data(), aec3InternalDelaySamples);
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

void PAEchoCancellerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    processBlockCallCount.fetch_add(1, std::memory_order_relaxed);

    const int totalNumSamples = buffer.getNumSamples();
    const int numMicChannels = static_cast<int>(micInFifos.size());

    if (numMicChannels == 0 || totalNumSamples == 0)
        return;

    // Tail Length actually changes the adaptive filter's length, so it's the
    // only one of the 6 AEC3-config controls that still needs a whole new
    // AEC3 instance. That construction is far too heavy for an audio
    // callback (it used to run right here -- including a ~3-second
    // calibration simulation -- a guaranteed dropout on a live rig), so
    // it's requested from the background rebuild thread instead (see
    // run()): processing continues on the old instance until the new one is
    // staged, then this block adopts it with a pointer swap, resets the
    // FIFOs (briefly silences the output -- zero-padded reads -- rather
    // than risking a click/spike from swapping filter state mid-stream),
    // and parks the old instance for the rebuild thread to free. The other
    // five controls only touch the suppressor, so they apply live via
    // applySuppressorConfigLive() -- no silence, no discarding the filter's
    // convergence. The suppressor branch below stays skipped while a tail
    // swap is pending; the adopted instance is built with the then-current
    // values of the other five, and any change racing past that is caught
    // by the same branch on the next block.
    const int desiredTailLengthIndex = tailLengthParam->getIndex();
    const int desiredSuppressionStrengthIndex = suppressionStrengthParam->getIndex();
    const bool desiredLimitHfGain = limitHfGainParam->get();
    const float desiredNearendSensitivity = nearendSensitivityParam->get();
    const float desiredProtectionHoldTime = protectionHoldTimeParam->get();
    const int desiredNearendDetectorIndex = nearendDetectorParam->getIndex();
    const float desiredTransitionSmoothing = transitionSmoothingParam->get();
    if (desiredTailLengthIndex != appliedTailLengthIndex.load(std::memory_order_relaxed) && isNonRealtime()) {
        // Offline render (bounce/export): the host isn't on a deadline, so
        // build and adopt the new instance right here instead of waiting on
        // the background thread. That thread polls on wall-clock time, and at
        // bounce speed its few tens of ms cover up to a second of audio
        // processed on the wrong instance -- the bounce then sounds different
        // from playback in its first window (~11dB less suppression,
        // measured by testFastBounceTailLengthAppliedPromptly). The old
        // instance is freed when the local box goes out of scope, which is
        // also fine offline.
        StagedApm inlineBuild;
        buildStagedApm(inlineBuild, desiredTailLengthIndex, currentSampleRate);
        adoptStagedApm(inlineBuild);
    } else if (desiredTailLengthIndex != appliedTailLengthIndex.load(std::memory_order_relaxed)) {
        requestedTailLengthIndex.store(desiredTailLengthIndex, std::memory_order_release);

        // Only touch the staged slot while the retired slot is free: the
        // claimed box must always have somewhere to be parked, since this
        // thread can neither free it nor block waiting for a place to.
        if (retiredApmSwap.load(std::memory_order_acquire) == nullptr) {
            if (StagedApm* staged = stagedApmSwap.exchange(nullptr, std::memory_order_acq_rel)) {
                if (staged->tailLengthIndex == desiredTailLengthIndex
                    && juce::exactlyEqual(staged->sampleRate, currentSampleRate)) {
                    adoptStagedApm(*staged); // the old instance is freed later, off this thread
                }
                // Park the box whether adopted (it now holds the old
                // instance) or rejected as stale (wrong tail/rate) -- the
                // rebuild thread deletes it either way.
                retiredApmSwap.store(staged, std::memory_order_release);
            }
        }
    } else if (desiredSuppressionStrengthIndex != appliedSuppressionStrengthIndex ||
              desiredLimitHfGain != appliedLimitHfGain ||
              desiredNearendDetectorIndex != appliedNearendDetectorIndex ||
              std::abs(desiredNearendSensitivity - appliedNearendSensitivity) > 0.05f ||
              std::abs(desiredProtectionHoldTime - appliedProtectionHoldTime) > 0.5f ||
              std::abs(desiredTransitionSmoothing - appliedTransitionSmoothing) > 0.5f) {
        applySuppressorConfigLive(desiredSuppressionStrengthIndex, desiredLimitHfGain,
                                  desiredNearendSensitivity, desiredProtectionHoldTime,
                                  desiredNearendDetectorIndex, desiredTransitionSmoothing);
    }

    // HPF frequency changed: unlike Tail Length, just swap the IIR
    // coefficients in place. The filter's internal state persists across
    // the change, which is the normal, click-free way to handle a live
    // coefficient update (no rebuild/silence needed).
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

    const float refGainLinear = juce::Decibels::decibelsToGain(referenceGainParam->get());

    inputPeakLevelPre.store(mainIn.getMagnitude(0, totalNumSamples), std::memory_order_relaxed);
    // The PA-ref meter has a target zone (see PluginEditor), so even its
    // "pre-HPF" reading must be the level after PA Reference Trim -- the level
    // the canceller actually receives -- or turning the trim would not move
    // the bar toward the zone.
    sidechainPeakLevelPre.store(
        refIn.getNumChannels() > 0 ? refIn.getMagnitude(0, totalNumSamples) * refGainLinear : 0.0f,
        std::memory_order_relaxed);

    const webrtc::StreamConfig refStreamConfig(static_cast<int>(currentSampleRate), 1);
    const webrtc::StreamConfig micStreamConfig(static_cast<int>(currentSampleRate), static_cast<size_t>(numMicChannels));

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
            const float* src = refIn.getNumChannels() > 0 ? refIn.getReadPointer(0) + chunkStart
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
            refInFifo.read(refFramePtrs[0], frameSize);

            // Stash the still-dry frame before ProcessStream overwrites
            // micFramePtrs in place with the wet result -- see the
            // dryFrameScratch comment in PluginProcessor.h.
            for (int ch = 0; ch < numMicChannels; ++ch)
                std::copy_n(micFramePtrs[static_cast<size_t>(ch)], frameSize,
                            dryFrameScratch[static_cast<size_t>(ch)].data());

            apm->ProcessReverseStream(refFramePtrs.data(), refStreamConfig, refStreamConfig, refFramePtrs.data());
            apm->ProcessStream(micFramePtrs.data(), micStreamConfig, micStreamConfig, micFramePtrs.data());

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
            // AEC3's own internal processing delay (see the comment on
            // aec3DelayCompensationFifos in PluginProcessor.h) -- also drained
            // unconditionally, for the same reason as above.
            aec3DelayCompensationFifos[static_cast<size_t>(srcCh)].write(dry, numSamples);
            aec3DelayCompensationFifos[static_cast<size_t>(srcCh)].read(dry, numSamples);

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
    // a saved project's values onto the wrong control -- exactly what
    // silently happened here before this existed, since these two functions
    // were no-ops: every parameter reset to its constructor default on
    // every reload, discarding whatever Tail Length/HPF/Dry-Wet/Suppression
    // Strength had actually been dialed in.
    juce::ValueTree state("PAEchoCancellerState");
    for (auto* param : getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            if (param != bypassParam) // hosts own bypass state (AU/VST3's own bypass mechanism); persisting our copy would fight theirs on reload
                state.setProperty(withId->paramID, withId->getValue(), nullptr);

    juce::MemoryOutputStream stream(destData, false);
    state.writeToStream(stream);
}

void PAEchoCancellerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
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
            if (state.hasProperty(withId->paramID))
                param->setValueNotifyingHost(static_cast<float>(state.getProperty(withId->paramID)));
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PAEchoCancellerAudioProcessor();
}
