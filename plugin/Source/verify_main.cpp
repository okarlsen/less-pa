// Standalone harness that drives PAEchoCancellerAudioProcessor directly
// (bypassing the AU/VST3 host wrapper) to prove the FIFO frame-accumulator
// bridges arbitrary/irregular host block sizes into the canceller's fixed
// frames correctly, at all three required sample rates, and to report the
// actual latency introduced.

#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h> // WAV reading for the --real material harness

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <vector>

namespace {

void writeWav16(const std::string& path, const std::vector<float>& samples, int sampleRate) {
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float v = samples[i] * 32767.0f;
        v = std::max(-32768.0f, std::min(32767.0f, v));
        pcm[i] = static_cast<int16_t>(v);
    }

    std::ofstream f(path, std::ios::binary);
    uint32_t dataSize = static_cast<uint32_t>(pcm.size() * 2);
    uint32_t byteRate = static_cast<uint32_t>(sampleRate) * 2;
    uint16_t blockAlign = 2;
    uint16_t bitsPerSample = 16;
    uint32_t fmtChunkSize = 16;
    uint16_t audioFormat = 1;
    uint16_t numChannels = 1;
    uint32_t sr = static_cast<uint32_t>(sampleRate);
    uint32_t riffChunkSize = 4 + (8 + fmtChunkSize) + (8 + dataSize);

    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&riffChunkSize), 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
    f.write(reinterpret_cast<const char*>(&audioFormat), 2);
    f.write(reinterpret_cast<const char*>(&numChannels), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    f.write(reinterpret_cast<const char*>(&byteRate), 4);
    f.write(reinterpret_cast<const char*>(&blockAlign), 2);
    f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&dataSize), 4);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataSize);
}

std::vector<float> onePoleLowpass(const std::vector<float>& x, float alpha) {
    std::vector<float> y(x.size());
    float prev = 0.0f;
    for (size_t i = 0; i < x.size(); ++i) {
        prev = alpha * prev + (1.0f - alpha) * x[i];
        y[i] = prev;
    }
    return y;
}

struct TestSignals {
    std::vector<float> reference;
    std::vector<float> voice;
    std::vector<float> mic;
};

// taps: (delay in ms, gain) pairs summed into the mic to form the leakage
// path. A single short tap models a dry/direct room; several taps spread
// out over hundreds of ms model a reflective hall.
//
// lowpassAlpha/lowpassStages shape the reference's spectral tilt. Heavier
// filtering (higher alpha, more stages) makes the reference more
// autocorrelated at short lags -- fine for a simple direct-path test, but
// it lets even a short adaptive filter "cheat" at matching a long tail via
// local self-similarity rather than truly modeling it (the same reason
// pure tones are pathological for AEC3 -- see the offline AEC3 test).
// Tests that actually want to distinguish filter lengths should use a
// lighter/broader reference.
TestSignals makeSignals(int sampleRate, double durationS,
                         const std::vector<std::pair<double, float>>& taps = { { 5.0, 0.5f } },
                         float lowpassAlpha = 0.90f, int lowpassStages = 2) {
    const int n = static_cast<int>(sampleRate * durationS);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> white(static_cast<size_t>(n));
    for (auto& v : white) v = dist(rng);

    std::vector<float> reference = white;
    for (int i = 0; i < lowpassStages; ++i)
        reference = onePoleLowpass(reference, lowpassAlpha);
    float peak = 1e-9f;
    for (auto v : reference) peak = std::max(peak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / peak;

    const double voiceStartS = durationS * (2.0 / 6.0);
    const double voiceEndS = durationS * (4.0 / 6.0);
    const int burstLen = static_cast<int>(0.25 * sampleRate);
    const int gapLen = static_cast<int>(0.15 * sampleRate);

    std::vector<float> voice(static_cast<size_t>(n), 0.0f);
    int start = static_cast<int>(voiceStartS * sampleRate);
    int end = static_cast<int>(voiceEndS * sampleRate);
    int i = start;
    bool on = true;
    while (i < end) {
        int segLen = on ? burstLen : gapLen;
        int segEnd = std::min(i + segLen, end);
        if (on) {
            for (int s = i; s < segEnd; ++s) {
                double t = static_cast<double>(s) / sampleRate;
                voice[static_cast<size_t>(s)] = 0.6f * static_cast<float>(std::sin(2.0 * M_PI * 1200.0 * t));
            }
        }
        i = segEnd;
        on = !on;
    }

    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noiseDist(-0.005f, 0.005f);
    for (int s = 0; s < n; ++s) {
        float leak = 0.0f;
        for (const auto& tap : taps) {
            int delaySamples = static_cast<int>(sampleRate * tap.first / 1000.0);
            if (s - delaySamples >= 0)
                leak += tap.second * reference[static_cast<size_t>(s - delaySamples)];
        }
        mic[static_cast<size_t>(s)] = leak + voice[static_cast<size_t>(s)] + noiseDist(rng);
    }

    return { reference, voice, mic };
}

// Runs the processor over the given signals using an irregular, cycling
// block-size pattern designed to land off the 10ms frame boundary. If
// roomChangeAtSample >= 0, the Room Size parameter is switched to
// roomChangeToIndex at that point in the stream (mid-processing, as a user
// turning the knob live would trigger), and changeSampleIndex receives the
// output sample index at which the change was actually applied.
std::vector<float> runThroughProcessor(PAEchoCancellerAudioProcessor& proc, int sampleRate,
                                        const std::vector<float>& reference, const std::vector<float>& mic,
                                        int roomChangeAtSample = -1, int roomChangeToIndex = -1,
                                        int* changeSampleIndex = nullptr, bool nonRealtime = false,
                                        int paramIndexToChange = 0) {
    static const int blockPattern[] = { 512, 37, 129, 1, 4096, 256, 7 };
    const int patternLen = static_cast<int>(sizeof(blockPattern) / sizeof(blockPattern[0]));
    int maxBlock = 0;
    for (int b : blockPattern) maxBlock = std::max(maxBlock, b);

    proc.setNonRealtime(nonRealtime);
    proc.prepareToPlay(sampleRate, maxBlock);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    printf("  [debug] totalIn=%d totalOut=%d totalChannels=%d mainInCh=%d refInCh=%d mainOutCh=%d\n",
           proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels(), totalChannels,
           proc.getChannelCountOfBus(true, 0), proc.getChannelCountOfBus(true, 1), proc.getChannelCountOfBus(false, 0));
    juce::AudioBuffer<float> buffer(totalChannels, maxBlock);

    const int n = static_cast<int>(mic.size());
    std::vector<float> output(static_cast<size_t>(n), 0.0f);
    juce::MidiBuffer midi;

    bool roomChangeApplied = false;

    int pos = 0;
    int patternIdx = 0;
    while (pos < n) {
        int blockSize = std::min(blockPattern[patternIdx % patternLen], n - pos);
        patternIdx++;
        if (blockSize <= 0) continue;

        if (!roomChangeApplied && roomChangeAtSample >= 0 && pos >= roomChangeAtSample) {
            // Mimic what a real host does when the user turns the knob:
            // go through the public parameter API, not a test backdoor.
            auto* param = proc.getParameters()[paramIndexToChange];
            const float normalized =
                static_cast<float>(roomChangeToIndex) / static_cast<float>(param->getNumSteps() - 1);
            param->setValueNotifyingHost(normalized);
            roomChangeApplied = true;
            if (changeSampleIndex != nullptr) *changeSampleIndex = pos;
        }

        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();

        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);

        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);

        if (patternIdx < 3) {
            double micSumSq = 0.0, refSumSq = 0.0;
            for (int s = 0; s < blockSize; ++s) {
                micSumSq += static_cast<double>(mainIn.getSample(0, s)) * mainIn.getSample(0, s);
                if (refIn.getNumChannels() > 0)
                    refSumSq += static_cast<double>(refIn.getSample(0, s)) * refIn.getSample(0, s);
            }
            printf("  [debug] block#%d size=%d mainIn RMS=%.4f refIn RMS=%.4f (refCh=%d)\n",
                   patternIdx, blockSize, std::sqrt(micSumSq / blockSize),
                   refIn.getNumChannels() > 0 ? std::sqrt(refSumSq / blockSize) : -1.0, refIn.getNumChannels());
        }

        proc.processBlock(buffer, midi);

        auto mainOut = proc.getBusBuffer(buffer, false, 0);
        for (int s = 0; s < blockSize; ++s)
            output[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);

        pos += blockSize;
    }

    proc.releaseResources();
    return output;
}

double rmsDbfs(const std::vector<float>& samples, int sampleRate, double startS, double endS) {
    int a = static_cast<int>(startS * sampleRate);
    int b = static_cast<int>(endS * sampleRate);
    a = std::max(0, a);
    b = std::min(static_cast<int>(samples.size()), b);
    if (b <= a) return -1000.0;
    double sumSq = 0.0;
    for (int i = a; i < b; ++i) sumSq += static_cast<double>(samples[static_cast<size_t>(i)]) * samples[static_cast<size_t>(i)];
    double rms = std::sqrt(sumSq / (b - a));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -1000.0;
}

bool setMonoLayout(PAEchoCancellerAudioProcessor& proc) {
    PAEchoCancellerAudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::mono());
    layout.inputBuses.add(juce::AudioChannelSet::mono());
    layout.outputBuses.add(juce::AudioChannelSet::mono());
    return proc.setBusesLayout(layout);
}

// User report: muting the reference track (so no PA signal reaches the
// sidechain) still shows gain reduction on the Suppression meter, even
// though far more audio is audibly getting through. Reproduces that exact
// scenario -- converge on real leak+audience material, then zero the
// reference while audience content continues -- and polls the *editor's*
// measured-suppression formula (getInputPeakLevelPostDelayed() minus
// getOutputPeakLevel(), floored, matching PluginEditor.cpp's timerCallback)
// to see how long any residual reading actually persists post-mute.
bool testSuppressionMeterAfterReferenceMute(int sampleRate, float amountPercent, const char* amountName) {
    printf("\n=== Suppression meter after reference mute test (%d Hz, %s) ===\n",
           sampleRate, amountName);
    const double totalDurationS = 10.0;
    const double muteAtS = 6.0; // longer pre-mute run so a long reverberant tail actually converges
    const int n = static_cast<int>(sampleRate * totalDurationS);
    const int muteSample = static_cast<int>(sampleRate * muteAtS);

    std::mt19937 refRng(21), audienceRng(22);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    for (int stage = 0; stage < 2; ++stage) reference = onePoleLowpass(reference, 0.9f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;
    for (int s = muteSample; s < n; ++s) reference[static_cast<size_t>(s)] = 0.0f; // the "mute"

    std::vector<float> audience(static_cast<size_t>(n));
    for (auto& v : audience) v = dist(audienceRng);
    HighPassFilterChain audienceHpf;
    audienceHpf.setCutoff(sampleRate, 1000.0f);
    for (auto& v : audience) v = audienceHpf.processSample(v);
    float audiencePeak = 1e-9f;
    for (float v : audience) audiencePeak = std::max(audiencePeak, std::abs(v));
    for (auto& v : audience) v = 0.4f * v / audiencePeak; // present throughout, before and after the mute

    // A single 5ms tap (as used in earlier tests) is a dry, direct path with
    // no reverberant tail, so it can't exercise the mechanism this test is
    // actually after. This uses the same spread-out, decaying multi-tap room
    // response as testLargeRoomBenefit (5ms direct plus reflections out to
    // 320ms) so the adaptive filter has a genuine decaying tail to learn, the
    // same way a real hall recording would (see the Oslo Spektrum test
    // material notes on why short synthetic taps can miss real-material
    // behavior).
    const std::vector<std::pair<double, float>> roomTaps = {
        { 5.0, 0.5f }, { 80.0, 0.3f }, { 180.0, 0.2f }, { 320.0, 0.15f }
    };
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = 0.0f;
        for (const auto& tap : roomTaps) {
            const int delaySamples = static_cast<int>(sampleRate * tap.first / 1000.0);
            if (s - delaySamples >= 0)
                leak += tap.second * reference[static_cast<size_t>(s - delaySamples)];
        }
        mic[static_cast<size_t>(s)] = leak + audience[static_cast<size_t>(s)];
    }

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    // Everything else stays at its plugin default (Tail Length 800ms, Max
    // Reduction -12 dB, Response 20 ms) -- exactly what a user hears out of
    // the box.
    proc.getAmountParameter()->setValueNotifyingHost(proc.getAmountParameter()->convertTo0to1(amountPercent));

    const int blockSize = sampleRate / 100; // 10ms
    proc.prepareToPlay(sampleRate, blockSize);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;

    constexpr float levelFloorLinear = 1.0e-6f;
    const std::vector<double> checkpointsS = { 0.0, 0.05, 0.1, 0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 3.9 }; // seconds after mute
    size_t nextCheckpoint = 0;
    std::vector<float> suppressionAtCheckpoint;

    int pos = 0;
    while (pos + blockSize <= n) {
        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();

        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);

        proc.processBlock(buffer, midi);
        pos += blockSize;

        const double elapsedSincemuteS = static_cast<double>(pos - muteSample) / sampleRate;
        if (pos >= muteSample && nextCheckpoint < checkpointsS.size() &&
            elapsedSincemuteS >= checkpointsS[nextCheckpoint]) {
            const float inputLevel = proc.getInputPeakLevelPostDelayed(proc.getLatencySamples());
            const float outputLevel = proc.getOutputPeakLevel();
            const float suppressionDb =
                juce::jmax(0.0f, juce::Decibels::gainToDecibels(juce::jmax(inputLevel, levelFloorLinear)) -
                                     juce::Decibels::gainToDecibels(juce::jmax(outputLevel, levelFloorLinear)));
            suppressionAtCheckpoint.push_back(suppressionDb);
            printf("  t=mute+%.2fs  input=%.4f output=%.4f  measured suppression=%.1f dB\n",
                   elapsedSincemuteS, inputLevel, outputLevel, suppressionDb);
            ++nextCheckpoint;
        }
    }
    proc.releaseResources();

    const float finalDb = suppressionAtCheckpoint.empty() ? -1.0f : suppressionAtCheckpoint.back();
    const bool pass = finalDb >= 0.0f && finalDb < 3.0f;
    printf("  %s\n", pass ? "PASS -- measured suppression converges back to ~0dB after the reference goes silent"
                           : "CHECK -- measured suppression stays elevated well after the reference is silent");
    return pass;
}

// User report: a fast near-end transient (kick/snare) makes the Suppression
// meter show significant activity even with no real echo to remove.
// Mechanism: Output at any instant reflects Input from getLatencySamples()
// earlier (one frame of buffering plus the suppressor FIR's delay) --
// comparing it against
// the *live* Input peak means a sharp attack sitting in the current Input
// block hasn't reached the Output yet, and looks "suppressed" purely from
// that timing gap. Confirms getInputPeakLevelPostDelayed() fixes it: a
// steady near-end bed (no leak, nothing to genuinely suppress) gets one
// short, loud click injected, and both the naive (live-input) and the
// delay-compensated formula are polled at fine (128-sample) resolution
// through the click to show the naive one spiking while the compensated one
// stays flat.
bool testSuppressionMeterTransientAlignment(int sampleRate) {
    printf("\n=== Suppression meter transient alignment test (%d Hz) ===\n", sampleRate);
    const double totalDurationS = 3.0;
    const double clickAtS = 2.0;
    const int n = static_cast<int>(sampleRate * totalDurationS);
    const int clickSample = static_cast<int>(sampleRate * clickAtS);
    const int clickLenSamples = static_cast<int>(sampleRate * 0.003); // ~3ms sharp attack, like a drum hit

    std::mt19937 audienceRng(33), clickRng(34);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Steady near-end bed, no PA leak at all -- nothing here should ever
    // register as "suppressed"; this isolates the transient-alignment
    // artifact from any real echo-cancellation activity.
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (auto& v : mic) v = dist(audienceRng);
    HighPassFilterChain audienceHpf;
    audienceHpf.setCutoff(sampleRate, 1000.0f);
    for (auto& v : mic) v = audienceHpf.processSample(v);
    float bedPeak = 1e-9f;
    for (float v : mic) bedPeak = std::max(bedPeak, std::abs(v));
    for (auto& v : mic) v = 0.2f * v / bedPeak;

    for (int i = 0; i < clickLenSamples; ++i) {
        const int s = clickSample + i;
        if (s >= n) break;
        const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(clickLenSamples); // sharp decay
        mic[static_cast<size_t>(s)] += 0.9f * envelope * dist(clickRng);
    }

    const std::vector<float> reference(static_cast<size_t>(n), 0.0f); // no PA content whatsoever

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    const int blockSize = 128; // small, realistic low-latency host block -- fine polling resolution
    proc.prepareToPlay(sampleRate, blockSize);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;

    constexpr float levelFloorLinear = 1.0e-6f;
    auto toDb = [](float linear) { return juce::Decibels::gainToDecibels(juce::jmax(linear, levelFloorLinear)); };

    float worstNaiveDb = 0.0f;
    float worstCompensatedDb = 0.0f;

    int pos = 0;
    while (pos + blockSize <= n) {
        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();

        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);

        proc.processBlock(buffer, midi);
        pos += blockSize;

        // Only look in the window bracketing the click -- well after initial
        // convergence, well before the buffers run out.
        if (pos >= clickSample - 2000 && pos <= clickSample + proc.getLatencySamples() + 2000) {
            const float outputLevel = proc.getOutputPeakLevel();
            const float naiveDb = toDb(proc.getInputPeakLevelPost()) - toDb(outputLevel);
            const float compensatedDb =
                toDb(proc.getInputPeakLevelPostDelayed(proc.getLatencySamples())) - toDb(outputLevel);
            worstNaiveDb = std::max(worstNaiveDb, naiveDb);
            worstCompensatedDb = std::max(worstCompensatedDb, compensatedDb);
        }
    }
    proc.releaseResources();

    printf("  Worst apparent suppression through the click: naive(live input)=%.1f dB  delay-compensated=%.1f dB\n",
           worstNaiveDb, worstCompensatedDb);

    const bool pass = worstNaiveDb > 6.0f && worstCompensatedDb < 2.0f;
    printf("  %s\n", pass ? "PASS -- delay compensation removes the transient's false suppression reading"
                           : "CHECK -- transient alignment fix isn't behaving as expected");
    return pass;
}

// Confirms that changing Tail Length mid-stream never produces a spike, and
// that it applies live: the canceller keeps the near part of its filter, so
// the output must not drop out either.
bool testClickFreeRoomChange(int sampleRate) {
    printf("\n=== Click-free Tail Length change test (%d Hz) ===\n", sampleRate);
    const double durationS = 6.0;
    auto signals = makeSignals(sampleRate, durationS);

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }

    // Target index 0 ("50ms"): must differ from the plugin's default (index 3,
    // "800ms") or the parameter "change" is a no-op.
    const int changeAtSample = static_cast<int>(sampleRate * durationS / 2.0);
    int changeSampleIndex = -1;
    auto output = runThroughProcessor(proc, sampleRate, signals.reference, signals.mic,
                                       changeAtSample, /* 50ms */ 0, &changeSampleIndex);

    float inputPeak = 0.0f;
    for (float v : signals.mic) inputPeak = std::max(inputPeak, std::abs(v));

    float outputPeak = 0.0f;
    int spikeIndex = -1;
    for (size_t i = 0; i < output.size(); ++i) {
        float a = std::abs(output[i]);
        outputPeak = std::max(outputPeak, a);
        if (a > inputPeak * 1.5f && spikeIndex < 0) spikeIndex = static_cast<int>(i);
    }

    // A dropout would show as a run of exact zeros (the old AEC3 engine
    // restarted with zero-padded FIFO reads here); the Kalman filter must
    // carry on without one.
    const int minZeroRun = sampleRate / 200; // ~5ms
    int zeroRunStart = -1, zeroRun = 0, foundZeroRunAt = -1;
    for (size_t i = static_cast<size_t>(std::max(0, changeSampleIndex)); i < output.size(); ++i) {
        if (std::abs(output[i]) < 1.0e-7f) {
            if (zeroRun == 0) zeroRunStart = static_cast<int>(i);
            if (++zeroRun >= minZeroRun) { foundZeroRunAt = zeroRunStart; break; }
        } else {
            zeroRun = 0;
        }
    }

    printf("  Tail Length change requested at sample %d (param set at sample %d)\n", changeAtSample, changeSampleIndex);
    printf("  Input peak (mic): %.4f   Output peak (whole run): %.4f\n", inputPeak, outputPeak);
    printf("  Zero gap (>=%d samples) after the change: %s\n", minZeroRun,
           foundZeroRunAt >= 0 ? "FOUND" : "none");

    const bool noSpike = (spikeIndex < 0);
    const bool noGap = (foundZeroRunAt < 0);
    const bool pass = noSpike && noGap;
    printf("  %s%s%s\n", pass ? "PASS" : "CHECK", noSpike ? "" : " -- SPIKE DETECTED",
           noGap ? "" : " -- OUTPUT DROPPED OUT");
    return pass;
}

// Confirms the Large preset actually earns its CPU cost: a room with echo
// energy spread out to ~320ms should cancel much better with a 400ms filter
// than with the 52ms Small default.
bool testLargeRoomBenefit(int sampleRate) {
    printf("\n=== Large room tail benefit test (%d Hz) ===\n", sampleRate);
    const double durationS = 6.0;
    const std::vector<std::pair<double, float>> roomTaps = {
        { 5.0, 0.5f }, { 80.0, 0.3f }, { 180.0, 0.2f }, { 320.0, 0.15f }
    };
    // Lightly-filtered (broadband) reference: distinguishing a 52ms filter
    // from a 400ms one requires a reference that a short filter can't
    // "cheat" on via short-lag self-correlation (see makeSignals comment).
    auto signals = makeSignals(sampleRate, durationS, roomTaps, /*lowpassAlpha*/ 0.3f, /*lowpassStages*/ 1);

    auto runWithRoomSize = [&](int roomSizeIndex) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        auto* param = proc.getParameters()[0];
        param->setValueNotifyingHost(static_cast<float>(roomSizeIndex) / static_cast<float>(param->getNumSteps() - 1));
        return runThroughProcessor(proc, sampleRate, signals.reference, signals.mic);
    };

    auto smallOutput = runWithRoomSize(0);
    auto largeOutput = runWithRoomSize(2);

    // Voice occupies 2.0-4.0s here, so measure the PA-only tail (4.0-6.0s):
    // past both the canceller's start-up and the voice burst.
    const double picS0 = durationS * (4.0 / 6.0), picS1 = durationS;
    const double mic_pa = rmsDbfs(signals.mic, sampleRate, picS0, picS1);
    const double small_pa = rmsDbfs(smallOutput, sampleRate, picS0, picS1);
    const double large_pa = rmsDbfs(largeOutput, sampleRate, picS0, picS1);

    printf("  PA-only window: mic=%.1f dBFS  Small=%.1f dBFS (%.1f dB reduction)  Large=%.1f dBFS (%.1f dB reduction)\n",
           mic_pa, small_pa, mic_pa - small_pa, large_pa, mic_pa - large_pa);

    // The suppressor after the filter compensates somewhat for a too-short
    // filter, so a dramatic dB gap isn't the right bar. What matters is
    // that Large is consistently better, never worse, on a tail the Small
    // filter physically can't reach (echo taps out to 320ms vs its 50ms).
    const bool pass = (large_pa < small_pa);
    printf("  %s\n", pass ? "PASS -- Large outperforms Small on a long room tail (as expected)" : "CHECK");
    return pass;
}

// User report: an offline Reaper render of the same material sounds
// different from live playback -- more HF content coming through,
// throughout the *whole* rendered file, not just the start. Root-caused
// 2026-07-28 (see testOfflineVsLiveSpectralShiftAfterWarmup below) to the
// warm-up/priming mechanism that used to live in PluginProcessor.cpp, since
// removed. With that mechanism gone, offline and live run the identical
// code path unconditionally, so this should now show exact parity
// regardless of material -- kept as a regression test against either the
// warm-up mechanism, or any other realtime/non-realtime special-casing,
// ever coming back. Builds PA content that turns on and off in segments
// (simulating songs separated by quiet gaps, with continuous audience/crowd
// noise underneath throughout) and measures HF content in several windows
// spread across the *whole* file for both modes.
bool testOfflineVsLiveDynamicHfContent(int sampleRate) {
    printf("\n=== Offline vs live HF content on dynamic material test (%d Hz) ===\n", sampleRate);
    // Long enough (a few minutes, like a real recording, not a short clip)
    // to expose any *slow-accumulating* divergence between the live and
    // offline adaptation paths that a short test would miss entirely --
    // the offline path's 3s priming head-start could leave the filter on a
    // permanently different trajectory for a long file even if a 24s test
    // shows nothing.
    const double durationS = 180.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(41), ambientRng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.90f);
    reference = onePoleLowpass(reference, 0.90f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    // PA content on for 6s, off for 3s ("between songs"), repeating -- a
    // sharp on/off envelope rather than a gentle fade, since real PA
    // start/stop is abrupt and this is exactly the kind of transition a
    // stationary-noise test can't exercise.
    const double onS = 6.0, offS = 3.0, cycleS = onS + offS;
    for (int s = 0; s < n; ++s) {
        const double t = static_cast<double>(s) / sampleRate;
        const double phase = std::fmod(t, cycleS);
        if (phase >= onS) reference[static_cast<size_t>(s)] = 0.0f;
    }

    // Full-bandwidth, like testLimitHfGainToggle's ambient bed -- real crowd
    // noise/clapping/cymbals have genuine HF energy, so this gives the
    // suppressor's HF-band decisions something to actually act on. Present
    // continuously (crowd noise doesn't stop between songs).
    std::vector<float> ambient(static_cast<size_t>(n));
    for (auto& v : ambient) v = dist(ambientRng);
    float ambientPeak = 1e-9f;
    for (float v : ambient) ambientPeak = std::max(ambientPeak, std::abs(v));
    for (auto& v : ambient) v = 0.15f * v / ambientPeak;

    const int delaySamples = static_cast<int>(sampleRate * 5.0 / 1000.0);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = (s - delaySamples >= 0) ? 0.6f * reference[static_cast<size_t>(s - delaySamples)] : 0.0f;
        mic[static_cast<size_t>(s)] = leak + ambient[static_cast<size_t>(s)];
    }

    PAEchoCancellerAudioProcessor liveProc;
    if (!setMonoLayout(liveProc)) {
        printf("  FAILED to set mono layout (live)\n");
        return false;
    }
    auto liveOutput = runThroughProcessor(liveProc, sampleRate, reference, mic,
                                           -1, -1, nullptr, /*nonRealtime*/ false);

    PAEchoCancellerAudioProcessor offlineProc;
    if (!setMonoLayout(offlineProc)) {
        printf("  FAILED to set mono layout (offline)\n");
        return false;
    }
    auto offlineOutput = runThroughProcessor(offlineProc, sampleRate, reference, mic,
                                              -1, -1, nullptr, /*nonRealtime*/ true);

    auto measureHfRms = [&](const std::vector<float>& output, int startSample, int endSample) {
        HighPassFilterChain hfMeasure;
        hfMeasure.setCutoff(sampleRate, 6000.0f);
        double sumSq = 0.0;
        int count = 0;
        for (int i = startSample; i < endSample && i < static_cast<int>(output.size()); ++i) {
            float f = hfMeasure.processSample(output[static_cast<size_t>(i)]);
            sumSq += static_cast<double>(f) * f;
            ++count;
        }
        return count > 0 ? std::sqrt(sumSq / count) : 0.0;
    };

    // One window per 10s slice across the whole (now several-minute) file,
    // so both an immediate discrepancy and a slow-building one (growing
    // worse deeper into the file) would show up, not just a single late
    // snapshot.
    double worstOffsetDb = 0.0;
    std::vector<double> offsetsDb;
    for (double windowStartS = 3.0; windowStartS < durationS - 10.0; windowStartS += 10.0) {
        const int startSample = static_cast<int>(windowStartS * sampleRate);
        const int endSample = static_cast<int>((windowStartS + 10.0) * sampleRate);
        const double hfLive = measureHfRms(liveOutput, startSample, endSample);
        const double hfOffline = measureHfRms(offlineOutput, startSample, endSample);
        const double offsetDb = 20.0 * std::log10(std::max(hfOffline, 1e-12) / std::max(hfLive, 1e-12));
        printf("  t=%5.1f-%5.1fs  live=%.6f  offline=%.6f  (offline %+.1f dB)\n",
               windowStartS, windowStartS + 10.0, hfLive, hfOffline, offsetDb);
        offsetsDb.push_back(offsetDb);
        if (std::abs(offsetDb) > std::abs(worstOffsetDb)) worstOffsetDb = offsetDb;
    }

    // Explicit drift check: average of the first quarter of windows vs. the
    // last quarter -- a growing gap (rather than noise bouncing both ways)
    // would point at a slow-accumulating divergence, not a one-off.
    const size_t quarter = std::max(static_cast<size_t>(1), offsetsDb.size() / 4);
    double earlyAvg = 0.0, lateAvg = 0.0;
    for (size_t i = 0; i < quarter; ++i) earlyAvg += offsetsDb[i];
    for (size_t i = offsetsDb.size() - quarter; i < offsetsDb.size(); ++i) lateAvg += offsetsDb[i];
    earlyAvg /= static_cast<double>(quarter);
    lateAvg /= static_cast<double>(quarter);

    printf("  Worst live/offline HF divergence across the file: %.1f dB\n", worstOffsetDb);
    printf("  Early-file avg offset: %.1f dB, late-file avg offset: %.1f dB (drift: %.1f dB)\n",
           earlyAvg, lateAvg, lateAvg - earlyAvg);

    // Live and offline now run the exact same code path unconditionally, so
    // this should be near-exact (tiny residual is just measurement/floating-
    // point noise) -- a real gap here would mean some new realtime/non-
    // realtime special-casing crept back in.
    const bool pass = std::abs(worstOffsetDb) < 0.2 && std::abs(lateAvg - earlyAvg) < 0.2;
    printf("  %s\n", pass ? "PASS -- live and offline are behaviorally identical, no drift"
                           : "CHECK -- offline and live diverge on HF content, or drift apart over the file");
    return pass;
}

// Real user-provided offline/online renders of actual PA material (not
// synthetic) showed a large, broadband (not HF-specific) residual-leakage
// gap between the two -- offline much worse specifically in the first
// several seconds, converging (but not fully vanishing) over roughly the
// following 5s. testOfflineVsLiveDynamicHfContent above (180s) found
// nothing despite its length: its reference was stationary noise (same
// spectral statistics, just gated fully on/off), and a first synthetic
// attempt here with a one-time spectral-shape change at the old 3s priming
// boundary *also* found nothing -- if anything, priming made offline's
// first few seconds dramatically *better* than live (near-perfect
// cancellation immediately, since the filter had already converged during
// priming, vs. live's genuine cold-start ramp-up), the opposite of what the
// real renders showed. Both of those used an unrealistically easy echo path
// (a single dry delay tap) and unrealistically simple dynamics (one
// amplitude level, or one step change). Real PA-in-a-hall leakage is a
// multi-tap reverberant path (see testLargeRoomBenefit's roomTaps) with
// genuinely continuous musical dynamics (loud/quiet passages throughout,
// not one on/off step) -- retrying the same question under those more
// realistic conditions *did* reproduce it: a linear adaptive filter re-fed
// the *exact same* 3-second reverberant clip twice in a row (priming, then
// replay) measurably overfit to that specific segment, showing up as a
// worse-than-live residual for several seconds right after the priming
// window ended, before reconverging -- confirming the root cause. The
// priming/replay mechanism has since been removed entirely (2026-07-28,
// user's explicit choice: offline now converges naturally like live, same
// as it always did before the warm-up feature existed) -- this test is kept
// as-is (same realistic room+dynamics construction) to confirm the fix: it
// should now show no divergence at all, including in what used to be the
// 0-3s priming window and the following few seconds.
bool testOfflineVsLiveSpectralShiftAfterWarmup(int sampleRate) {
    printf("\n=== Offline vs live, realistic room + dynamics regression test (%d Hz) ===\n", sampleRate);
    const double durationS = 20.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(51), noiseFloorRng(53), envRng(54);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Broadband-ish reference (lightly filtered, single stage) so a short
    // filter can't "cheat" via short-lag self-correlation -- same rationale
    // as testLargeRoomBenefit.
    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.3f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    // Continuous musical dynamics: a slowly-wandering amplitude envelope
    // (smoothed random walk, clamped to stay audible) rather than a single
    // on/off step or a one-time spectral change -- real music's loudness
    // varies constantly, including through the first few seconds.
    std::vector<float> envelope(static_cast<size_t>(n));
    {
        float env = 0.6f;
        std::uniform_real_distribution<float> stepDist(-0.02f, 0.02f);
        for (int s = 0; s < n; ++s) {
            env += stepDist(envRng);
            env = std::max(0.15f, std::min(1.0f, env));
            envelope[static_cast<size_t>(s)] = env;
        }
    }
    for (int s = 0; s < n; ++s) reference[static_cast<size_t>(s)] *= envelope[static_cast<size_t>(s)];

    // Multi-tap reverberant room response, matching testLargeRoomBenefit's
    // model -- extended slightly further to actually stress an 800ms Tail
    // Length (the plugin's current default).
    const std::vector<std::pair<double, float>> roomTaps = {
        { 5.0, 0.5f }, { 80.0, 0.3f }, { 180.0, 0.2f }, { 320.0, 0.15f }, { 500.0, 0.1f }
    };

    // Leak only, no ambient/voice -- isolates the leak-cancellation residual
    // cleanly rather than mixing in near-end content.
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noiseFloorDist(-0.0005f, 0.0005f);
    for (int s = 0; s < n; ++s) {
        float leak = 0.0f;
        for (const auto& tap : roomTaps) {
            const int delaySamples = static_cast<int>(sampleRate * tap.first / 1000.0);
            if (s - delaySamples >= 0) leak += tap.second * reference[static_cast<size_t>(s - delaySamples)];
        }
        mic[static_cast<size_t>(s)] = leak + noiseFloorDist(noiseFloorRng);
    }

    PAEchoCancellerAudioProcessor liveProc;
    if (!setMonoLayout(liveProc)) {
        printf("  FAILED to set mono layout (live)\n");
        return false;
    }
    auto liveOutput = runThroughProcessor(liveProc, sampleRate, reference, mic,
                                           -1, -1, nullptr, /*nonRealtime*/ false);

    PAEchoCancellerAudioProcessor offlineProc;
    if (!setMonoLayout(offlineProc)) {
        printf("  FAILED to set mono layout (offline)\n");
        return false;
    }
    auto offlineOutput = runThroughProcessor(offlineProc, sampleRate, reference, mic,
                                              -1, -1, nullptr, /*nonRealtime*/ true);

    auto rms = [](const std::vector<float>& output, int startSample, int endSample) {
        double sumSq = 0.0;
        int count = 0;
        for (int i = startSample; i < endSample && i < static_cast<int>(output.size()); ++i) {
            sumSq += static_cast<double>(output[static_cast<size_t>(i)]) * output[static_cast<size_t>(i)];
            ++count;
        }
        return count > 0 ? std::sqrt(sumSq / count) : 0.0;
    };

    double worstOffsetDb = 0.0;
    for (double windowStartS = 0.0; windowStartS < durationS - 1.0; windowStartS += 1.0) {
        const int startSample = static_cast<int>(windowStartS * sampleRate);
        const int endSample = static_cast<int>((windowStartS + 1.0) * sampleRate);
        const double rmsLive = rms(liveOutput, startSample, endSample);
        const double rmsOffline = rms(offlineOutput, startSample, endSample);
        const double offsetDb = 20.0 * std::log10(std::max(rmsOffline, 1e-12) / std::max(rmsLive, 1e-12));
        printf("  t=%4.1f-%4.1fs  live=%.6f  offline=%.6f  (offline %+.1f dB)\n",
               windowStartS, windowStartS + 1.0, rmsLive, rmsOffline, offsetDb);
        if (std::abs(offsetDb) > std::abs(worstOffsetDb)) worstOffsetDb = offsetDb;
    }

    printf("  Worst live/offline residual gap (either direction): %+.1f dB\n", worstOffsetDb);
    // With the warm-up/priming mechanism removed, live and offline run the
    // identical code path unconditionally -- this should now be near-zero
    // everywhere, including in what used to be the 0-3s priming window
    // (where this same test previously measured offline as dramatically
    // *better*, and the following few seconds (where it measured offline as
    // up to +2.1dB *worse* -- the confirmed root cause of the user-reported
    // divergence). A real gap here would mean the regression (or something
    // like it) came back.
    const bool pass = std::abs(worstOffsetDb) < 0.2;
    printf("  %s\n", pass ? "PASS -- offline and live are identical throughout, including around the old priming boundary"
                           : "CHECK -- offline and live diverge -- the warm-up regression (or similar) may have returned");
    return pass;
}

// User's next hypothesis: does the offline/warm-up render path actually
// pick up the *current* (non-default) parameter values, or could it end up
// using constructor defaults instead? None of the tests above actually
// checked this -- both offline comparisons so far left every parameter at
// its default for both runs. This sets two deliberately different,
// non-default settings (Amount 0% -- the filter alone -- vs. Amount 100%
// with Max Reduction -24 dB) *before* prepareToPlay ever runs, then renders
// the same material offline both ways -- if the settings were being
// ignored, both renders would come out at the defaults and match; if
// they're correctly applied, the strongest setting shows clearly less HF.
bool testOfflineRenderUsesCurrentSettingsNotDefaults(int sampleRate) {
    printf("\n=== Offline render respects current (non-default) settings test (%d Hz) ===\n", sampleRate);
    const double durationS = 24.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(51), ambientRng(52);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.90f);
    reference = onePoleLowpass(reference, 0.90f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    const double onS = 6.0, offS = 3.0, cycleS = onS + offS;
    for (int s = 0; s < n; ++s) {
        const double t = static_cast<double>(s) / sampleRate;
        if (std::fmod(t, cycleS) >= onS) reference[static_cast<size_t>(s)] = 0.0f;
    }

    std::vector<float> ambient(static_cast<size_t>(n));
    for (auto& v : ambient) v = dist(ambientRng);
    float ambientPeak = 1e-9f;
    for (float v : ambient) ambientPeak = std::max(ambientPeak, std::abs(v));
    for (auto& v : ambient) v = 0.15f * v / ambientPeak;

    const int delaySamples = static_cast<int>(sampleRate * 5.0 / 1000.0);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = (s - delaySamples >= 0) ? 0.6f * reference[static_cast<size_t>(s - delaySamples)] : 0.0f;
        mic[static_cast<size_t>(s)] = leak + ambient[static_cast<size_t>(s)];
    }

    auto runOffline = [&](bool strongest) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        if (strongest) {
            proc.getAmountParameter()->setValueNotifyingHost(1.0f);       // 100%
            proc.getMaxReductionParameter()->setValueNotifyingHost(0.0f); // -24 dB
        } else {
            proc.getAmountParameter()->setValueNotifyingHost(0.0f);       // 0%: the filter alone
        }
        return runThroughProcessor(proc, sampleRate, reference, mic,
                                    -1, -1, nullptr, /*nonRealtime*/ true);
    };

    auto filterOnlyOfflineOutput = runOffline(false);
    auto strongestOfflineOutput = runOffline(true);

    auto measureHfRms = [&](const std::vector<float>& output) {
        HighPassFilterChain hfMeasure;
        hfMeasure.setCutoff(sampleRate, 6000.0f);
        double sumSq = 0.0;
        int count = 0;
        const int start = static_cast<int>(4.0 * sampleRate); // past initial filter settling
        for (int i = start; i < static_cast<int>(output.size()); ++i) {
            float f = hfMeasure.processSample(output[static_cast<size_t>(i)]);
            sumSq += static_cast<double>(f) * f;
            ++count;
        }
        return std::sqrt(sumSq / count);
    };

    const double hfFilterOnly = measureHfRms(filterOnlyOfflineOutput);
    const double hfStrongest = measureHfRms(strongestOfflineOutput);
    const double reducedByDb = 20.0 * std::log10(hfFilterOnly / std::max(hfStrongest, 1e-12));

    printf("  Offline HF (>6kHz) content: Amount 0%%=%.6f  Amount 100%%/-24dB=%.6f  (%.1f dB less)\n",
           hfFilterOnly, hfStrongest, reducedByDb);

    const bool pass = reducedByDb > 3.0;
    printf("  %s\n", pass ? "PASS -- offline render correctly applies non-default settings, not constructor defaults"
                           : "CHECK -- offline render doesn't seem to be picking up non-default settings");
    return pass;
}

// Confirms the Dry/Wet blend's dry path is correctly delay-matched against
// the wet (cancelled) path. This is the critical risk in any dry/wet design: if
// the two paths are misaligned by even a few samples, blending them would
// comb-filter the content they share (mostly voice) instead of cleanly
// mixing. Checked by setting the mix to 0% (fully dry) and confirming the
// output matches an independently-computed delayed-HPF'd copy of the mic
// signal almost exactly -- any misalignment would show up as a large
// difference here, not a small one.
bool testDryWetAlignment(int sampleRate) {
    printf("\n=== Dry/Wet alignment test (%d Hz) ===\n", sampleRate);
    const double durationS = 6.0;
    auto signals = makeSignals(sampleRate, durationS);

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    proc.getDryWetMixParameter()->setValueNotifyingHost(0.0f); // 0% wet = fully dry

    auto output = runThroughProcessor(proc, sampleRate, signals.reference, signals.mic);
    const int nominalLatency = proc.getLatencySamples();

    // Independently compute what the dry path should contain: the mic
    // signal through the same HPF the processor applies. Read the actual
    // default from the parameter rather than hardcoding it, so this stays
    // correct if the default HPF frequency ever changes again.
    HighPassFilterChain expectedChain;
    expectedChain.setCutoff(sampleRate, proc.getHpfFrequencyParameter()->get());
    std::vector<float> expectedDry(signals.mic.size());
    for (size_t i = 0; i < signals.mic.size(); ++i)
        expectedDry[i] = expectedChain.processSample(signals.mic[i]);

    const int n = static_cast<int>(signals.mic.size());

    // getLatencySamples() is an accurate estimate (it now includes AEC3's
    // own precisely-measured internal processing delay -- see
    // aec3InternalDelaySamples), but not necessarily exact to the sample:
    // the FIFO's frame-accumulation wait still has some phase-dependent
    // slop against this test's irregular host block-size pattern. Rather
    // than assume the nominal figure is exact, search a window around it
    // (coarse then fine) for the delay that actually minimizes the
    // difference -- this test cares whether the HPF shape is right, not
    // whether getLatencySamples() is sample-perfect (testDryWetCombFiltering
    // already covers exact alignment directly, against the wet path).
    // makeSignals' default taps heavily lowpass the reference (2-stage
    // one-pole, alpha=0.9) before leaking it into mic -- almost all of that
    // energy sits below the HPF's ~150Hz cutoff, so the "PA-leak-only"
    // portion of the file (before voice.first joins at durationS*2/6) is
    // very nearly silent *after* the HPF removes it. Comparing two
    // near-silent signals via a relative-dB ratio is numerically
    // ill-conditioned regardless of alignment (tiny denominators amplify
    // any residual noise into large, meaningless dB swings) -- confirmed
    // directly: an isolated-click delay measurement through this exact
    // adversarial block pattern shows a stable ~850-875 sample delay
    // throughout the whole file (matching nominal within the same slop
    // already documented above), yet this correlation search reported wildly
    // bad matches specifically in the pre-voice region at 44.1kHz. Starting
    // the comparison once voice is present avoids that ill-conditioned
    // region without touching what's actually being tested (the HPF shape).
    // Skip a bit further than voice's own onset (durationS*2/6): voice
    // bursts on/off (0.25s on, 0.15s off), so the first second or so after
    // onset still mixes in enough low-energy "off" gaps that the average
    // stays numerically marginal at 44.1kHz specifically (confirmed by
    // scanning short sub-windows through this region directly) -- by
    // durationS*3/6 enough on/off cycles have accumulated for a stable
    // average.
    const int voiceStartSample = static_cast<int>(durationS * (3.0 / 6.0) * sampleRate);
    auto relativeDiffDbAtLatency = [&](int latency) {
        const int compareStart = std::max({ std::max(latency, 0) + sampleRate / 2, voiceStartSample });
        const int compareEnd = n - sampleRate / 2;
        double sumSqDiff = 0.0, sumSqSignal = 0.0;
        for (int i = compareStart; i < compareEnd; ++i) {
            const int srcIdx = i - latency;
            if (srcIdx < 0 || srcIdx >= static_cast<int>(expectedDry.size())) continue;
            const double diff = static_cast<double>(output[static_cast<size_t>(i)])
                                 - expectedDry[static_cast<size_t>(srcIdx)];
            sumSqDiff += diff * diff;
            sumSqSignal += static_cast<double>(expectedDry[static_cast<size_t>(srcIdx)])
                            * expectedDry[static_cast<size_t>(srcIdx)];
        }
        const double rmsDiff = std::sqrt(sumSqDiff / (compareEnd - compareStart));
        const double rmsSignal = std::sqrt(sumSqSignal / (compareEnd - compareStart));
        return 20.0 * std::log10(rmsDiff / rmsSignal);
    };

    int bestLatency = nominalLatency;
    double bestDb = relativeDiffDbAtLatency(nominalLatency);
    constexpr int coarseSearchMargin = 1000, coarseStep = 8;
    for (int offset = -coarseSearchMargin; offset <= coarseSearchMargin; offset += coarseStep) {
        const double db = relativeDiffDbAtLatency(nominalLatency + offset);
        if (db < bestDb) { bestDb = db; bestLatency = nominalLatency + offset; }
    }
    // Fixed base for the fine pass -- iterating with "bestLatency + offset"
    // while also updating bestLatency mid-loop would shift the window being
    // scanned out from under itself instead of exploring a fixed range
    // around the coarse result.
    const int coarseBestLatency = bestLatency;
    for (int offset = -coarseStep; offset <= coarseStep; ++offset) {
        const double db = relativeDiffDbAtLatency(coarseBestLatency + offset);
        if (db < bestDb) { bestDb = db; bestLatency = coarseBestLatency + offset; }
    }

    printf("  Nominal latency=%d, best-matching latency=%d (%+d samples), difference at best: %.1f dB (expect < -20dB)\n",
           nominalLatency, bestLatency, bestLatency - nominalLatency, bestDb);

    // Confirms the HPF shape is correct (the actual thing this test is
    // for) and that the discovered delay isn't wildly off from the
    // nominal estimate (a sanity check against a genuine alignment bug,
    // not just phase-dependent slop).
    //
    // -20dB rather than -40dB: this test's block-size *cycling* pattern
    // (runThroughProcessor's {512,37,129,1,4096,256,7}, changing every
    // call, deliberately adversarial -- no real host does this) means the
    // FIFO's phase relationship never settles into the single stable delay
    // a constant or slowly-varying block size gets. Confirmed directly: at
    // a *fixed* block size (512 or 128 samples), this same comparison
    // measures effectively perfect alignment (no measurable difference at
    // all) -- testDryWetCombFiltering below covers exactly that realistic
    // case. -20dB here still fails hard on a genuine alignment regression
    // (which showed as -4.7dB before this fix), just not as strict as a
    // fixed-block-size measurement could be.
    const bool pass = bestDb < -20.0 && std::abs(bestLatency - nominalLatency) < sampleRate / 20; // < 50ms
    printf("  %s\n", pass ? "PASS -- dry path's HPF shape is correct and close to the expected latency"
                           : "CHECK -- misalignment would show up as a much larger difference here");
    return pass;
}


// User report: audible comb filtering with Dry/Wet Mix at 50%. Suspected
// root cause: dryDelayFifos is pre-filled with a *fixed* silence amount
// (reportedLatencySamples, i.e. one AEC3 frame) meant to approximate the
// wet path's buffering delay -- but the wet path's *actual* delay (samples
// in micInFifos waiting for a full 480-sample frame, plus samples in
// micOutFifos waiting to be drained at the host's block rate) depends on
// the phase between the host's block size and AEC3's fixed frame size, and
// was already measured (see the Suppression meter transient-alignment
// work) to exceed the nominal one-frame estimate by a wide margin at some
// block sizes. testDryWetAlignment above can't catch this: it only checks
// the dry path against an externally-computed delay using the *same*
// reportedLatencySamples value being tested, entirely self-referential.
// This instead measures where an isolated click actually lands in a
// fully-wet run vs. a fully-dry run of the exact same material, with no
// reference signal at all (so AEC3 stays transparent -- gain near 1 -- and
// the "wet" path is essentially just HPF + FIFO latency, isolating the
// timing question from anything AEC3-decision-specific), using a small,
// non-frame-aligned host block size like a real low-latency live rig would
// use, rather than the mixed block-size pattern testDryWetAlignment uses.
bool testDryWetCombFiltering(int sampleRate) {
    printf("\n=== Dry/Wet comb-filtering test (%d Hz) ===\n", sampleRate);
    const double durationS = 3.0;
    const int n = static_cast<int>(sampleRate * durationS);
    const int clickSample = n / 2;
    const int clickLenSamples = static_cast<int>(sampleRate * 0.001); // ~1ms sharp click

    std::mt19937 clickRng(61);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < clickLenSamples; ++i) {
        const int s = clickSample + i;
        if (s >= n) break;
        const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(clickLenSamples);
        mic[static_cast<size_t>(s)] = 0.8f * envelope * dist(clickRng);
    }
    const std::vector<float> reference(static_cast<size_t>(n), 0.0f); // no PA content at all

    auto runWithMix = [&](float wetMixPercent, int blockSize) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        proc.getDryWetMixParameter()->setValueNotifyingHost(wetMixPercent / 100.0f);
        proc.prepareToPlay(sampleRate, blockSize);

        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        juce::MidiBuffer midi;

        std::vector<float> output(static_cast<size_t>(n), 0.0f);
        int pos = 0;
        while (pos + blockSize <= n) {
            buffer.setSize(totalChannels, blockSize, false, false, true);
            buffer.clear();
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
                mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
            for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
                refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);
            proc.processBlock(buffer, midi);
            auto mainOut = proc.getBusBuffer(buffer, false, 0);
            for (int s = 0; s < blockSize; ++s)
                output[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);
            pos += blockSize;
        }
        proc.releaseResources();
        return output;
    };

    auto findPeakPosition = [&](const std::vector<float>& output, int searchStart, int searchEnd) {
        int peakPos = searchStart;
        float peakVal = 0.0f;
        for (int i = searchStart; i < searchEnd && i < static_cast<int>(output.size()); ++i) {
            if (std::abs(output[static_cast<size_t>(i)]) > peakVal) {
                peakVal = std::abs(output[static_cast<size_t>(i)]);
                peakPos = i;
            }
        }
        return peakPos;
    };

    const int blockSize = 128; // small, non-frame-aligned -- like a real low-latency live rig
    auto wetOnlyOutput = runWithMix(100.0f, blockSize);
    auto dryOnlyOutput = runWithMix(0.0f, blockSize);

    const int searchStart = clickSample - 100;
    const int searchEnd = clickSample + 4000; // generous window past even a very wrong latency estimate
    const int wetPeakPos = findPeakPosition(wetOnlyOutput, searchStart, searchEnd);
    const int dryPeakPos = findPeakPosition(dryOnlyOutput, searchStart, searchEnd);
    const int mismatchSamples = wetPeakPos - dryPeakPos;

    printf("  Click peak position (block=%d): wet-only=%d  dry-only=%d  (mismatch: %d samples, %.2fms)\n",
           blockSize, wetPeakPos, dryPeakPos, mismatchSamples, 1000.0 * mismatchSamples / sampleRate);

    // A residual few samples (well under 1ms) is expected: it's peak-
    // detection granularity, not the ~400+ sample structural misalignment
    // this test was built to catch. 20 samples (~0.4ms at 48kHz) is a
    // generous margin that would still fail hard if the real bug came back.
    const bool pass = std::abs(mismatchSamples) <= 20;
    printf("  %s\n", pass ? "PASS -- wet and dry paths are sample-accurately aligned"
                          : "CHECK -- wet/dry misalignment found -- this is exactly what causes comb filtering");
    return pass;
}

// Confirms getLatencySamples() reports the TRUE, accurate input-to-output
// delay at all three required sample rates -- not a re-derived estimate
// from a separate calibration harness (which would risk a methodology
// mismatch, as happened once already with mismatched click shapes/tail-
// length configs between the plugin's own calibration and a diagnostic
// harness), but a direct end-to-end measurement through the real
// PAEchoCancellerAudioProcessor itself: an isolated click through the mic
// input, silent reference (so AEC3 stays transparent, gain ~1, isolating
// pure buffering/processing delay from any echo-removal decision), 100%
// wet, small non-frame-aligned block size like a real low-latency rig.
// Wherever the click's peak actually lands in the output is the true
// delay -- compared directly against what getLatencySamples() reported.
bool testGetLatencySamplesAccuracyAllRates() {
    bool allPass = true;
    for (int sampleRate : { 44100, 48000, 96000 }) {
        printf("\n=== getLatencySamples() accuracy test (%d Hz) ===\n", sampleRate);

        // The click must land well past AEC3's own ~2.5s initial-state
        // bootstrap (it behaves differently -- and reports a different
        // internal delay -- while still converging), matching how
        // measureAec3InternalDelaySamples primes 300 frames (3s) of silence
        // before its own calibration click. An earlier version of this test
        // placed the click at just 1.0s in (durationS/2 with durationS=2.0),
        // still inside that bootstrap window, which showed up as a false
        // ~0.55ms divergence at 48/96kHz (but not 44.1kHz, apparently not
        // enough to move the needle there) -- not a real getLatencySamples()
        // bug, just an apples-to-oranges comparison against a calibration
        // that measures its own delay only after settling.
        const double durationS = 5.0;
        const int n = static_cast<int>(sampleRate * durationS);
        const int clickSample = static_cast<int>(sampleRate * 3.5);
        const int clickLenSamples = std::max(1, static_cast<int>(sampleRate * 0.001)); // ~1ms sharp click

        // Seed 81, matching measureAec3InternalDelaySamples's own calibration
        // click exactly (same formula too): a decaying-*random*-noise click's
        // true peak sample lands at a seed-dependent position somewhere
        // within its envelope, not deterministically at sample 0, so
        // comparing against a *differently*-seeded click here would measure
        // that peak-position difference as if it were a getLatencySamples()
        // error. Matching the seed cancels that ambiguity out, the same way
        // testDryWetCombFiltering's wet-vs-dry comparison stays exact by
        // reusing one click for both of its measurements.
        std::mt19937 clickRng(81);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::vector<float> mic(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < clickLenSamples; ++i) {
            const int s = clickSample + i;
            if (s >= n) break;
            const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(clickLenSamples);
            mic[static_cast<size_t>(s)] = 0.8f * envelope * dist(clickRng);
        }
        const std::vector<float> reference(static_cast<size_t>(n), 0.0f); // silent: AEC3 stays transparent

        PAEchoCancellerAudioProcessor proc;
        if (!setMonoLayout(proc)) {
            printf("  FAILED to set mono layout\n");
            allPass = false;
            continue;
        }
        proc.getDryWetMixParameter()->setValueNotifyingHost(1.0f); // 100% wet

        const int blockSize = 128; // small, non-frame-aligned -- like a real low-latency live rig
        proc.prepareToPlay(sampleRate, blockSize);
        const int reportedLatency = proc.getLatencySamples();

        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        juce::MidiBuffer midi;

        std::vector<float> output(static_cast<size_t>(n), 0.0f);
        int pos = 0;
        while (pos + blockSize <= n) {
            buffer.setSize(totalChannels, blockSize, false, false, true);
            buffer.clear();
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
                mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
            for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
                refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);
            proc.processBlock(buffer, midi);
            auto mainOut = proc.getBusBuffer(buffer, false, 0);
            for (int s = 0; s < blockSize; ++s)
                output[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);
            pos += blockSize;
        }
        proc.releaseResources();

        // Ground truth is where an *independently* HPF-filtered copy of this
        // exact click shows its own peak, not the raw click's own position:
        // the plugin's input HPF (a sharp 4th-order 150Hz Butterworth)
        // reshapes a short decaying-noise click enough that the single
        // loudest sample can land tens of samples away from where it sat in
        // the unfiltered click, entirely separate from any real processing
        // delay. Comparing raw-click-position to HPF'd-and-AEC3'd output
        // measures that reshaping as if it were a getLatencySamples() error
        // (confirmed: real, and reproducible, at 48/96kHz specifically).
        // Filtering the same click the same way the plugin does (matching
        // its default cutoff) and finding *that* copy's own peak cancels the
        // reshaping out, the same way testDryWetAlignment's expectedChain
        // does for the dry-path HPF comparison.
        HighPassFilterChain groundTruthHpf;
        groundTruthHpf.setCutoff(sampleRate, proc.getHpfFrequencyParameter()->get());
        std::vector<float> expectedClick(mic.size());
        for (size_t i = 0; i < mic.size(); ++i) expectedClick[i] = groundTruthHpf.processSample(mic[i]);
        int expectedPeakPos = clickSample;
        float expectedPeakVal = 0.0f;
        for (int i = clickSample - 100; i < clickSample + 4000 && i < n; ++i) {
            if (std::abs(expectedClick[static_cast<size_t>(i)]) > expectedPeakVal) {
                expectedPeakVal = std::abs(expectedClick[static_cast<size_t>(i)]);
                expectedPeakPos = i;
            }
        }

        int peakPos = clickSample;
        float peakVal = 0.0f;
        const int searchStart = clickSample - 100;
        const int searchEnd = clickSample + 4000; // generous window past even a very wrong latency estimate
        for (int i = searchStart; i < searchEnd && i < n; ++i) {
            if (std::abs(output[static_cast<size_t>(i)]) > peakVal) {
                peakVal = std::abs(output[static_cast<size_t>(i)]);
                peakPos = i;
            }
        }
        const int measuredDelay = peakPos - expectedPeakPos;

        printf("  Reported: %d samples (%.2fms)  Measured (actual click shift): %d samples (%.2fms)\n",
               reportedLatency, 1000.0 * reportedLatency / sampleRate,
               measuredDelay, 1000.0 * measuredDelay / sampleRate);

        // getLatencySamples() is a worst-case bound on the FIFO's frame-
        // accumulation wait, not an exact figure -- the true instantaneous
        // delay depends on the phase between the host's block size and
        // frameSize, and can legitimately land anywhere from that worst
        // case down to noticeably less for a favorable phase. Confirmed
        // directly: re-running this same measurement at several different
        // fixed block sizes (128/200/333/512/63) at 48kHz gave 872/864/901/
        // 872/901 -- varying with block size, as expected for a phase
        // effect, but never once exceeding the reported 910. So the
        // contract to check is one-sided: reported must never
        // UNDER-estimate the real delay (that's the actual safety property
        // a host's delay compensation depends on), and shouldn't over-
        // estimate by more than about one frame's worth of legitimate
        // phase-dependent slack.
        const int frameSizeForCheck = static_cast<int>(std::round(sampleRate / 100.0));
        const bool pass = measuredDelay <= reportedLatency && reportedLatency - measuredDelay <= frameSizeForCheck;
        printf("  %s\n", pass ? "PASS -- getLatencySamples() is a safe, accurate upper bound on the real delay"
                              : "CHECK -- getLatencySamples() under-reports, or over-reports by more than a frame");
        allPass = allPass && pass;
    }
    return allPass;
}

// Confirms the HPF is a genuine 4th-order (24dB/octave) Butterworth
// response -- not, say, a single 2nd-order (12dB/octave) section or two
// identical (non-Butterworth-aligned) stages -- by checking measured
// attenuation against the textbook Butterworth formula
// |H|^2 = 1 / (1 + (fc/f)^(2n)) at a few key points relative to cutoff.
// PA Reference Trim applies a plain linear gain to the reference before
// AEC3 ever sees it (see the comment on referenceGainParam in
// PluginProcessor.h for why this is our own code rather than AEC3's
// render_levels.render_power_gain_db). This does NOT verify it fixes
// over-suppression of correlated content -- an earlier attempt at that
// test showed no measurable effect: AEC3's absolute-level gates
// (echo_audibility, render_levels) sit around -70dBFS-equivalent, so a
// reference that's merely too loud (not clipping, not near-silent) never
// crosses them, and the masking-threshold ratios that actually decide
// suppression strength are computed from quantities that rescale together
// with the reference, cancelling out a pure gain change. So this control
// is a straightforward gain-staging utility (for a reference that's
// clipping or too quiet to give AEC3 a usable signal), not an artifact
// fix -- this test just confirms the gain is applied correctly.
bool testReferenceGainTrim() {
    printf("\n=== PA Reference Trim test ===\n");
    const int sampleRate = 48000;
    const int blockSize = 512;

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    proc.prepareToPlay(sampleRate, blockSize);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::MidiBuffer midi;

    auto measurePostFilterPeak = [&](float trimDb) {
        proc.getReferenceGainParameter()->setValueNotifyingHost(
            proc.getReferenceGainParameter()->convertTo0to1(trimDb));

        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        buffer.clear();
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            for (int s = 0; s < blockSize; ++s)
                refIn.setSample(ch, s, dist(rng));

        proc.processBlock(buffer, midi);
        return proc.getSidechainPeakLevelPost();
    };

    const float peak0dB = measurePostFilterPeak(0.0f);
    const float peakMinus12dB = measurePostFilterPeak(-12.0f);
    proc.releaseResources();

    const double ratioDb = 20.0 * std::log10(static_cast<double>(peak0dB) / static_cast<double>(peakMinus12dB));
    printf("  Post-filter peak: 0dB trim=%.4f  -12dB trim=%.4f  (ratio %.1f dB, expect ~12.0)\n",
           peak0dB, peakMinus12dB, ratioDb);

    const bool pass = std::abs(ratioDb - 12.0) < 0.5;
    printf("  %s\n", pass ? "PASS -- trim applies as the expected linear gain" : "CHECK");
    return pass;
}

// The PA-ref meter has a target zone, so both of its readings (before and
// after the HPF) must include PA Reference Trim -- otherwise turning the trim
// would not move the bar toward the zone. A 0.1-amplitude 1kHz reference
// (-20dBFS peak) at -12/0/+12dB trim must read ~0.025/0.1/0.4.
bool testReferenceMeterIncludesTrim() {
    printf("\n=== PA-ref meter includes Reference Trim test ===\n");
    const int sampleRate = 48000;
    const int blockSize = 4800;
    bool ok = true;
    for (float trimDb : { -12.0f, 0.0f, 12.0f }) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        auto* trim = proc.getReferenceGainParameter();
        trim->setValueNotifyingHost(trim->convertTo0to1(trimDb));
        proc.prepareToPlay(sampleRate, blockSize);
        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        juce::MidiBuffer midi;
        for (int block = 0; block < 3; ++block) { // let the HPF settle
            buffer.clear();
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int s = 0; s < blockSize; ++s)
                for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
                    refIn.setSample(ch, s, 0.1f * std::sin(2.0f * juce::MathConstants<float>::pi * 1000.0f
                                                           * static_cast<float>(block * blockSize + s) / sampleRate));
            proc.processBlock(buffer, midi);
        }
        const float expected = 0.1f * juce::Decibels::decibelsToGain(trimDb);
        const float pre = proc.getSidechainPeakLevelPre(), post = proc.getSidechainPeakLevelPost();
        const bool preOk = std::abs(pre - expected) < 0.03f * expected;
        const bool postOk = std::abs(post - expected) < 0.05f * expected;
        printf("  trim %+5.1f dB: expected peak %.4f  pre-HPF %.4f (%s)  post-HPF %.4f (%s)\n", trimDb, expected, pre,
               preOk ? "ok" : "WRONG", post, postOk ? "ok" : "WRONG");
        ok = ok && preOk && postOk;
        proc.releaseResources();
    }
    printf("  %s\n", ok ? "PASS -- both PA-ref meter readings follow the trim" : "CHECK -- the PA-ref meter ignores the trim");
    return ok;
}

// Confirms getStateInformation/setStateInformation actually round-trip
// every parameter. These were empty no-ops for a while -- harmless in this
// standalone harness (which never calls them), but in a real host it means
// every parameter silently resets to its constructor default on every
// project reload or plugin reinsert, discarding whatever the user tuned.
// JUCE documents prepareToPlay's samplesPerBlock as the *expected* block
// size, not a hard ceiling, and hosts genuinely exceed it -- most relevant
// here, when switching between live playback and an offline render, which is
// exactly this plugin's workflow. Every scratch buffer and FIFO is sized
// against that promised figure, so an oversized block used to index straight
// past the end of them: a confirmed segfault (SIGSEGV), reproduced with
// prepared=256 / actual=4096, not a theoretical concern. processBlock now
// splits oversized blocks into chunks of at most preparedBlockSize. Checks
// both that it survives AND that the audio is actually right afterwards --
// silently emitting zeros would "pass" a crash-only test.
bool testOversizedHostBlock(int sampleRate) {
    printf("\n=== Oversized host block test (%d Hz) ===\n", sampleRate);
    const int preparedBlock = 256;
    const int oversizedBlock = 4096; // 16x what we told the host we expected

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    proc.getDryWetMixParameter()->setValueNotifyingHost(0.0f); // fully dry: output should track input
    proc.prepareToPlay(sampleRate, preparedBlock);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, oversizedBlock);
    juce::MidiBuffer midi;

    std::mt19937 rng(91);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);

    double worstRms = 1e9;
    const int numBlocks = 24;
    for (int b = 0; b < numBlocks; ++b) {
        buffer.setSize(totalChannels, oversizedBlock, false, false, true);
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int s = 0; s < oversizedBlock; ++s) {
            const float v = dist(rng);
            for (int ch = 0; ch < mainIn.getNumChannels(); ++ch) mainIn.setSample(ch, s, v);
            for (int ch = 0; ch < refIn.getNumChannels(); ++ch) refIn.setSample(ch, s, 0.0f);
        }
        proc.processBlock(buffer, midi);
        auto mainOut = proc.getBusBuffer(buffer, false, 0);
        double sumSq = 0.0;
        for (int s = 0; s < oversizedBlock; ++s) {
            const double v = mainOut.getSample(0, s);
            sumSq += v * v;
        }
        // Skip the first few blocks: the pipeline is still filling, so early
        // output legitimately contains the startup zero-padding.
        if (b >= 4) worstRms = std::min(worstRms, std::sqrt(sumSq / oversizedBlock));
    }
    proc.releaseResources();

    // Input is uniform(-0.5,0.5) -> RMS ~0.289; the HPF removes only content
    // below 150Hz, so a correct dry path should stay in the same ballpark.
    // A crash never gets here; silent/garbage output fails on the level.
    const double expectedRms = 0.289;
    printf("  prepared=%d, actual block=%d -> worst steady-state output RMS=%.4f (input ~%.3f)\n",
           preparedBlock, oversizedBlock, worstRms, expectedRms);
    const bool pass = worstRms > 0.5 * expectedRms && worstRms < 1.5 * expectedRms;
    printf("  %s\n", pass ? "PASS -- survives an oversized block and still produces correct audio"
                          : "CHECK -- oversized block produced wrong/no audio (or the scratch buffers overran)");
    return pass;
}

// An IIR's feedback state latches non-finite values permanently: one NaN
// entering the delay line keeps recirculating, so every later output is NaN
// too, and AEC3's adaptive filter downstream latches it as well. Measured
// before the guard in HighPassFilterChain existed: a 5ms NaN burst left the
// output 100% non-finite for the whole rest of the run, with no recovery.
// For live use that means one glitched frame from anything upstream silently
// kills an audience mic for the rest of the show. Confirms the plugin both
// survives the burst and fully recovers afterwards.
bool testNonFiniteInputRecovery(int sampleRate) {
    printf("\n=== Non-finite (NaN/Inf) input recovery test (%d Hz) ===\n", sampleRate);
    const double durationS = 12.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(21);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.3f);
    float pk = 1e-9f;
    for (float v : reference) pk = std::max(pk, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / pk;

    const int leakDelay = static_cast<int>(sampleRate * 0.02);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s)
        mic[static_cast<size_t>(s)] = (s - leakDelay >= 0) ? 0.6f * reference[static_cast<size_t>(s - leakDelay)] : 0.0f;

    // A burst of both NaN and Inf at t=6s -- 5ms, i.e. well under one AEC3 frame.
    const int burstStart = static_cast<int>(sampleRate * 6.0);
    const int burstLen = static_cast<int>(sampleRate * 0.005);
    for (int i = 0; i < burstLen && burstStart + i < n; ++i)
        mic[static_cast<size_t>(burstStart + i)] =
            (i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    const int blockSize = 256;
    proc.prepareToPlay(sampleRate, blockSize);
    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;

    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    int pos = 0;
    while (pos + blockSize <= n) {
        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch) mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch) refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);
        proc.processBlock(buffer, midi);
        auto mainOut = proc.getBusBuffer(buffer, false, 0);
        for (int s = 0; s < blockSize; ++s) out[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);
        pos += blockSize;
    }
    proc.releaseResources();

    auto countNonFinite = [&](double a, double b) {
        int bad = 0;
        for (int i = static_cast<int>(a * sampleRate); i < static_cast<int>(b * sampleRate) && i < n; ++i)
            if (!std::isfinite(out[static_cast<size_t>(i)])) ++bad;
        return bad;
    };
    auto rmsOf = [&](double a, double b) {
        double sumSq = 0.0; int cnt = 0;
        for (int i = static_cast<int>(a * sampleRate); i < static_cast<int>(b * sampleRate) && i < n; ++i) {
            const float v = out[static_cast<size_t>(i)];
            if (std::isfinite(v)) { sumSq += static_cast<double>(v) * v; ++cnt; }
        }
        return cnt > 0 ? std::sqrt(sumSq / cnt) : 0.0;
    };

    const int badBefore = countNonFinite(4.0, 6.0);
    const int badAfter = countNonFinite(7.0, 12.0);
    const double rmsBefore = rmsOf(4.0, 6.0);
    const double rmsAfter = rmsOf(9.0, 12.0);

    printf("  before burst: non-finite=%d, residual RMS=%.6f\n", badBefore, rmsBefore);
    printf("  after burst:  non-finite=%d, residual RMS=%.6f\n", badAfter, rmsAfter);

    // Nothing non-finite may ever reach the output, and cancellation must
    // still be working afterwards (a filter left permanently blown would
    // show up as a much larger residual, or as digital silence).
    const bool pass = badBefore == 0 && badAfter == 0 && rmsAfter > 0.0 && rmsAfter < rmsBefore * 8.0;
    printf("  %s\n", pass ? "PASS -- burst is contained, output stays finite and cancellation recovers"
                          : "CHECK -- non-finite samples reached the output, or the filter never recovered");
    return pass;
}

// The reported latency is set once per prepareToPlay, and Tail Length then
// changes live: so the latency must be a function of the sample rate only,
// never of the filter length, or the dry path and the host's delay
// compensation would drift on every tail change.
bool testLatencyInvariantAcrossTailLengths() {
    printf("\n=== Latency invariance across Tail Lengths test ===\n");
    const int rates[] = { 44100, 48000, 96000 };
    bool pass = true;
    for (int rate : rates) {
        int latencies[4] = { -1, -1, -1, -1 };
        for (int tailIndex = 0; tailIndex < 4; ++tailIndex) {
            PAEchoCancellerAudioProcessor proc;
            setMonoLayout(proc);
            auto* param = proc.getTailLengthParameter();
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(tailIndex)));
            proc.prepareToPlay(rate, 512);
            latencies[tailIndex] = proc.getLatencySamples();
            proc.releaseResources();
        }
        const bool rateOk = latencies[0] == latencies[1] && latencies[1] == latencies[2]
                            && latencies[2] == latencies[3];
        printf("  %d Hz: latency per tail = %d / %d / %d / %d samples -> %s\n", rate,
               latencies[0], latencies[1], latencies[2], latencies[3], rateOk ? "invariant" : "VARIES");
        pass = pass && rateOk;
    }
    printf("  %s\n", pass ? "PASS -- latency is independent of Tail Length"
                          : "CHECK -- latency depends on Tail Length");
    return pass;
}

// Changing Tail Length mid-stream must never stall the audio callback (the
// AEC3 engine of 1.0.x once rebuilt inside processBlock, ~150ms+ of blocking
// work, a guaranteed live-rig dropout). The Kalman filter only drops or adds
// partitions, so no single callback may take longer than a generous
// "something is very wrong" threshold, and the reported latency must not
// move.
bool testLiveTailLengthChangeNonBlocking(int sampleRate) {
    printf("\n=== Live Tail Length change is non-blocking test (%d Hz) ===\n", sampleRate);
    const double durationS = 10.0;
    auto signals = makeSignals(sampleRate, durationS);

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }

    const int blockSize = 512;
    proc.prepareToPlay(sampleRate, blockSize);
    const int latencyBefore = proc.getLatencySamples();

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;

    const int n = static_cast<int>(signals.mic.size());
    const int changeAtSample = static_cast<int>(sampleRate * 4.0); // past the canceller's start-up
    bool changeApplied = false;
    double maxBlockMsAfterChange = 0.0;

    for (int pos = 0; pos + blockSize <= n; pos += blockSize) {
        if (!changeApplied && pos >= changeAtSample) {
            auto* param = proc.getTailLengthParameter();
            param->setValueNotifyingHost(param->convertTo0to1(0.0f)); // 800ms (default) -> 50ms
            changeApplied = true;
        }

        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);

        const auto t0 = std::chrono::steady_clock::now();
        proc.processBlock(buffer, midi);
        const auto t1 = std::chrono::steady_clock::now();

        if (changeApplied) {
            const double blockMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            maxBlockMsAfterChange = std::max(maxBlockMsAfterChange, blockMs);
        }
    }

    const int latencyAfter = proc.getLatencySamples();
    proc.releaseResources();

    // 80ms: far above any sane per-block cost (the steady-state cost of a
    // 512-sample block is well under 1ms even at 96kHz) but far below the
    // ~150ms+ the old inline rebuild burned -- loose enough to not flake on
    // a loaded machine, tight enough that a reintroduced inline rebuild
    // fails it every time.
    const bool noStall = maxBlockMsAfterChange < 80.0;
    const bool latencyStable = (latencyAfter == latencyBefore);

    printf("  Change requested at sample %d\n", changeAtSample);
    printf("  Max single processBlock after change: %.2f ms (limit 80ms; old inline rebuild: ~150ms+)\n",
           maxBlockMsAfterChange);
    printf("  Reported latency: %d -> %d samples (%s)\n", latencyBefore, latencyAfter,
           latencyStable ? "unchanged, as designed" : "CHANGED");

    const bool pass = noStall && latencyStable;
    printf("  %s\n", pass ? "PASS -- live tail change applies without blocking the audio thread" : "CHECK");
    return pass;
}

// A host may apply a session's saved Tail Length after prepareToPlay (or
// re-prepare with defaults before a bounce). With the 1.0.x AEC3 engine that
// went through a background rebuild polled on wall-clock time, so at bounce
// speed seconds of audio ran on the wrong filter. The Kalman filter applies
// Tail Length at the next block, so a bounce with the tail set after
// prepareToPlay must match one with it set before, from the start.
bool testFastBounceTailLengthAppliedPromptly(int sampleRate) {
    printf("\n=== Fast-bounce Tail Length applied promptly test (%d Hz) ===\n", sampleRate);
    const double durationS = 14.0; // makeSignals puts the voice burst at ~4.7-9.3s
    auto signals = makeSignals(sampleRate, durationS);
    const int savedTailIndex = 2; // 400ms; the plugin's default is index 3 (800ms)
    const int blockSize = 512;

    auto run = [&](bool tailBeforePrepare) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        auto setSavedTail = [&] {
            auto* param = proc.getTailLengthParameter();
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(savedTailIndex)));
        };
        if (tailBeforePrepare)
            setSavedTail();
        proc.setNonRealtime(true);
        proc.prepareToPlay(sampleRate, blockSize);
        if (!tailBeforePrepare)
            setSavedTail();

        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        juce::MidiBuffer midi;
        const int n = static_cast<int>(signals.mic.size());
        std::vector<float> out(static_cast<size_t>(n), 0.0f);
        for (int pos = 0; pos + blockSize <= n; pos += blockSize) {
            buffer.clear();
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
                mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
            for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
                refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);
            proc.processBlock(buffer, midi);
            auto mainOut = proc.getBusBuffer(buffer, false, 0);
            for (int s = 0; s < blockSize; ++s)
                out[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);
        }
        proc.releaseResources();
        return out;
    };

    const auto before = run(true);
    const auto after = run(false);

    // The two runs must be sample-identical: the tail applies at the first
    // block either way. Any difference means audio ran on the wrong tail.
    double maxDiff = 0.0;
    for (size_t i = 0; i < before.size(); ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(before[i] - after[i])));
    printf("  Max sample difference, tail set before vs after prepareToPlay: %.3g\n", maxDiff);

    const bool pass = maxDiff < 1e-6;
    printf("  %s\n", pass ? "PASS -- a Tail Length applied after prepareToPlay takes effect from the first block"
                          : "CHECK -- the saved Tail Length is applied late");
    return pass;
}

// The delay readout in the editor is only useful if the canceller's
// estimate flows through the getter -- confirm an estimate appears once the
// canceller has signal to lock onto, that it lands in a sane range (the
// synthetic leak is at 5ms; the point is "present and plausible", not a
// precise figure), and that release clears it.
bool testDelayStatsExposed(int sampleRate) {
    printf("\n=== Echo-path delay stats exposed test (%d Hz) ===\n", sampleRate);
    const double durationS = 8.0;
    auto signals = makeSignals(sampleRate, durationS);

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }

    const int blockSize = 512;
    proc.prepareToPlay(sampleRate, blockSize);
    const int delayBeforeAudio = proc.getEstimatedEchoPathDelayMs();

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;
    const int n = static_cast<int>(signals.mic.size());
    for (int pos = 0; pos + blockSize <= n; pos += blockSize) {
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);
        proc.processBlock(buffer, midi);
    }

    const int delayMs = proc.getEstimatedEchoPathDelayMs();
    proc.releaseResources();
    const int delayAfterRelease = proc.getEstimatedEchoPathDelayMs();

    printf("  Before audio: %d   After 8s of leak: %d ms   After release: %d\n",
           delayBeforeAudio, delayMs, delayAfterRelease);

    const bool pass = delayMs >= 0 && delayMs <= 100 && delayAfterRelease == -1;
    printf("  %s\n", pass ? "PASS -- delay estimate is exposed and plausible"
                          : "CHECK -- delay estimate missing or implausible");
    return pass;
}

bool testStateSaveRestore() {
    printf("\n=== State save/restore round-trip test ===\n");

    PAEchoCancellerAudioProcessor proc;
    const auto setTo = [](juce::RangedAudioParameter* param, float value) {
        param->setValueNotifyingHost(param->convertTo0to1(value));
    };
    setTo(proc.getTailLengthParameter(), 2.0f); // "400ms"
    setTo(proc.getAmountParameter(), 63.0f);
    setTo(proc.getMaxReductionParameter(), -7.5f);
    setTo(proc.getResponseParameter(), 33.0f);
    setTo(proc.getHpfFrequencyParameter(), 150.0f);
    setTo(proc.getReferenceGainParameter(), -4.0f);
    setTo(proc.getDryWetMixParameter(), 37.0f);

    juce::MemoryBlock state;
    proc.getStateInformation(state);

    PAEchoCancellerAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    const bool pass =
        restored.getTailLengthParameter()->getIndex() == 2 &&
        std::abs(restored.getAmountParameter()->get() - 63.0f) < 0.5f &&
        std::abs(restored.getMaxReductionParameter()->get() + 7.5f) < 0.05f &&
        std::abs(restored.getResponseParameter()->get() - 33.0f) < 0.5f &&
        std::abs(restored.getHpfFrequencyParameter()->get() - 150.0f) < 0.5f &&
        std::abs(restored.getReferenceGainParameter()->get() + 4.0f) < 0.05f &&
        std::abs(restored.getDryWetMixParameter()->get() - 37.0f) < 0.5f;

    printf("  Tail Length=%d Amount=%.1f%% MaxReduction=%.1fdB Response=%.1fms HPF=%.1fHz Trim=%.1fdB DryWet=%.1f%%\n",
           restored.getTailLengthParameter()->getIndex(),
           static_cast<double>(restored.getAmountParameter()->get()),
           static_cast<double>(restored.getMaxReductionParameter()->get()),
           static_cast<double>(restored.getResponseParameter()->get()),
           static_cast<double>(restored.getHpfFrequencyParameter()->get()),
           static_cast<double>(restored.getReferenceGainParameter()->get()),
           static_cast<double>(restored.getDryWetMixParameter()->get()));
    printf("  %s\n", pass ? "PASS -- all parameters round-tripped correctly" : "CHECK");

    // Bypass is deliberately absent from saved state (hosts own bypass via
    // AU/VST3's bypass mechanism -- see getStateInformation): confirm the
    // serialized tree contains no "bypass" key even when the parameter is
    // on at save time, and that a crafted state which DOES contain one is
    // ignored on restore rather than applied.
    proc.getBypassParameter()->setValueNotifyingHost(1.0f);
    juce::MemoryBlock bypassedState;
    proc.getStateInformation(bypassedState);
    const auto parsed = juce::ValueTree::readFromData(bypassedState.getData(), bypassedState.getSize());
    const bool bypassExcluded = parsed.isValid() && !parsed.hasProperty("bypass");

    juce::ValueTree crafted("PAEchoCancellerState");
    crafted.setProperty("bypass", 1.0f, nullptr);
    juce::MemoryBlock craftedBlock;
    {
        juce::MemoryOutputStream stream(craftedBlock, false);
        crafted.writeToStream(stream);
    }
    PAEchoCancellerAudioProcessor craftedTarget;
    craftedTarget.setStateInformation(craftedBlock.getData(), static_cast<int>(craftedBlock.getSize()));
    const bool bypassNotRestored = craftedTarget.getBypassParameter()->getValue() < 0.5f;

    printf("  bypass paramID in saved state: %s (expect absent)   crafted bypass=1 state restored: %s (expect ignored)\n",
           bypassExcluded ? "absent" : "PRESENT", bypassNotRestored ? "ignored" : "APPLIED");
    printf("  %s\n", (bypassExcluded && bypassNotRestored)
                          ? "PASS -- bypass stays out of saved state in both directions"
                          : "CHECK -- bypass leaked into state save/restore");
    return pass && bypassExcluded && bypassNotRestored;
}

// A session saved by 1.0.x: the controls that carried over (same IDs) come
// back as saved, the new ones start at their defaults, and the values of
// the removed Classic-engine controls are ignored without disturbing
// anything else.
bool testOldSessionRestore() {
    printf("\n=== 1.0.x session restore test ===\n");

    juce::ValueTree old("PAEchoCancellerState");
    old.setProperty("tailLength", 1.0f / 3.0f, nullptr);  // index 1, "200ms"
    old.setProperty("suppressionStrength", 1.0f, nullptr); // Hard
    old.setProperty("hpfFrequency", 0.5f, nullptr);
    old.setProperty("referenceGain", 0.75f, nullptr);     // +12 dB
    old.setProperty("limitHfGain", 1.0f, nullptr);
    old.setProperty("nearendSensitivity", 0.2f, nullptr);
    old.setProperty("protectionHoldTime", 0.9f, nullptr);
    old.setProperty("dryWetMix", 0.8f, nullptr);
    old.setProperty("nearendDetector", 1.0f, nullptr);
    old.setProperty("transitionSmoothing", 0.3f, nullptr);
    old.setProperty("engine", 1.0f, nullptr);             // Classic
    juce::MemoryBlock block;
    {
        juce::MemoryOutputStream stream(block, false);
        old.writeToStream(stream);
    }

    PAEchoCancellerAudioProcessor fresh;
    PAEchoCancellerAudioProcessor restored;
    restored.setStateInformation(block.getData(), static_cast<int>(block.getSize()));

    const auto same = [](juce::RangedAudioParameter* a, float normalised) {
        return std::abs(a->getValue() - normalised) < 1e-4f;
    };
    const auto atDefault = [](juce::RangedAudioParameter* a) {
        return std::abs(a->getValue() - a->getDefaultValue()) < 1e-4f;
    };
    const bool kept = restored.getTailLengthParameter()->getIndex() == 1
                      && same(restored.getHpfFrequencyParameter(), 0.5f)
                      && same(restored.getReferenceGainParameter(), 0.75f)
                      && same(restored.getDryWetMixParameter(), 0.8f);
    const bool newAtDefaults = atDefault(restored.getAmountParameter())
                               && atDefault(restored.getMaxReductionParameter())
                               && atDefault(restored.getResponseParameter());
    const bool sameParameterCount = restored.getParameters().size() == fresh.getParameters().size();

    printf("  Tail=%d HPF=%.1fHz Trim=%.1fdB Mix=%.1f%% | Amount=%.1f%% MaxReduction=%.1fdB Response=%.1fms\n",
           restored.getTailLengthParameter()->getIndex(),
           static_cast<double>(restored.getHpfFrequencyParameter()->get()),
           static_cast<double>(restored.getReferenceGainParameter()->get()),
           static_cast<double>(restored.getDryWetMixParameter()->get()),
           static_cast<double>(restored.getAmountParameter()->get()),
           static_cast<double>(restored.getMaxReductionParameter()->get()),
           static_cast<double>(restored.getResponseParameter()->get()));
    const bool pass = kept && newAtDefaults && sameParameterCount;
    printf("  %s\n", pass ? "PASS -- carried-over controls restored, new ones at defaults, removed ones ignored"
                          : "CHECK -- old session restore is wrong");
    return pass;
}

// Amount, Max Reduction and Response are the suppressor's whole interface,
// so pin down what each one means on the synthetic material:
//  - Amount 0% is the adaptive filter alone, and so is Max Reduction 0 dB
//    at any Amount (the suppressor may not cut anywhere): the two outputs
//    must be identical.
//  - More Amount removes more PA (25% < 100% in the PA-only window).
//  - A deeper Max Reduction lets 100% go further than a shallow one.
//  - Every setting applies live: changing all three mid-stream must not
//    produce a spike or a dropout.
bool testSuppressorControls(int sampleRate) {
    printf("\n=== Amount / Max Reduction / Response test (%d Hz) ===\n", sampleRate);
    const double durationS = 6.0;
    auto signals = makeSignals(sampleRate, durationS);

    auto runWith = [&](float amount, float maxReductionDb, float responseMs) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        proc.getAmountParameter()->setValueNotifyingHost(proc.getAmountParameter()->convertTo0to1(amount));
        proc.getMaxReductionParameter()->setValueNotifyingHost(
            proc.getMaxReductionParameter()->convertTo0to1(maxReductionDb));
        proc.getResponseParameter()->setValueNotifyingHost(proc.getResponseParameter()->convertTo0to1(responseMs));
        return runThroughProcessor(proc, sampleRate, signals.reference, signals.mic);
    };

    const auto filterOnly = runWith(0.0f, -12.0f, 20.0f);
    const auto noFloor = runWith(80.0f, 0.0f, 20.0f);
    const auto mid = runWith(25.0f, -12.0f, 20.0f);
    const auto full = runWith(100.0f, -12.0f, 20.0f);
    const auto fullShallow = runWith(100.0f, -3.0f, 20.0f);
    const auto fullDeep = runWith(100.0f, -24.0f, 20.0f);

    double maxDiff = 0.0;
    for (size_t i = 0; i < filterOnly.size(); ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(filterOnly[i] - noFloor[i])));

    const double picS0 = 0.5, picS1 = durationS * (2.0 / 6.0); // PA-only window, as testBasicCancellation
    const double mic = rmsDbfs(signals.mic, sampleRate, picS0, picS1);
    const auto red = [&](const std::vector<float>& out) { return mic - rmsDbfs(out, sampleRate, picS0, picS1); };
    printf("  PA-only reduction: filter only %.1f dB, Amount 25%% %.1f dB, 100%% %.1f dB; "
           "100%% at -3 dB %.1f dB, at -24 dB %.1f dB\n",
           red(filterOnly), red(mid), red(full), red(fullShallow), red(fullDeep));
    printf("  Max difference, Amount 0%% vs Max Reduction 0 dB: %.3g\n", maxDiff);

    // Live changes of all three, mid-stream.
    PAEchoCancellerAudioProcessor live;
    setMonoLayout(live);
    const int blockSize = 256;
    live.prepareToPlay(sampleRate, blockSize);
    const int totalChannels = std::max(live.getTotalNumInputChannels(), live.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;
    const int n = static_cast<int>(signals.mic.size());
    float inputPeak = 0.0f, outputPeak = 0.0f;
    for (float v : signals.mic) inputPeak = std::max(inputPeak, std::abs(v));
    int zeroRun = 0, longestZeroRunAfterStart = 0;
    for (int pos = 0; pos + blockSize <= n; pos += blockSize) {
        const int step = pos / (sampleRate / 4); // a new setting every 250ms
        live.getAmountParameter()->setValueNotifyingHost((step % 5) / 4.0f);
        live.getMaxReductionParameter()->setValueNotifyingHost((step % 3) / 2.0f);
        live.getResponseParameter()->setValueNotifyingHost((step % 4) / 3.0f);
        buffer.clear();
        auto mainIn = live.getBusBuffer(buffer, true, 0);
        auto refIn = live.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);
        live.processBlock(buffer, midi);
        auto mainOut = live.getBusBuffer(buffer, false, 0);
        for (int s = 0; s < blockSize; ++s) {
            const float v = std::abs(mainOut.getSample(0, s));
            outputPeak = std::max(outputPeak, v);
            if (pos > sampleRate / 10) { // past the primed start
                zeroRun = v < 1.0e-7f ? zeroRun + 1 : 0;
                longestZeroRunAfterStart = std::max(longestZeroRunAfterStart, zeroRun);
            }
        }
    }
    live.releaseResources();
    printf("  Live changes every 250ms: output peak %.3f vs input peak %.3f, longest zero run %d samples\n",
           outputPeak, inputPeak, longestZeroRunAfterStart);

    const bool zeroIsFilterOnly = maxDiff < 1e-7;
    const bool amountWorks = red(full) > red(mid) + 1.0 && red(mid) > red(filterOnly);
    const bool floorWorks = red(fullDeep) > red(fullShallow) + 1.0;
    const bool liveClean = outputPeak < inputPeak * 1.5f && longestZeroRunAfterStart < sampleRate / 200;
    const bool pass = zeroIsFilterOnly && amountWorks && floorWorks && liveClean;
    printf("  %s%s%s%s%s\n", pass ? "PASS" : "CHECK", zeroIsFilterOnly ? "" : " -- 0 dB/0% DIFFER",
           amountWorks ? "" : " -- AMOUNT NOT MONOTONIC", floorWorks ? "" : " -- MAX REDUCTION HAS NO EFFECT",
           liveClean ? "" : " -- LIVE CHANGE GLITCHED");
    return pass;
}

// Bypass check (a): with bypass engaged, the output must be the TRULY raw
// input -- HPF included in "unmodified", so content below the 150Hz cutoff
// must survive -- delayed by exactly the latency-matched amount, while an
// actively-cancelling AEC3 (live reference, real leakage in the mic) runs
// underneath. The dry-path tap can't provide this (it's post-HPF), which
// is exactly what this test would catch if the bypass were wired there: the
// comparison against the raw signal fails loudly on the missing 40Hz
// content, while the post-HPF control comparison below confirms the test
// could tell the difference.
bool testBypassRawPassthrough(int sampleRate) {
    printf("\n=== Bypass raw passthrough test (%d Hz) ===\n", sampleRate);
    const double durationS = 4.0;
    const int n = static_cast<int>(sampleRate * durationS);

    // Reference: broadband-ish noise, leaked into the mic at 5ms/0.5 so
    // AEC3 has genuine echo to chase -- bypass must pass the input through
    // regardless of how hard the canceller is working. Plus a 40Hz
    // component the HPF would strip (the "truly raw" sentinel) and
    // independent noise standing in for audience content.
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = 0.5f * dist(rng);
    reference = onePoleLowpass(reference, 0.6f);

    const int leakDelay = sampleRate / 200; // 5ms
    std::vector<float> mic(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        float v = 0.4f * static_cast<float>(std::sin(2.0 * M_PI * 40.0 * t));
        if (i >= leakDelay) v += 0.5f * reference[static_cast<size_t>(i - leakDelay)];
        v += 0.1f * dist(rng);
        mic[static_cast<size_t>(i)] = v;
    }

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    proc.getBypassParameter()->setValueNotifyingHost(1.0f); // bypassed from the very first sample

    const int blockSize = 128; // fixed, non-frame-aligned, like a real low-latency rig
    proc.prepareToPlay(sampleRate, blockSize);
    const int nominalLatency = proc.getLatencySamples();

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output(static_cast<size_t>(n), 0.0f);
    int pos = 0;
    while (pos + blockSize <= n) {
        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, reference.data() + pos, blockSize);
        proc.processBlock(buffer, midi);
        auto mainOut = proc.getBusBuffer(buffer, false, 0);
        for (int s = 0; s < blockSize; ++s)
            output[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);
        pos += blockSize;
    }
    proc.releaseResources();
    // The driver drops a final partial block whenever blockSize doesn't
    // divide n (e.g. 176400 % 128 = 16 at 44.1kHz) -- output stays zero
    // there, which is the harness's doing, not the plugin's. Compare only
    // what was actually processed.
    const int processedEnd = pos;

    // The bypass path moves samples through FIFOs by plain copy, so at SOME
    // integer delay the match should be exact (not merely close) once the
    // pipeline has settled. Find that delay by direct search around the
    // nominal figure, then measure the residual at it.
    const int frameSize = sampleRate / 100;
    const int compareStart = 2 * sampleRate; // well past startup fill
    auto rmsDiffAtDelay = [&](const std::vector<float>& expected, int delay) {
        double sumSq = 0.0;
        int count = 0;
        for (int i = compareStart; i < processedEnd; ++i) {
            const int src = i - delay;
            if (src < 0 || src >= n) continue;
            const double d = static_cast<double>(output[static_cast<size_t>(i)])
                              - expected[static_cast<size_t>(src)];
            sumSq += d * d;
            ++count;
        }
        return count > 0 ? std::sqrt(sumSq / count) : 1e9;
    };
    int bestDelay = nominalLatency;
    double bestRms = rmsDiffAtDelay(mic, nominalLatency);
    for (int d = std::max(0, nominalLatency - frameSize); d <= nominalLatency + frameSize; ++d) {
        const double r = rmsDiffAtDelay(mic, d);
        if (r < bestRms) { bestRms = r; bestDelay = d; }
    }

    // Control: the same comparison against a post-HPF copy of the input at
    // the same delay must fail loudly (the 40Hz sentinel alone is ~0.28
    // RMS) -- proving the test can distinguish raw from post-HPF, i.e.
    // that a bypass mis-wired to the dry tap could not pass.
    HighPassFilterChain hpf;
    hpf.setCutoff(sampleRate, proc.getHpfFrequencyParameter()->get());
    std::vector<float> postHpf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) postHpf[static_cast<size_t>(i)] = hpf.processSample(mic[static_cast<size_t>(i)]);
    const double hpfRms = rmsDiffAtDelay(postHpf, bestDelay);

    printf("  Bypassed output vs raw input: best delay=%d (nominal %d, %+d), residual RMS=%.2e\n",
           bestDelay, nominalLatency, bestDelay - nominalLatency, bestRms);
    {
        // Where does any residual live? Isolated bad samples point at a
        // glitch event; a contiguous mismatched region points at the stream
        // delay changing mid-run (FIFO underrun inserting zeros).
        int mismatches = 0, firstMismatch = -1, lastMismatch = -1;
        for (int i = compareStart; i < processedEnd; ++i) {
            const int src = i - bestDelay;
            if (src < 0) continue;
            if (std::abs(output[static_cast<size_t>(i)] - mic[static_cast<size_t>(src)]) > 1.0e-4f) {
                if (firstMismatch < 0) firstMismatch = i;
                lastMismatch = i;
                ++mismatches;
            }
        }
        if (mismatches > 0)
            printf("  [debug] %d samples with |diff|>1e-4 in %d..%d (first=%d last=%d)\n",
                   mismatches, compareStart, processedEnd, firstMismatch, lastMismatch);
    }
    printf("  Same comparison vs post-HPF input (control, must be large): RMS=%.4f\n", hpfRms);

    const bool pass = bestRms < 1.0e-6                      // exact raw passthrough (float-copy exact)
                       && std::abs(bestDelay - nominalLatency) <= frameSize // latency-matched, no wild offset
                       && hpfRms > 0.05;                     // and provably raw, not post-HPF
    printf("  %s\n", pass ? "PASS -- bypass is a latency-matched, truly-raw passthrough"
                          : "CHECK -- bypass output is not the raw input at the matched delay");
    return pass;
}

// Bypass check (b): toggling bypass mid-stream, both directions, produces
// no click/spike -- same no-spike detection pattern as
// testClickFreeRoomChange -- and actually takes effect (bypassed span
// matches the raw input; processed span doesn't).
bool testBypassToggleClickFree(int sampleRate) {
    printf("\n=== Bypass toggle click-free test (%d Hz) ===\n", sampleRate);
    const double durationS = 6.0;
    auto signals = makeSignals(sampleRate, durationS);
    const int n = static_cast<int>(signals.mic.size());

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }

    const int blockSize = 128;
    proc.prepareToPlay(sampleRate, blockSize);
    const int nominalLatency = proc.getLatencySamples();
    const int bypassOnAt = 2 * sampleRate;
    const int bypassOffAt = 4 * sampleRate;

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output(static_cast<size_t>(n), 0.0f);
    int pos = 0;
    bool onApplied = false, offApplied = false;
    while (pos + blockSize <= n) {
        if (!onApplied && pos >= bypassOnAt) {
            proc.getBypassParameter()->setValueNotifyingHost(1.0f);
            onApplied = true;
        }
        if (!offApplied && pos >= bypassOffAt) {
            proc.getBypassParameter()->setValueNotifyingHost(0.0f);
            offApplied = true;
        }
        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);
        proc.processBlock(buffer, midi);
        auto mainOut = proc.getBusBuffer(buffer, false, 0);
        for (int s = 0; s < blockSize; ++s)
            output[static_cast<size_t>(pos + s)] = mainOut.getSample(0, s);
        pos += blockSize;
    }
    proc.releaseResources();

    float inputPeak = 0.0f;
    for (float v : signals.mic) inputPeak = std::max(inputPeak, std::abs(v));
    float outputPeak = 0.0f;
    int spikeIndex = -1;
    for (size_t i = 0; i < output.size(); ++i) {
        const float a = std::abs(output[i]);
        outputPeak = std::max(outputPeak, a);
        if (a > inputPeak * 1.5f && spikeIndex < 0) spikeIndex = static_cast<int>(i);
    }

    // Effect checks, well clear of both toggles and their ~5ms ramps: mid-
    // bypass the output equals the raw input at the matched delay; after
    // toggling back it must NOT (AEC3 + HPF are audibly at work again).
    auto rmsDiffVsRaw = [&](int startSample, int endSample, int delay) {
        double sumSq = 0.0;
        int count = 0;
        for (int i = startSample; i < endSample && i < n; ++i) {
            const int src = i - delay;
            if (src < 0) continue;
            const double d = static_cast<double>(output[static_cast<size_t>(i)])
                              - signals.mic[static_cast<size_t>(src)];
            sumSq += d * d;
            ++count;
        }
        return count > 0 ? std::sqrt(sumSq / count) : 1e9;
    };
    // The exact stream delay sits within a few samples of nominal but isn't
    // guaranteed to equal it (the same block/frame phase slop
    // testDryWetAlignment documents), and even a 3-sample offset makes the
    // 1.2kHz voice content read as a large false diff -- so find the actual
    // delay in the bypassed span the same way the raw-passthrough test
    // does, then hold the processed-span comparison to that same delay.
    const int frameSize = sampleRate / 100;
    const int bypSpanStart = 3 * sampleRate, bypSpanEnd = 4 * sampleRate - sampleRate / 10;
    int bestDelay = nominalLatency;
    double bypassedDiff = rmsDiffVsRaw(bypSpanStart, bypSpanEnd, nominalLatency);
    for (int d = std::max(0, nominalLatency - frameSize); d <= nominalLatency + frameSize; ++d) {
        const double r = rmsDiffVsRaw(bypSpanStart, bypSpanEnd, d);
        if (r < bypassedDiff) { bypassedDiff = r; bestDelay = d; }
    }
    const double processedDiff = rmsDiffVsRaw(5 * sampleRate, n, bestDelay);

    printf("  Input peak: %.4f   Output peak: %.4f   (spike threshold %.4f)\n",
           inputPeak, outputPeak, inputPeak * 1.5f);
    printf("  RMS diff vs raw input at delay %d (nominal %d): bypassed span=%.4f (expect ~0), processed span=%.4f (expect >>0)\n",
           bestDelay, nominalLatency, bypassedDiff, processedDiff);

    const bool noSpike = spikeIndex < 0;
    const bool tookEffect = bypassedDiff < 0.02 && processedDiff > 10.0 * std::max(bypassedDiff, 1e-6);
    const bool pass = noSpike && tookEffect;
    printf("  %s%s%s\n", pass ? "PASS -- bypass toggles click-free and actually engages/disengages" : "CHECK",
           noSpike ? "" : " -- SPIKE DETECTED", tookEffect ? "" : " -- BYPASS DID NOT ENGAGE/DISENGAGE AS EXPECTED");
    return pass;
}

bool testHighPassFilterResponse() {
    printf("\n=== High-pass filter response test ===\n");
    const int sampleRate = 48000;
    const float cutoffHz = 80.0f;
    const double durationS = 1.0;
    const int n = static_cast<int>(sampleRate * durationS);

    auto measureAttenuationDb = [&](float toneFreq) {
        HighPassFilterChain chain;
        chain.setCutoff(sampleRate, cutoffHz);

        const int settleStart = static_cast<int>(n * 0.75); // past any filter settling transient
        double sumSqIn = 0.0, sumSqOut = 0.0;
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            const float x = static_cast<float>(std::sin(2.0 * M_PI * toneFreq * t));
            const float y = chain.processSample(x);
            if (i >= settleStart) {
                sumSqIn += static_cast<double>(x) * x;
                sumSqOut += static_cast<double>(y) * y;
            }
        }
        const double rmsIn = std::sqrt(sumSqIn / (n - settleStart));
        const double rmsOut = std::sqrt(sumSqOut / (n - settleStart));
        return 20.0 * std::log10(rmsOut / rmsIn);
    };

    const double att1kHz = measureAttenuationDb(1000.0f);
    const double attCutoff = measureAttenuationDb(cutoffHz);
    const double att1OctBelow = measureAttenuationDb(cutoffHz / 2.0f);
    const double att2OctBelow = measureAttenuationDb(cutoffHz / 4.0f);

    printf("  1kHz (passband):     %6.1f dB (expect ~0)\n", att1kHz);
    printf("  %3.0fHz (cutoff):      %6.1f dB (expect ~-3, standard Butterworth corner)\n", cutoffHz, attCutoff);
    printf("  %3.0fHz (1 oct below): %6.1f dB (expect ~-24 -- a 12dB/oct filter would only show ~-15 here)\n",
           cutoffHz / 2.0f, att1OctBelow);
    printf("  %3.0fHz (2 oct below): %6.1f dB (expect ~-48)\n", cutoffHz / 4.0f, att2OctBelow);

    const bool pass = att1kHz > -1.0 && att1kHz < 1.0 && attCutoff > -5.0 && attCutoff < -1.0
                       && att1OctBelow < -18.0 && att2OctBelow < -40.0;
    printf("  %s\n", pass ? "PASS -- confirms a genuine 4th-order (24dB/oct) Butterworth response" : "CHECK");
    return pass;
}

} // namespace

static void printDefaultBusLayout() {
    PAEchoCancellerAudioProcessor proc; // freshly constructed, no layout negotiation yet
    printf("=== Default bus layout (as declared, before any host negotiation) ===\n");
    printf("  Input buses:  %d\n", proc.getBusCount(true));
    for (int i = 0; i < proc.getBusCount(true); ++i) {
        auto* bus = proc.getBus(true, i);
        printf("    [%d] \"%s\": %s (%d ch)\n", i, bus->getName().toRawUTF8(),
               bus->getCurrentLayout().getDescription().toRawUTF8(), bus->getNumberOfChannels());
    }
    printf("  Output buses: %d\n", proc.getBusCount(false));
    for (int i = 0; i < proc.getBusCount(false); ++i) {
        auto* bus = proc.getBus(false, i);
        printf("    [%d] \"%s\": %s (%d ch)\n", i, bus->getName().toRawUTF8(),
               bus->getCurrentLayout().getDescription().toRawUTF8(), bus->getNumberOfChannels());
    }
    printf("  Total: %d in / %d out\n\n", proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
}

// Informational only (not pass/fail): measures actual wall-clock processing
// cost per Tail Length setting, since the adaptive filter's per-frame CPU
// cost scales with filter length (its partition count) -- this gives a concrete
// relative-cost figure to go alongside the user's own live CPU-meter
// readings when deciding how many simultaneous instances are affordable.
static void benchmarkTailLengthCpuCost(int sampleRate) {
    printf("\n=== Tail Length CPU cost benchmark (%d Hz, wall-clock, single core) ===\n", sampleRate);
    const double durationS = 10.0;
    auto signals = makeSignals(sampleRate, durationS);

    const char* labels[] = { "50ms", "200ms", "400ms", "800ms" };
    double baselineSeconds = 0.0;

    for (int tailIndex = 0; tailIndex < 4; ++tailIndex) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        auto* param = proc.getTailLengthParameter();
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(tailIndex)));

        const auto start = std::chrono::steady_clock::now();
        runThroughProcessor(proc, sampleRate, signals.reference, signals.mic);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double elapsedSeconds = std::chrono::duration<double>(elapsed).count();

        if (tailIndex == 0) baselineSeconds = elapsedSeconds;
        printf("  %-5s: %.3fs to process %.0fs of audio (%.2fx real-time, %.2fx the 50ms cost)\n",
               labels[tailIndex], elapsedSeconds, durationS, durationS / elapsedSeconds,
               elapsedSeconds / baselineSeconds);
    }
}

// ---------------------------------------------------------------------------
// Real-material regression harness
//
// Every test above runs on synthetic signals, and synthetic tests have
// demonstrably missed real-material bugs before (the offline-render
// divergence was only ever audible on actual concert recordings). This mode
// runs a real audience-mic recording and its matching PA reference feed
// through the actual processor across a matrix of settings and reports
// objective per-config metrics, so tuning experiments (Amount, Max
// Reduction, Response, Tail Length) can be measured on the material that
// matters instead of judged by ear alone.
//
//   PAEchoCancellerVerify --real <mic.wav> <reference.wav> [--seconds N] [--write-outputs] [--irregular]
//
// The two files must share one sample rate; channel 0 of each is used.
// --seconds N trims the material to its first N seconds -- a 60-90s slice
// of a real show is plenty to rank configs, and 6 configs over a full
// half-hour recording is minutes of pointless waiting.
// Metrics deliberately skip the first 5 seconds (the canceller's start-up
// and convergence). "Reduction" here is mic-vs-output level over
// the whole measured span -- on real material that's bleed removal *plus*
// any wanted-content suppression (there's no ground-truth separation), so
// treat it as a comparative figure between configs, not an absolute
// quality score. --write-outputs saves each config's processed audio as
// 16-bit WAVs next to the CSV for listening comparison.
// ---------------------------------------------------------------------------

bool loadWavMono(const juce::File& file, std::vector<float>& out, double& sampleRateOut) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) {
        printf("ERROR: cannot read '%s'\n", file.getFullPathName().toRawUTF8());
        return false;
    }
    const auto numSamples = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), numSamples);
    reader->read(&buf, 0, numSamples, 0, true, true);
    out.assign(buf.getReadPointer(0), buf.getReadPointer(0) + numSamples);
    sampleRateOut = reader->sampleRate;
    return true;
}

// Normalized autocorrelation of a (mean-removed) metric trace, for spotting
// periodicity -- the same approach used for the earlier raw-AEC3 uniform-
// cadence disproof of the ~1s dropout lead (that analysis found no peak near
// 1s when driving AEC3 directly in clean 10ms frames; this reuses the method
// on the full plugin path). Returns r(lag) for lag = 1..maxLag over the
// trace with its first skipBins bins dropped (AEC3 bootstrap/convergence --
// the settling ramp is itself a strong low-frequency trend that would
// swamp any small periodic signature).
static std::vector<double> normalizedAutocorrelation(const std::vector<double>& trace,
                                                     int skipBins, int maxLag) {
    std::vector<double> x(trace.begin() + std::min<size_t>(static_cast<size_t>(skipBins), trace.size()),
                          trace.end());
    const int n = static_cast<int>(x.size());
    std::vector<double> r;
    if (n < maxLag * 2)
        return r;
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= n;
    for (auto& v : x) v -= mean;
    double denom = 0.0;
    for (double v : x) denom += v * v;
    if (denom <= 0.0)
        return r;
    r.reserve(static_cast<size_t>(maxLag));
    for (int lag = 1; lag <= maxLag; ++lag) {
        double acc = 0.0;
        for (int i = 0; i + lag < n; ++i)
            acc += x[static_cast<size_t>(i)] * x[static_cast<size_t>(i + lag)];
        r.push_back(acc / denom);
    }
    return r;
}

// Prints the autocorrelation verdict for one metric trace (binned at
// binsPerSecond): the strongest lag overall, and how the ~1s region
// (0.8s-1.2s) compares against the rest -- a genuine ~1s periodic dropout
// would put a clear local peak there.
static void reportPeriodicity(const char* label, const std::vector<double>& trace, int binsPerSecond) {
    const int skipBins = 5 * binsPerSecond; // 5s, same span --real's metrics already skip
    const int maxLag = 5 * binsPerSecond;   // look out to 5s
    const auto r = normalizedAutocorrelation(trace, skipBins, maxLag);
    if (r.empty()) {
        printf("  %-14s trace too short for autocorrelation\n", label);
        return;
    }
    int bestLag = 1;
    for (int lag = 1; lag <= static_cast<int>(r.size()); ++lag)
        if (r[static_cast<size_t>(lag - 1)] > r[static_cast<size_t>(bestLag - 1)]) bestLag = lag;
    // The ~1s window of interest, and the max outside it for contrast.
    const int oneSecLo = (8 * binsPerSecond) / 10, oneSecHi = (12 * binsPerSecond) / 10;
    double oneSecMax = -2.0, elsewhereMax = -2.0;
    int oneSecMaxLag = oneSecLo;
    for (int lag = 1; lag <= static_cast<int>(r.size()); ++lag) {
        const double v = r[static_cast<size_t>(lag - 1)];
        if (lag >= oneSecLo && lag <= oneSecHi) {
            if (v > oneSecMax) { oneSecMax = v; oneSecMaxLag = lag; }
        } else if (v > elsewhereMax) {
            elsewhereMax = v;
        }
    }
    printf("  %-14s peak r=%.3f at %.2fs; ~1s region max r=%.3f at %.2fs (elsewhere max r=%.3f)%s\n",
           label, r[static_cast<size_t>(bestLag - 1)], static_cast<double>(bestLag) / binsPerSecond,
           oneSecMax, static_cast<double>(oneSecMaxLag) / binsPerSecond, elsewhereMax,
           (oneSecMax > 0.3 && oneSecMax > elsewhereMax + 0.1)
               ? "  <-- ~1s PERIODICITY CANDIDATE" : "");
}

int runRealMaterialMode(const char* micPath, const char* refPath, double maxSeconds, bool writeOutputs,
                        bool irregularCadence) {
    printf("=== Real-material regression harness (%s cadence) ===\n",
           irregularCadence ? "IRREGULAR block-size" : "uniform 512-sample");
    std::vector<float> mic, ref;
    double micRate = 0.0, refRate = 0.0;
    if (!loadWavMono(juce::File(juce::String(micPath)), mic, micRate)) return 1;
    if (!loadWavMono(juce::File(juce::String(refPath)), ref, refRate)) return 1;
    if (!juce::exactlyEqual(micRate, refRate)) {
        printf("ERROR: sample rates differ (mic %.0f Hz, reference %.0f Hz) -- resample one first\n", micRate, refRate);
        return 1;
    }
    const int sampleRate = static_cast<int>(micRate);
    size_t n = std::min(mic.size(), ref.size());
    if (maxSeconds > 0.0)
        n = std::min(n, static_cast<size_t>(maxSeconds * sampleRate));
    mic.resize(n);
    ref.resize(n);
    const double lengthS = static_cast<double>(n) / sampleRate;
    printf("Material: %.1fs at %d Hz (mic '%s', ref '%s')\n", lengthS, sampleRate, micPath, refPath);

    const double skipS = 5.0; // start-up + convergence
    if (lengthS < skipS + 5.0) {
        printf("ERROR: need at least ~10s of material (%.1fs given)\n", lengthS);
        return 1;
    }

    // >=2kHz band isolation for the HF metric -- the range where residual
    // PA bleed (cymbals, vocal consonants, "hiss") is most audible.
    auto highBandRms = [sampleRate](const std::vector<float>& x, double startS) {
        HighPassFilterChain hpf;
        hpf.setCutoff(sampleRate, 2000.0f);
        const size_t start = static_cast<size_t>(startS * sampleRate);
        double sumSq = 0.0;
        size_t count = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const float f = hpf.processSample(x[i]); // run from 0 so filter state is settled by `start`
            if (i >= start) { sumSq += static_cast<double>(f) * f; ++count; }
        }
        return count > 0 ? std::sqrt(sumSq / static_cast<double>(count)) : 0.0;
    };

    auto fullBandRms = [sampleRate](const std::vector<float>& x, double startS) {
        const size_t start = static_cast<size_t>(startS * sampleRate);
        double sumSq = 0.0;
        size_t count = 0;
        for (size_t i = start; i < x.size(); ++i) { sumSq += static_cast<double>(x[i]) * x[i]; ++count; }
        return count > 0 ? std::sqrt(sumSq / static_cast<double>(count)) : 0.0;
    };

    const double micFullRms = fullBandRms(mic, skipS);
    const double micHighRms = highBandRms(mic, skipS);

    struct Config { int tailIndex; float amount; float maxReductionDb; float responseMs; const char* name; };
    // The defaults, then each suppressor control swept on its own (Max
    // Reduction and Response at 50% Amount, where they have room to act),
    // then the defaults at a shorter tail.
    const Config configs[] = {
        { 3, 80.0f, -12.0f, 30.0f, "default" },
        { 3, 0.0f, -12.0f, 20.0f, "amount0" },
        { 3, 50.0f, -12.0f, 20.0f, "amount50" },
        { 3, 100.0f, -12.0f, 20.0f, "amount100" },
        { 3, 50.0f, -6.0f, 20.0f, "amount50_maxred6" },
        { 3, 50.0f, -24.0f, 20.0f, "amount50_maxred24" },
        { 3, 50.0f, -12.0f, 5.0f, "amount50_resp5" },
        { 3, 50.0f, -12.0f, 50.0f, "amount50_resp50" },
        { 1, 80.0f, -12.0f, 30.0f, "tail200_default" },
    };

    // The irregular run writes its aggregate report to a separate file so a
    // cadence comparison never clobbers the uniform baseline it's being
    // compared against.
    juce::File csvFile(juce::File::getCurrentWorkingDirectory().getChildFile(
        irregularCadence ? "real_material_report_irregular.csv" : "real_material_report.csv"));
    juce::String csv("config,tail_ms,amount_pct,max_reduction_db,response_ms,fullband_reduction_db,highband_reduction_db,delay_ms\n");

    printf("\n%-22s %14s %14s %10s\n", "config", "full-band red.", ">=2kHz red.", "delay est.");
    static constexpr int tailMs[] = { 50, 200, 400, 800 };
    bool allRan = true;
    for (const auto& cfg : configs) {
        PAEchoCancellerAudioProcessor proc;
        if (!setMonoLayout(proc)) { printf("  FAILED to set mono layout\n"); return 1; }
        {
            const auto setTo = [](juce::RangedAudioParameter* param, float value) {
                param->setValueNotifyingHost(param->convertTo0to1(value));
            };
            setTo(proc.getTailLengthParameter(), static_cast<float>(cfg.tailIndex));
            setTo(proc.getAmountParameter(), cfg.amount);
            setTo(proc.getMaxReductionParameter(), cfg.maxReductionDb);
            setTo(proc.getResponseParameter(), cfg.responseMs);
        }

        // Irregular cadence: the same deliberately off-frame-boundary cycling
        // pattern runThroughProcessor uses for the synthetic suite -- the one
        // host-realistic condition the ~1s-dropout investigation had never
        // combined with real material and time-series state logging. Uniform
        // cadence (fixed 512) is the control: same material, same config,
        // only the block schedule differs.
        static const int blockPattern[] = { 512, 37, 129, 1, 4096, 256, 7 };
        const int patternLen = static_cast<int>(sizeof(blockPattern) / sizeof(blockPattern[0]));
        const int maxBlock = irregularCadence ? 4096 : 512;
        proc.setNonRealtime(true);
        proc.prepareToPlay(sampleRate, maxBlock);

        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, maxBlock);
        juce::MidiBuffer midi;
        std::vector<float> output(n, 0.0f);

        // The delay readout polled every ~100ms of material, on the same
        // 100ms grid the RMS traces below use -- fine enough to resolve a
        // ~1s periodicity (10 bins per cycle).
        const int binSize = sampleRate / 10;
        struct StatSample { size_t pos; int delayMs; };
        std::vector<StatSample> statSamples;
        size_t nextStatPoll = static_cast<size_t>(binSize);

        size_t pos = 0;
        int patternIdx = 0;
        while (pos < n) {
            const int blockSize = static_cast<int>(std::min<size_t>(
                static_cast<size_t>(irregularCadence ? blockPattern[patternIdx++ % patternLen] : 512),
                n - pos));
            if (blockSize <= 0) continue;
            buffer.setSize(totalChannels, blockSize, false, false, true);
            buffer.clear();
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
                mainIn.copyFrom(ch, 0, mic.data() + pos, blockSize);
            for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
                refIn.copyFrom(ch, 0, ref.data() + pos, blockSize);
            proc.processBlock(buffer, midi);
            auto mainOut = proc.getBusBuffer(buffer, false, 0);
            std::copy_n(mainOut.getReadPointer(0), blockSize, output.data() + pos);
            pos += static_cast<size_t>(blockSize);

            if (pos >= nextStatPoll) {
                statSamples.push_back({ pos, proc.getEstimatedEchoPathDelayMs() });
                while (nextStatPoll <= pos) nextStatPoll += static_cast<size_t>(binSize);
            }
        }

        const int delayMs = proc.getEstimatedEchoPathDelayMs();
        proc.releaseResources();

        // 100ms-binned RMS traces + the stat polls resampled onto the same
        // grid, written per config for plotting and autocorrelated right here
        // for the periodicity verdict.
        const size_t numBins = n / static_cast<size_t>(binSize);
        std::vector<double> outTraceDb(numBins), micTraceDb(numBins), delayTrace(numBins);
        {
            size_t statIdx = 0;
            int lastDelay = -1;
            for (size_t b = 0; b < numBins; ++b) {
                const size_t binEnd = (b + 1) * static_cast<size_t>(binSize);
                double outSumSq = 0.0, micSumSq = 0.0;
                for (size_t i = b * static_cast<size_t>(binSize); i < binEnd; ++i) {
                    outSumSq += static_cast<double>(output[i]) * output[i];
                    micSumSq += static_cast<double>(mic[i]) * mic[i];
                }
                outTraceDb[b] = 10.0 * std::log10(std::max(1e-12, outSumSq / binSize));
                micTraceDb[b] = 10.0 * std::log10(std::max(1e-12, micSumSq / binSize));
                while (statIdx < statSamples.size() && statSamples[statIdx].pos <= binEnd) {
                    lastDelay = statSamples[statIdx].delayMs;
                    ++statIdx;
                }
                delayTrace[b] = lastDelay;
            }
        }
        {
            juce::String trace("t_s,mic_rms_db,out_rms_db,delay_ms\n");
            for (size_t b = 0; b < numBins; ++b)
                trace << juce::String(0.1 * static_cast<double>(b + 1), 1) << ","
                      << juce::String(micTraceDb[b], 2) << "," << juce::String(outTraceDb[b], 2) << ","
                      << static_cast<int>(delayTrace[b]) << "\n";
            juce::File::getCurrentWorkingDirectory()
                .getChildFile(juce::String("real_trace_") + (irregularCadence ? "irregular_" : "uniform_")
                              + cfg.name + ".csv")
                .replaceWithText(trace);
        }
        printf("%s periodicity (100ms bins, autocorrelation, 5s..end):\n", cfg.name);
        reportPeriodicity("out RMS dB", outTraceDb, 10);
        reportPeriodicity("out-mic dB", [&] {
            // The output trace tracks the material's own level swings; the
            // per-bin suppression depth (out minus mic, in dB) removes that
            // shared component, which is where a periodic *suppression*
            // artifact would show even if the material masks it in raw level.
            std::vector<double> d(numBins);
            for (size_t b = 0; b < numBins; ++b) d[b] = outTraceDb[b] - micTraceDb[b];
            return d;
        }(), 10);
        reportPeriodicity("delay est ms", delayTrace, 10);

        const double outFullRms = fullBandRms(output, skipS);
        const double outHighRms = highBandRms(output, skipS);
        const double fullRedDb = 20.0 * std::log10(std::max(1e-12, micFullRms) / std::max(1e-12, outFullRms));
        const double highRedDb = 20.0 * std::log10(std::max(1e-12, micHighRms) / std::max(1e-12, outHighRms));

        printf("%-22s %11.1f dB %11.1f dB %7d ms\n", cfg.name, fullRedDb, highRedDb, delayMs);
        csv << cfg.name << "," << tailMs[cfg.tailIndex] << "," << cfg.amount << "," << cfg.maxReductionDb
            << "," << cfg.responseMs << "," << juce::String(fullRedDb, 2) << "," << juce::String(highRedDb, 2)
            << "," << delayMs << "\n";

        if (writeOutputs)
            writeWav16("real_out_" + std::string(cfg.name) + ".wav", output, sampleRate);
    }

    csvFile.replaceWithText(csv);
    printf("\nWrote %s%s\n", csvFile.getFullPathName().toRawUTF8(),
           writeOutputs ? " and real_out_<config>.wav files (16-bit, for listening comparison)" : "");
    printf("Note: 'reduction' compares mic vs output level over %.0fs..end -- bleed removal plus any\n"
           "wanted-content suppression together; compare configs against each other, not as absolutes.\n", skipS);
    return allRan ? 0 : 1;
}

// --screenshot <out.png> [--adjusted]: renders the editor to a PNG after
// a few seconds of the synthetic test signals have run through the
// processor, so the meters and status line show a realistic mid-show state.
// For reviewing panel changes without a host; needs a display (or xvfb-run).
int runScreenshotMode(const char* outPath, bool adjusted) {
    juce::ScopedJuceInitialiser_GUI gui;
    const int sampleRate = 48000;
    const int blockSize = 512;
    auto signals = makeSignals(sampleRate, 6.0);

    PAEchoCancellerAudioProcessor proc;
    setMonoLayout(proc);
    proc.prepareToPlay(sampleRate, blockSize);
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;
    const int n = static_cast<int>(signals.mic.size());
    for (int pos = 0; pos + blockSize <= n; pos += blockSize) {
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);
        proc.processBlock(buffer, midi);
        // Let the editor's 30Hz timer see the audio flowing near the end.
        if (pos > n - 40 * blockSize)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }

    // --adjusted moves a suppressor control off its default, so the Defaults
    // button shows enabled.
    if (adjusted)
        proc.getResponseParameter()->setValueNotifyingHost(
            proc.getResponseParameter()->convertTo0to1(35.0f));
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);

    const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
    juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(outPath);
    out.deleteFile();
    juce::FileOutputStream stream(out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream(image, stream);
    printf("%s %s\n", ok ? "Wrote" : "FAILED to write", out.getFullPathName().toRawUTF8());
    editor.reset();
    proc.releaseResources();
    return ok ? 0 : 1;
}

bool testBasicCancellation(int rate, double durationS) {
    {
        printf("\n=== Sample rate: %d Hz ===\n", rate);

        auto signals = makeSignals(rate, durationS);

        PAEchoCancellerAudioProcessor proc;
        PAEchoCancellerAudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::mono());
        layout.inputBuses.add(juce::AudioChannelSet::mono());
        layout.outputBuses.add(juce::AudioChannelSet::mono());
        if (!proc.setBusesLayout(layout)) {
            printf("  FAILED to set mono buses layout\n");
            return false;
        }

        auto output = runThroughProcessor(proc, rate, signals.reference, signals.mic);
        int latency = proc.getLatencySamples();
        double latencyMs = 1000.0 * latency / rate;
        printf("  Reported latency: %d samples (%.2f ms)\n", latency, latencyMs);

        double picS0 = 0.0, picS1 = durationS * (2.0 / 6.0);
        double mic_pa = rmsDbfs(signals.mic, rate, picS0, picS1);
        double cleaned_pa = rmsDbfs(output, rate, picS0, picS1);
        double reductionDb = mic_pa - cleaned_pa;

        double voiceStartS = durationS * (2.25 / 6.0);
        double voiceEndS = durationS * (3.75 / 6.0);
        double voice_gt = rmsDbfs(signals.voice, rate, voiceStartS, voiceEndS);
        double cleaned_voice = rmsDbfs(output, rate, voiceStartS, voiceEndS);
        double voiceErrDb = cleaned_voice - voice_gt;

        printf("  PA-only window: mic=%.1f dBFS cleaned=%.1f dBFS reduction=%.1f dB\n", mic_pa, cleaned_pa, reductionDb);
        printf("  Voice window:   ground_truth=%.1f dBFS cleaned=%.1f dBFS error=%.1f dB\n", voice_gt, cleaned_voice, voiceErrDb);

        // 8 dB rather than the 10 dB the 1.0.x engine was held to: this
        // window (0-2 s) includes the canceller's start-up, and the default
        // Range (-12 dB) caps the suppressor.
        // testSuppressorControls covers what the stronger settings reach.
        bool pass = reductionDb > 8.0 && std::abs(voiceErrDb) < 6.0;
        printf("  %s\n", pass ? "PASS" : "CHECK");

        writeWav16("plugin_verify_" + std::to_string(rate) + "_mic.wav", signals.mic, rate);
        writeWav16("plugin_verify_" + std::to_string(rate) + "_cleaned.wav", output, rate);
        return pass;
    }
}

// --bench-kalman: CPU of the Kalman engine alone, and of its FFT, at 48 kHz.
static int runKalmanBench() {
    const int fs = 48000, seconds = 10;
    KalmanEchoCanceller k;
    k.prepare(fs, 1, 0.8);
    const int N = k.getBlockSize();
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.0f, 0.1f);
    std::vector<float> ref(static_cast<size_t>(N)), mic(static_cast<size_t>(N));
    float* micPtr = mic.data();
    const int blocks = fs * seconds / N;
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < N; ++i) { ref[static_cast<size_t>(i)] = nd(rng); mic[static_cast<size_t>(i)] = 0.3f * ref[static_cast<size_t>(i)] + 0.1f * nd(rng); }
        k.processFrame(&micPtr, ref.data(), 1);
    }
    const double tk = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
    juce::dsp::FFT fft(8);
    std::vector<float> buf(512, 0.0f);
    const int nfft = blocks * 43;
    const auto t1 = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < nfft; ++i) { buf[0] = static_cast<float>(i); fft.performRealOnlyForwardTransform(buf.data(), true); }
    const double tf = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t1);
    printf("Kalman 800 ms, mono: %.3f s for %d s of audio = %.1f%% of one core\n", tk, seconds, 100.0 * tk / seconds);
    printf("  of which ~%d FFTs of 256: %.3f s (%.1f%% of one core)\n", nfft, tf, 100.0 * tf / seconds);
    return 0;
}

// End-to-end latency check, for phase-aligned mixing of ambience mics: the
// delay the plugin actually adds must equal what it reports to the host,
// in every situation a host can put it in -- every sample rate, any block
// size (fixed or varying from call to call), realtime or offline, mono or
// stereo, any Mix, bypassed, and with the PA present so the suppressor is
// working. The mic carries independent white noise (the "crowd") per
// channel; the delay is the lag of the output's cross-correlation peak
// against that noise, and its sign must be positive (no polarity flip).
// The input HPF is minimum-phase, so its impulse response peaks at lag 0
// and does not move the peak.
namespace latencymatrix {
struct Case {
    int rate; int block; bool stereo; float mixPct; bool bypass; bool nonRealtime; bool paActive;
};
struct Result { int reported = -1; int measured[2] = { -1, -1 }; double peakRatio[2] = { 0, 0 }; bool positive[2] = { false, false }; };

Result run(const Case& c, double durationS = 2.0) {
    const int n = static_cast<int>(c.rate * durationS);
    std::mt19937 rngL(101), rngR(202), rngRef(303);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> crowd[2] = { std::vector<float>(static_cast<size_t>(n)), std::vector<float>(static_cast<size_t>(n)) };
    for (auto& v : crowd[0]) v = 0.1f * dist(rngL);
    for (auto& v : crowd[1]) v = 0.1f * dist(rngR);
    std::vector<float> ref(static_cast<size_t>(n), 0.0f), echo(static_cast<size_t>(n), 0.0f);
    if (c.paActive) {
        for (auto& v : ref) v = 0.3f * dist(rngRef);
        // A short room: 5 ms direct path plus a few reflections.
        const int d0 = c.rate / 200;
        const std::pair<int, float> taps[] = { { d0, 0.8f }, { d0 + c.rate / 300, 0.35f }, { d0 + c.rate / 90, -0.2f }, { d0 + c.rate / 40, 0.1f } };
        for (auto [d, g] : taps)
            for (int i = d; i < n; ++i) echo[static_cast<size_t>(i)] += g * ref[static_cast<size_t>(i - d)];
    }

    PAEchoCancellerAudioProcessor proc;
    PAEchoCancellerAudioProcessor::BusesLayout layout;
    const auto mainSet = c.stereo ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
    layout.inputBuses.add(mainSet);
    layout.inputBuses.add(juce::AudioChannelSet::mono());
    layout.outputBuses.add(mainSet);
    proc.setBusesLayout(layout);
    proc.getDryWetMixParameter()->setValueNotifyingHost(proc.getDryWetMixParameter()->convertTo0to1(c.mixPct));
    proc.getBypassParameter()->setValueNotifyingHost(c.bypass ? 1.0f : 0.0f);

    // block > 0: fixed size. block == 0: a varying host-like pattern.
    static const int pattern[] = { 512, 37, 129, 1, 4096, 256, 7, 1000, 64 };
    const int maxBlock = c.block > 0 ? c.block : 4096;
    proc.setNonRealtime(c.nonRealtime);
    proc.prepareToPlay(c.rate, c.block > 0 ? c.block : 512); // pattern also exceeds the prepared size
    Result r;
    r.reported = proc.getLatencySamples();

    const int numCh = c.stereo ? 2 : 1;
    std::vector<float> out[2] = { std::vector<float>(static_cast<size_t>(n), 0.0f), std::vector<float>(static_cast<size_t>(n), 0.0f) };
    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, maxBlock);
    juce::MidiBuffer midi;
    int pos = 0, idx = 0;
    while (pos < n) {
        const int bs = std::min(c.block > 0 ? c.block : pattern[idx++ % 9], n - pos);
        buffer.setSize(totalChannels, bs, false, false, true);
        buffer.clear();
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < numCh; ++ch)
            for (int s = 0; s < bs; ++s)
                mainIn.setSample(ch, s, crowd[ch][static_cast<size_t>(pos + s)] + echo[static_cast<size_t>(pos + s)]);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, ref.data() + pos, bs);
        proc.processBlock(buffer, midi);
        auto mainOut = proc.getBusBuffer(buffer, false, 0);
        for (int ch = 0; ch < numCh; ++ch)
            for (int s = 0; s < bs; ++s) out[ch][static_cast<size_t>(pos + s)] = mainOut.getSample(ch, s);
        pos += bs;
    }
    if (proc.getLatencySamples() != r.reported) r.reported = -2; // must not change while running
    proc.releaseResources();

    // Correlate over a window in the second half, where the canceller has
    // converged and the suppressor is acting on the PA.
    const int win = 8192, start = n - win - 2048;
    for (int ch = 0; ch < numCh; ++ch) {
        double best = 0.0, second = 0.0; int bestLag = -1; double bestSigned = 0.0;
        std::vector<double> xc(1025);
        for (int lag = 0; lag <= 1024; ++lag) {
            double acc = 0.0;
            for (int i = 0; i < win; ++i)
                acc += static_cast<double>(out[ch][static_cast<size_t>(start + i)]) * crowd[ch][static_cast<size_t>(start + i - lag)];
            xc[static_cast<size_t>(lag)] = acc;
            if (std::abs(acc) > best) { best = std::abs(acc); bestLag = lag; bestSigned = acc; }
        }
        for (int lag = 0; lag <= 1024; ++lag)
            if (std::abs(lag - bestLag) > 2) second = std::max(second, std::abs(xc[static_cast<size_t>(lag)]));
        r.measured[ch] = bestLag;
        r.peakRatio[ch] = second > 0 ? 20.0 * std::log10(best / second) : 99.0;
        r.positive[ch] = bestSigned > 0.0;
    }
    return r;
}

// Returns pass; prints one line per case when verbose, else only failures.
bool runMatrix(bool full, bool verbose) {
    std::vector<int> rates = full ? std::vector<int>{ 44100, 48000, 88200, 96000, 176400, 192000 }
                                  : std::vector<int>{ 44100, 48000, 96000 };
    std::vector<int> blocks = full ? std::vector<int>{ 16, 32, 64, 100, 128, 192, 256, 441, 512, 1024, 2048, 0 }
                                   : std::vector<int>{ 32, 128, 192, 512, 0 };
    struct Variant { bool stereo; float mix; bool bypass; bool nonRealtime; bool pa; const char* name; };
    const Variant variants[] = {
        { true, 100.0f, false, false, true, "stereo, Mix 100%, PA on" },
        { true, 100.0f, false, true, true, "stereo, Mix 100%, PA on, offline" },
        { true, 50.0f, false, false, true, "stereo, Mix 50%, PA on" },
        { true, 0.0f, false, false, true, "stereo, Mix 0%, PA on" },
        { false, 100.0f, false, false, true, "mono, Mix 100%, PA on" },
        { true, 100.0f, false, false, false, "stereo, Mix 100%, no PA" },
        { true, 100.0f, true, false, true, "stereo, bypassed" },
        { true, 100.0f, true, true, true, "stereo, bypassed, offline" },
    };
    bool allOk = true;
    int cases = 0, failures = 0;
    for (int rate : rates)
        for (int block : blocks)
            for (const auto& v : variants) {
                const Case c{ rate, block, v.stereo, v.mix, v.bypass, v.nonRealtime, v.pa };
                const auto r = run(c);
                bool ok = r.reported > 0;
                for (int ch = 0; ch < (v.stereo ? 2 : 1); ++ch)
                    ok = ok && r.measured[ch] == r.reported && r.positive[ch] && r.peakRatio[ch] > 6.0;
                ++cases;
                if (!ok) { ++failures; allOk = false; }
                if (verbose || !ok)
                    printf("  %6d Hz  block %-7s %-34s reported %4d  measured %4d/%4d  peak +%.0f dB%s  %s\n", rate,
                           block > 0 ? std::to_string(block).c_str() : "varying", v.name, r.reported, r.measured[0],
                           v.stereo ? r.measured[1] : r.measured[0], r.peakRatio[0],
                           (r.positive[0] && (!v.stereo || r.positive[1])) ? "" : " INVERTED", ok ? "ok" : "MISMATCH");
            }
    printf("  %d cases, %d mismatches\n", cases, failures);
    return allOk;
}
} // namespace latencymatrix

bool testLatencyMatrix() {
    printf("\n=== Measured latency vs reported, across rates, block sizes, Mix, bypass, offline ===\n");
    const bool pass = latencymatrix::runMatrix(false, false);
    printf("  %s\n", pass ? "PASS" : "CHECK -- measured delay differs from getLatencySamples()");
    return pass;
}

int main(int argc, char* argv[]) {
    // --latency-matrix: the full latency sweep (all rates and block sizes),
    // one line per case.
    if (argc >= 2 && std::string(argv[1]) == "--latency-matrix")
        return latencymatrix::runMatrix(true, true) ? 0 : 1;
    if (argc >= 2 && std::string(argv[1]) == "--bench-kalman")
        return runKalmanBench();
    if (argc >= 3 && std::string(argv[1]) == "--screenshot") {
        bool adjusted = false;
        for (int i = 3; i < argc; ++i)
            adjusted |= std::string(argv[i]) == "--adjusted";
        return runScreenshotMode(argv[2], adjusted);
    }

    // --real <mic.wav> <ref.wav>: run the real-material harness instead of
    // the synthetic suite (see the block comment above runRealMaterialMode).
    if (argc >= 2 && std::string(argv[1]) == "--real") {
        if (argc < 4) {
            printf("Usage: %s --real <mic.wav> <reference.wav> [--seconds N] [--write-outputs] [--irregular]\n", argv[0]);
            return 1;
        }
        double maxSeconds = 0.0; // 0 = whole file
        bool writeOutputs = false;
        bool irregularCadence = false; // --irregular: host-realistic varying block sizes (see runRealMaterialMode)
        for (int i = 4; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--seconds" && i + 1 < argc)
                maxSeconds = std::atof(argv[++i]);
            else if (arg == "--write-outputs")
                writeOutputs = true;
            else if (arg == "--irregular")
                irregularCadence = true;
        }
        return runRealMaterialMode(argv[2], argv[3], maxSeconds, writeOutputs, irregularCadence);
    }

    // --bypass-only: just the bypass regression tests, for fast iteration on
    // the bypass path without waiting for the whole suite.
    if (argc >= 2 && std::string(argv[1]) == "--bypass-only") {
        bool ok = true;
        for (int rate : { 44100, 48000, 96000 }) ok = testBypassRawPassthrough(rate) && ok;
        ok = testBypassToggleClickFree(48000) && ok;
        ok = testStateSaveRestore() && ok;
        printf("\n%s\n", ok ? "ALL TESTS PASS" : "SOME TESTS FAILED -- see CHECK above");
        return ok ? 0 : 1;
    }

    printDefaultBusLayout();

    const int rates[] = { 44100, 48000, 96000 };
    const double durationS = 6.0;
    bool allPass = true;

    for (int rate : rates) allPass = testBasicCancellation(rate, durationS) && allPass;

    allPass = testSuppressorControls(48000) && allPass;
    allPass = testClickFreeRoomChange(48000) && allPass;
    allPass = testLargeRoomBenefit(48000) && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 0.0f, "Amount 0%") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 25.0f, "Amount 25%") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 80.0f, "Amount 80%, default") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 100.0f, "Amount 100%") && allPass;
    allPass = testSuppressionMeterTransientAlignment(48000) && allPass;
    allPass = testOfflineVsLiveDynamicHfContent(48000) && allPass;
    allPass = testOfflineVsLiveSpectralShiftAfterWarmup(48000) && allPass;
    allPass = testOfflineRenderUsesCurrentSettingsNotDefaults(48000) && allPass;
    allPass = testHighPassFilterResponse() && allPass;
    for (int rate : rates) allPass = testDryWetAlignment(rate) && allPass;
    for (int rate : rates) allPass = testDryWetCombFiltering(rate) && allPass;
    allPass = testGetLatencySamplesAccuracyAllRates() && allPass;
    allPass = testLatencyMatrix() && allPass;
    allPass = testReferenceMeterIncludesTrim() && allPass;
    allPass = testStateSaveRestore() && allPass;
    allPass = testOldSessionRestore() && allPass;
    for (int rate : rates) allPass = testBypassRawPassthrough(rate) && allPass;
    allPass = testBypassToggleClickFree(48000) && allPass;
    allPass = testReferenceGainTrim() && allPass;
    for (int rate : rates) allPass = testOversizedHostBlock(rate) && allPass;
    for (int rate : rates) allPass = testNonFiniteInputRecovery(rate) && allPass;
    allPass = testLatencyInvariantAcrossTailLengths() && allPass;
    for (int rate : rates) allPass = testLiveTailLengthChangeNonBlocking(rate) && allPass;
    allPass = testFastBounceTailLengthAppliedPromptly(48000) && allPass;
    allPass = testDelayStatsExposed(48000) && allPass;

    printf("\n%s\n", allPass ? "ALL TESTS PASS" : "SOME TESTS FAILED -- see CHECK above");

    benchmarkTailLengthCpuCost(48000);

    return allPass ? 0 : 1;
}
