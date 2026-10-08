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

#include <atomic>
#include <cstdlib>
#include <new>

// Heap allocations counted while gAllocCounting is set: the audio-thread
// checks below switch it on around processBlock to prove the audio path
// never allocates (an allocation can block on a lock inside malloc, and
// over hours of live use a steady trickle of them is how memory grows).
static std::atomic<bool> gAllocCounting{ false };
static std::atomic<uint64_t> gAllocCount{ 0 };

void* operator new(std::size_t size)
{
    if (gAllocCounting.load(std::memory_order_relaxed))
        gAllocCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
// Not inlined, so GCC doesn't see free() meet a pointer from operator new
// at the call sites and warn (-Wmismatched-new-delete) about the pair.
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
// local self-similarity rather than truly modeling it. Tests that actually
// want to distinguish filter lengths should use a lighter/broader reference.
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
// block-size pattern that never lines up with the canceller's frame. If
// tailChangeAtSample >= 0, Tail Length is switched to tailChangeToIndex at
// that point in the stream (mid-processing, as a user turning the knob live
// would trigger), and changeSampleIndex receives the output sample index at
// which the change was actually applied.
std::vector<float> runThroughProcessor(PAEchoCancellerAudioProcessor& proc, int sampleRate,
                                        const std::vector<float>& reference, const std::vector<float>& mic,
                                        int tailChangeAtSample = -1, int tailChangeToIndex = -1,
                                        int* changeSampleIndex = nullptr, bool nonRealtime = false) {
    static const int blockPattern[] = { 512, 37, 129, 1, 4096, 256, 7 };
    const int patternLen = static_cast<int>(sizeof(blockPattern) / sizeof(blockPattern[0]));
    int maxBlock = 0;
    for (int b : blockPattern) maxBlock = std::max(maxBlock, b);

    proc.setNonRealtime(nonRealtime);
    proc.prepareToPlay(sampleRate, maxBlock);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, maxBlock);

    const int n = static_cast<int>(mic.size());
    std::vector<float> output(static_cast<size_t>(n), 0.0f);
    juce::MidiBuffer midi;

    bool tailChangeApplied = false;

    int pos = 0;
    int patternIdx = 0;
    while (pos < n) {
        int blockSize = std::min(blockPattern[patternIdx % patternLen], n - pos);
        patternIdx++;
        if (blockSize <= 0) continue;

        if (!tailChangeApplied && tailChangeAtSample >= 0 && pos >= tailChangeAtSample) {
            // Mimic what a real host does when the user turns the knob:
            // go through the public parameter API, not a test backdoor.
            auto* param = proc.getTailLengthParameter();
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(tailChangeToIndex)));
            tailChangeApplied = true;
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

// Muting the reference (no PA reaching the sidechain) must bring the
// Reduction meter back to ~0 dB. Converges on leak+audience material, then
// zeroes the reference while the audience continues, and polls the
// *editor's* measured-reduction formula (getInputPeakLevelPostDelayed()
// minus getOutputPeakLevel(), floored, matching PluginEditor.cpp's
// timerCallback) to see how long any residual reading persists.
bool testSuppressionMeterAfterReferenceMute(int sampleRate, float amountPercent, const char* amountName) {
    printf("\n=== Reduction meter after reference mute test (%d Hz, %s) ===\n",
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

    // A spread-out, decaying multi-tap room response (5ms direct plus
    // reflections out to 320ms, as testLongTailBenefit) so the adaptive
    // filter has a genuine reverberant tail to learn, as in a real hall.
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
    // Everything else stays at its plugin default (Tail Length 800ms, Range
    // -12 dB, Time 30 ms) -- exactly what a user hears out of the box.
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
            printf("  t=mute+%.2fs  input=%.4f output=%.4f  measured reduction=%.1f dB\n",
                   elapsedSincemuteS, inputLevel, outputLevel, suppressionDb);
            ++nextCheckpoint;
        }
    }
    proc.releaseResources();

    const float finalDb = suppressionAtCheckpoint.empty() ? -1.0f : suppressionAtCheckpoint.back();
    const bool pass = finalDb >= 0.0f && finalDb < 3.0f;
    printf("  %s\n", pass ? "PASS -- measured reduction converges back to ~0dB after the reference goes silent"
                           : "CHECK -- measured reduction stays elevated well after the reference is silent");
    return pass;
}

// A fast transient (kick/snare) with no PA to remove must not show on the
// Reduction meter. Output at any instant reflects Input from
// getLatencySamples() earlier, so comparing it against the *live* Input
// peak makes a sharp attack that hasn't reached the Output yet look
// "reduced" purely from that timing gap. A steady crowd bed (no leak) gets
// one short, loud click injected, and both the naive (live-input) and the
// delay-compensated formula are polled at 128-sample resolution through
// the click: the naive one must spike, the compensated one stay flat.
bool testSuppressionMeterTransientAlignment(int sampleRate) {
    printf("\n=== Suppression meter transient alignment test (%d Hz) ===\n", sampleRate);
    const double totalDurationS = 3.0;
    const double clickAtS = 2.0;
    const int n = static_cast<int>(sampleRate * totalDurationS);
    const int clickSample = static_cast<int>(sampleRate * clickAtS);
    const int clickLenSamples = static_cast<int>(sampleRate * 0.003); // ~3ms sharp attack, like a drum hit

    std::mt19937 audienceRng(33), clickRng(34);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Steady crowd bed, no PA leak at all -- nothing here should ever
    // register as reduced; this isolates the transient-alignment artifact
    // from any real cancellation.
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

    printf("  Worst apparent reduction through the click: naive(live input)=%.1f dB  delay-compensated=%.1f dB\n",
           worstNaiveDb, worstCompensatedDb);

    const bool pass = worstNaiveDb > 6.0f && worstCompensatedDb < 2.0f;
    printf("  %s\n", pass ? "PASS -- delay compensation removes the transient's false reduction reading"
                           : "CHECK -- transient alignment fix isn't behaving as expected");
    return pass;
}

// Confirms that changing Tail Length mid-stream never produces a spike, and
// that it applies live: the canceller keeps the near part of its filter, so
// the output must not drop out either.
bool testClickFreeTailLengthChange(int sampleRate) {
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

    // A dropout would show as a run of exact zeros; the filter must carry
    // on without one.
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

// Confirms a longer Tail Length earns its CPU cost: a room with echo energy
// spread out to ~320ms should cancel better with a 400ms filter than with a
// 50ms one.
bool testLongTailBenefit(int sampleRate) {
    printf("\n=== Long Tail Length benefit test (%d Hz) ===\n", sampleRate);
    const double durationS = 6.0;
    const std::vector<std::pair<double, float>> roomTaps = {
        { 5.0, 0.5f }, { 80.0, 0.3f }, { 180.0, 0.2f }, { 320.0, 0.15f }
    };
    // Lightly-filtered (broadband) reference: distinguishing a 50ms filter
    // from a 400ms one requires a reference that a short filter can't
    // "cheat" on via short-lag self-correlation (see makeSignals comment).
    auto signals = makeSignals(sampleRate, durationS, roomTaps, /*lowpassAlpha*/ 0.3f, /*lowpassStages*/ 1);

    auto runWithTail = [&](int tailIndex) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        auto* param = proc.getTailLengthParameter();
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(tailIndex)));
        return runThroughProcessor(proc, sampleRate, signals.reference, signals.mic);
    };

    auto shortOutput = runWithTail(0); // 50ms
    auto longOutput = runWithTail(2);  // 400ms

    // Voice occupies 2.0-4.0s here, so measure the PA-only tail (4.0-6.0s):
    // past both the canceller's start-up and the voice burst.
    const double picS0 = durationS * (4.0 / 6.0), picS1 = durationS;
    const double mic_pa = rmsDbfs(signals.mic, sampleRate, picS0, picS1);
    const double short_pa = rmsDbfs(shortOutput, sampleRate, picS0, picS1);
    const double long_pa = rmsDbfs(longOutput, sampleRate, picS0, picS1);

    printf("  PA-only window: mic=%.1f dBFS  50ms=%.1f dBFS (%.1f dB reduction)  400ms=%.1f dBFS (%.1f dB reduction)\n",
           mic_pa, short_pa, mic_pa - short_pa, long_pa, mic_pa - long_pa);

    // The suppressor after the filter compensates somewhat for a too-short
    // filter, so a dramatic dB gap isn't the right bar. What matters is
    // that 400ms is better, never worse, on a tail the 50ms filter
    // physically can't reach (echo taps out to 320ms).
    const bool pass = (long_pa < short_pa);
    printf("  %s\n", pass ? "PASS -- 400ms outperforms 50ms on a long room tail (as expected)" : "CHECK");
    return pass;
}

// An offline bounce must sound the same as playback. Offline and live run
// the identical code path, so this guards against any realtime/non-realtime
// special-casing creeping in. Builds PA content that turns on and off in
// segments (songs separated by quiet gaps, with continuous crowd noise
// underneath) and measures HF content in windows across the *whole* file
// for both modes.
bool testOfflineVsLiveDynamicHfContent(int sampleRate) {
    printf("\n=== Offline vs live HF content on dynamic material test (%d Hz) ===\n", sampleRate);
    // Long enough (a few minutes, like a real recording, not a short clip)
    // to expose any *slow-accumulating* divergence between the live and
    // offline paths that a short test would miss entirely.
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

    // Full-bandwidth -- real crowd noise/clapping/cymbals have genuine HF
    // energy, so this gives the
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

    // One window per 10s slice across the whole several-minute file,
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

    // Live and offline run the exact same code path, so this should be
    // near-exact -- a real gap here would mean some realtime/non-realtime
    // special-casing crept in.
    const bool pass = std::abs(worstOffsetDb) < 0.2 && std::abs(lateAvg - earlyAvg) < 0.2;
    printf("  %s\n", pass ? "PASS -- live and offline are behaviorally identical, no drift"
                           : "CHECK -- offline and live diverge on HF content, or drift apart over the file");
    return pass;
}

// The same live/offline parity on harder material: a multi-tap reverberant
// echo path with continuously varying musical dynamics, compared in 1s
// windows from the very start, where the filter is still converging.
bool testOfflineVsLiveReverberantRoom(int sampleRate) {
    printf("\n=== Offline vs live, realistic room + dynamics regression test (%d Hz) ===\n", sampleRate);
    const double durationS = 20.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(51), noiseFloorRng(53), envRng(54);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Broadband-ish reference (lightly filtered, single stage) so a short
    // filter can't "cheat" via short-lag self-correlation -- same rationale
    // as testLongTailBenefit.
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

    // Multi-tap reverberant room response, matching testLongTailBenefit's
    // model -- extended slightly further to stress the default 800ms Tail
    // Length.
    const std::vector<std::pair<double, float>> roomTaps = {
        { 5.0, 0.5f }, { 80.0, 0.3f }, { 180.0, 0.2f }, { 320.0, 0.15f }, { 500.0, 0.1f }
    };

    // Leak only, no crowd -- isolates the leak-cancellation residual.
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
    // Live and offline run the identical code path, so this must be
    // near-zero everywhere, including the first seconds.
    const bool pass = std::abs(worstOffsetDb) < 0.2;
    printf("  %s\n", pass ? "PASS -- offline and live are identical throughout, including the start"
                           : "CHECK -- offline and live diverge");
    return pass;
}

// An offline render must use the *current* (non-default) parameter values,
// not constructor defaults. Sets two deliberately different settings
// (Strength 0% -- the filter alone -- vs. Strength 100% with Range -24 dB)
// *before* prepareToPlay runs, then renders the same material offline both
// ways: if the settings were ignored, both would come out at the defaults
// and match; applied, the strongest setting shows clearly less HF.
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

    printf("  Offline HF (>6kHz) content: Strength 0%%=%.6f  Strength 100%%/-24dB=%.6f  (%.1f dB less)\n",
           hfFilterOnly, hfStrongest, reducedByDb);

    const bool pass = reducedByDb > 3.0;
    printf("  %s\n", pass ? "PASS -- offline render correctly applies non-default settings, not constructor defaults"
                           : "CHECK -- offline render doesn't seem to be picking up non-default settings");
    return pass;
}

// Confirms Mix's dry path is delay-matched against the wet (cancelled)
// path: misaligned by even a few samples, blending them would comb-filter
// the content they share. At Mix 0% (fully dry) the output must equal an
// independently computed HPF'd copy of the mic delayed by exactly
// getLatencySamples(), under runThroughProcessor's irregular block sizes.
bool testDryWetAlignment(int sampleRate) {
    printf("\n=== Mix dry-path alignment test (%d Hz) ===\n", sampleRate);
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

    // Compared from the middle of the file, once the voice bursts are in:
    // makeSignals' default reference is heavily lowpassed, so most of the
    // PA-only part sits below the HPF cutoff and is near-silent after it,
    // where a relative-dB comparison is ill-conditioned. A search around the
    // nominal latency reports where the best match actually is.
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
    // Fixed base for the fine pass, so updating bestLatency doesn't move
    // the window being scanned.
    const int coarseBestLatency = bestLatency;
    for (int offset = -coarseStep; offset <= coarseStep; ++offset) {
        const double db = relativeDiffDbAtLatency(coarseBestLatency + offset);
        if (db < bestDb) { bestDb = db; bestLatency = coarseBestLatency + offset; }
    }

    printf("  Nominal latency=%d, best-matching latency=%d (%+d samples), difference at best: %.1f dB (expect < -60dB)\n",
           nominalLatency, bestLatency, bestLatency - nominalLatency, bestDb);

    // The dry path must match at exactly the reported latency.
    const bool pass = bestDb < -60.0 && bestLatency == nominalLatency;
    printf("  %s\n", pass ? "PASS -- dry path is the HPF'd input at exactly the reported latency"
                           : "CHECK -- dry path is misaligned or differs from the HPF'd input");
    return pass;
}


// Mix at 50% must not comb-filter: an isolated click must land at the same
// sample in a fully wet and a fully dry run. With no reference signal the
// canceller passes the click through, so the "wet" path is just HPF plus
// buffering, isolating the timing question. Uses a small, non-frame-aligned
// host block size like a low-latency live rig.
bool testDryWetCombFiltering(int sampleRate) {
    printf("\n=== Mix comb-filtering test (%d Hz) ===\n", sampleRate);
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

    const bool pass = mismatchSamples == 0;
    printf("  %s\n", pass ? "PASS -- wet and dry paths are sample-accurately aligned"
                          : "CHECK -- wet/dry misalignment found -- this is exactly what causes comb filtering");
    return pass;
}

// Confirms getLatencySamples() is the true input-to-output delay at each
// sample rate, measured end to end: an isolated click through the mic,
// silent reference (so the canceller passes it through), 100% wet, small
// non-frame-aligned block size. Where the click lands is the true delay.
// testLatencyMatrix covers the full sweep of rates, block sizes and modes.
bool testGetLatencySamplesAccuracyAllRates() {
    bool allPass = true;
    for (int sampleRate : { 44100, 48000, 96000 }) {
        printf("\n=== getLatencySamples() accuracy test (%d Hz) ===\n", sampleRate);

        const double durationS = 5.0;
        const int n = static_cast<int>(sampleRate * durationS);
        const int clickSample = static_cast<int>(sampleRate * 3.5);
        const int clickLenSamples = std::max(1, static_cast<int>(sampleRate * 0.001)); // ~1ms sharp click

        std::mt19937 clickRng(81);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::vector<float> mic(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < clickLenSamples; ++i) {
            const int s = clickSample + i;
            if (s >= n) break;
            const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(clickLenSamples);
            mic[static_cast<size_t>(s)] = 0.8f * envelope * dist(clickRng);
        }
        const std::vector<float> reference(static_cast<size_t>(n), 0.0f); // silent: nothing to cancel

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

        // Ground truth is where an independently HPF'd copy of this exact
        // click peaks, not the raw click: the 4th-order input HPF reshapes a
        // short noise click enough to move its loudest sample.
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

        const bool pass = measuredDelay == reportedLatency;
        printf("  %s\n", pass ? "PASS -- getLatencySamples() equals the real delay"
                              : "CHECK -- getLatencySamples() differs from the real delay");
        allPass = allPass && pass;
    }
    return allPass;
}

// PA Reference Trim is a plain linear gain on the reference before the
// canceller sees it, for a feed that is clipping or too quiet. Confirms the
// gain is applied correctly.
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

// JUCE documents prepareToPlay's samplesPerBlock as the *expected* block
// size, not a hard ceiling, and hosts genuinely exceed it -- notably when
// switching between playback and an offline render. Every scratch buffer
// and FIFO is sized against that figure, so processBlock splits oversized
// blocks into chunks (an overrun here was once a real segfault). Checks
// both that it survives AND that the audio is right afterwards -- silently
// emitting zeros would "pass" a crash-only test.
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
// too, and the canceller's adaptive filter downstream would latch it as
// well. Without the guard in HighPassFilterChain a 5ms NaN burst left the
// output 100% non-finite for the whole rest of the run, with no recovery.
// For live use that means one glitched frame from anything upstream silently
// kills an audience mic for the rest of the show. Confirms the plugin both
// survives the burst and fully recovers afterwards.
// hugeFinite: the burst is +/-1e30 instead -- finite, so it passes the
// HPF's NaN guard, but its square overflows inside the canceller.
bool testNonFiniteInputRecovery(int sampleRate, bool hugeFinite = false) {
    printf("\n=== %s input recovery test (%d Hz) ===\n", hugeFinite ? "Huge finite (1e30)" : "Non-finite (NaN/Inf)",
           sampleRate);
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

    // A 5ms burst of both NaN and Inf at t=6s.
    const int burstStart = static_cast<int>(sampleRate * 6.0);
    const int burstLen = static_cast<int>(sampleRate * 0.005);
    for (int i = 0; i < burstLen && burstStart + i < n; ++i)
        mic[static_cast<size_t>(burstStart + i)] =
            hugeFinite ? ((i % 2 == 0) ? 1e30f : -1e30f)
                       : ((i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity());

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

// Changing Tail Length mid-stream must never stall the audio callback: the
// filter only drops or adds partitions, so no single callback may take
// longer than a generous "something is very wrong" threshold, and the
// reported latency must not move.
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
    // 512-sample block is well under 1ms even at 96kHz) -- loose enough not
    // to flake on a loaded machine, tight enough to catch a rebuild or an
    // allocation on the audio thread.
    const bool noStall = maxBlockMsAfterChange < 80.0;
    const bool latencyStable = (latencyAfter == latencyBefore);

    printf("  Change requested at sample %d\n", changeAtSample);
    printf("  Max single processBlock after change: %.2f ms (limit 80ms)\n",
           maxBlockMsAfterChange);
    printf("  Reported latency: %d -> %d samples (%s)\n", latencyBefore, latencyAfter,
           latencyStable ? "unchanged, as designed" : "CHANGED");

    const bool pass = noStall && latencyStable;
    printf("  %s\n", pass ? "PASS -- live tail change applies without blocking the audio thread" : "CHECK");
    return pass;
}

// A host may apply a session's saved Tail Length after prepareToPlay (or
// re-prepare with defaults before a bounce). The filter applies Tail Length
// at the next block, so a bounce with the tail set after prepareToPlay must
// match one with it set before, from the start.
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

// Confirms getStateInformation/setStateInformation round-trip every
// parameter, so a session reload restores what the user dialed in.
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

    printf("  Tail Length=%d Strength=%.1f%% Range=%.1fdB Time=%.1fms HPF=%.1fHz Trim=%.1fdB Mix=%.1f%%\n",
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

    printf("  Tail=%d HPF=%.1fHz Trim=%.1fdB Mix=%.1f%% | Strength=%.1f%% Range=%.1fdB Time=%.1fms\n",
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

// Strength, Range and Time are the suppressor's whole interface, so pin
// down what each one means on the synthetic material:
//  - Strength 0% is the adaptive filter alone, and so is Range 0 dB at any
//    Strength (the suppressor may not cut anywhere): the two outputs must be
//    identical.
//  - More Strength removes more PA (25% < 100% in the PA-only window).
//  - A deeper Range lets 100% go further than a shallow one.
//  - Every setting applies live: changing all three mid-stream must not
//    produce a spike or a dropout.
bool testSuppressorControls(int sampleRate) {
    printf("\n=== Bleed suppressor Strength / Range / Time test (%d Hz) ===\n", sampleRate);
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
    printf("  PA-only reduction: filter only %.1f dB, Strength 25%% %.1f dB, 100%% %.1f dB; "
           "100%% at -3 dB %.1f dB, at -24 dB %.1f dB\n",
           red(filterOnly), red(mid), red(full), red(fullShallow), red(fullDeep));
    printf("  Max difference, Strength 0%% vs Range 0 dB: %.3g\n", maxDiff);

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
           amountWorks ? "" : " -- STRENGTH NOT MONOTONIC", floorWorks ? "" : " -- RANGE HAS NO EFFECT",
           liveClean ? "" : " -- LIVE CHANGE GLITCHED");
    return pass;
}

// Bypass check (a): with bypass engaged, the output must be the TRULY raw
// input -- HPF included in "unmodified", so content below the 150Hz cutoff
// must survive -- delayed by exactly the reported latency, while the
// canceller (live reference, real leakage in the mic) runs underneath. The dry-path tap can't provide this (it's post-HPF), which
// is exactly what this test would catch if the bypass were wired there: the
// comparison against the raw signal fails loudly on the missing 40Hz
// content, while the post-HPF control comparison below confirms the test
// could tell the difference.
bool testBypassRawPassthrough(int sampleRate) {
    printf("\n=== Bypass raw passthrough test (%d Hz) ===\n", sampleRate);
    const double durationS = 4.0;
    const int n = static_cast<int>(sampleRate * durationS);

    // Reference: broadband-ish noise, leaked into the mic at 5ms/0.5 so
    // the canceller has genuine echo to chase -- bypass must pass the input through
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

    // The bypass path moves samples through FIFOs by plain copy, so at the
    // reported delay the match must be exact, not merely close. A search
    // around it reports where the best match actually is.
    const int searchMargin = 512;
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
    for (int d = std::max(0, nominalLatency - searchMargin); d <= nominalLatency + searchMargin; ++d) {
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
    printf("  Same comparison vs post-HPF input (control, must be large): RMS=%.4f\n", hpfRms);

    const bool pass = bestRms < 1.0e-6                      // exact raw passthrough (float-copy exact)
                       && bestDelay == nominalLatency        // at exactly the reported latency
                       && hpfRms > 0.05;                     // and provably raw, not post-HPF
    printf("  %s\n", pass ? "PASS -- bypass is a latency-matched, truly-raw passthrough"
                          : "CHECK -- bypass output is not the raw input at the matched delay");
    return pass;
}

// Bypass check (b): toggling bypass mid-stream, both directions, produces
// no click/spike -- same no-spike detection pattern as
// testClickFreeTailLengthChange -- and actually takes effect (bypassed span
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
    // toggling back it must NOT (canceller + HPF are at work again).
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
    // Find the actual delay in the bypassed span the same way the
    // raw-passthrough test does (it must be the reported latency), then hold
    // the processed-span comparison to that same delay.
    const int searchMargin = 512;
    const int bypSpanStart = 3 * sampleRate, bypSpanEnd = 4 * sampleRate - sampleRate / 10;
    int bestDelay = nominalLatency;
    double bypassedDiff = rmsDiffVsRaw(bypSpanStart, bypSpanEnd, nominalLatency);
    for (int d = std::max(0, nominalLatency - searchMargin); d <= nominalLatency + searchMargin; ++d) {
        const double r = rmsDiffVsRaw(bypSpanStart, bypSpanEnd, d);
        if (r < bypassedDiff) { bypassedDiff = r; bestDelay = d; }
    }
    const double processedDiff = rmsDiffVsRaw(5 * sampleRate, n, bestDelay);

    printf("  Input peak: %.4f   Output peak: %.4f   (spike threshold %.4f)\n",
           inputPeak, outputPeak, inputPeak * 1.5f);
    printf("  RMS diff vs raw input at delay %d (nominal %d): bypassed span=%.4f (expect ~0), processed span=%.4f (expect >>0)\n",
           bestDelay, nominalLatency, bypassedDiff, processedDiff);

    const bool noSpike = spikeIndex < 0;
    const bool tookEffect = bestDelay == nominalLatency && bypassedDiff < 0.02
                            && processedDiff > 10.0 * std::max(bypassedDiff, 1e-6);
    const bool pass = noSpike && tookEffect;
    printf("  %s%s%s\n", pass ? "PASS -- bypass toggles click-free and actually engages/disengages" : "CHECK",
           noSpike ? "" : " -- SPIKE DETECTED", tookEffect ? "" : " -- BYPASS DID NOT ENGAGE/DISENGAGE AS EXPECTED");
    return pass;
}

// Confirms the HPF is a genuine 4th-order (24dB/octave) Butterworth
// response -- not, say, a single 2nd-order (12dB/octave) section or two
// identical (non-Butterworth-aligned) stages -- by checking measured
// attenuation against the textbook Butterworth formula
// |H|^2 = 1 / (1 + (fc/f)^(2n)) at a few key points relative to cutoff.
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
// objective per-config metrics, so tuning experiments (Strength, Range,
// Time, Tail Length) can be measured on the material that matters instead
// of judged by ear alone.
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
    // The defaults, then each suppressor control swept on its own (Range
    // and Time at 50% Strength, where they have room to act), then the
    // defaults at a shorter tail.
    const Config configs[] = {
        { 3, 80.0f, -12.0f, 30.0f, "default" },
        { 3, 0.0f, -12.0f, 30.0f, "strength0" },
        { 3, 50.0f, -12.0f, 30.0f, "strength50" },
        { 3, 100.0f, -12.0f, 30.0f, "strength100" },
        { 3, 50.0f, -6.0f, 30.0f, "strength50_range6" },
        { 3, 50.0f, -24.0f, 30.0f, "strength50_range24" },
        { 3, 50.0f, -12.0f, 5.0f, "strength50_time5" },
        { 3, 50.0f, -12.0f, 50.0f, "strength50_time50" },
        { 1, 80.0f, -12.0f, 30.0f, "tail200_default" },
    };

    // The irregular run writes its aggregate report to a separate file so a
    // cadence comparison never clobbers the uniform baseline it's being
    // compared against.
    juce::File csvFile(juce::File::getCurrentWorkingDirectory().getChildFile(
        irregularCadence ? "real_material_report_irregular.csv" : "real_material_report.csv"));
    juce::String csv("config,tail_ms,strength_pct,range_db,time_ms,fullband_reduction_db,highband_reduction_db,delay_ms\n");

    printf("\n%-22s %14s %14s %10s\n", "config", "full-band red.", ">=2kHz red.", "delay est.");
    static constexpr int tailMs[] = { 50, 200, 400, 800 };
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
        // pattern runThroughProcessor uses for the synthetic suite. Uniform
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
        }

        const int delayMs = proc.getEstimatedEchoPathDelayMs();
        proc.releaseResources();

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
    return 0;
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

        // 8 dB: this window (0-2 s) includes the canceller's start-up, and
        // the default Range (-12 dB) caps the suppressor.
        // testSuppressorControls covers what the stronger settings reach.
        bool pass = reductionDb > 8.0 && std::abs(voiceErrDb) < 6.0;
        printf("  %s\n", pass ? "PASS" : "CHECK");
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

// Logic and MainStage keep an AU's sidechain bus active when Side Chain is
// None and feed it the track's own input. That copy must count as no
// reference: output identical to a silent Reference bus, and the flag the
// editor shows set. A real PA reference must not trip it, and a stereo
// input folded to mono (L+R)/2 must be caught too.
bool testReferenceIsCopyOfMainInput(int sampleRate) {
    printf("\n=== Reference is a copy of the main input (Logic Side Chain: None) @ %d Hz ===\n", sampleRate);
    const auto signals = makeSignals(sampleRate, 6.0, { { 5.0, 0.6f }, { 22.0, 0.3f } });
    const auto& reference = signals.reference;
    const auto& mic = signals.mic;
    const std::vector<float> silence(mic.size(), 0.0f);

    PAEchoCancellerAudioProcessor procCopy, procSilent, procReal;
    const auto outCopy = runThroughProcessor(procCopy, sampleRate, mic, mic);
    const bool copyFlagged = procCopy.isReferenceCopyOfMainInput();
    const auto outSilent = runThroughProcessor(procSilent, sampleRate, silence, mic);
    runThroughProcessor(procReal, sampleRate, reference, mic);
    const bool realFlagged = procReal.isReferenceCopyOfMainInput();

    float maxDiff = 0.0f;
    for (size_t i = 0; i < outCopy.size(); ++i)
        maxDiff = std::max(maxDiff, std::abs(outCopy[i] - outSilent[i]));

    // Stereo input with different channels, Reference = (L+R)/2.
    bool foldFlagged = false;
    {
        PAEchoCancellerAudioProcessor proc;
        const int blockSize = 256;
        proc.prepareToPlay(sampleRate, blockSize);
        juce::AudioBuffer<float> buffer(std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()), blockSize);
        juce::MidiBuffer midi;
        for (int pos = 0; pos + blockSize <= static_cast<int>(mic.size()); pos += blockSize) {
            buffer.clear();
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int s = 0; s < blockSize; ++s) {
                const float l = mic[static_cast<size_t>(pos + s)];
                const float r = 0.5f * reference[static_cast<size_t>(pos + s)];
                mainIn.setSample(0, s, l);
                mainIn.setSample(1, s, r);
                refIn.setSample(0, s, 0.5f * (l + r));
            }
            proc.processBlock(buffer, midi);
        }
        foldFlagged = proc.isReferenceCopyOfMainInput();
        proc.releaseResources();
    }

    printf("  Copy flagged=%d  real PA flagged=%d  (L+R)/2 flagged=%d  max |copy - silent ref| = %.3g\n",
           copyFlagged, realFlagged, foldFlagged, static_cast<double>(maxDiff));
    const bool pass = copyFlagged && !realFlagged && foldFlagged && juce::exactlyEqual(maxDiff, 0.0f);
    printf("  %s\n", pass ? "PASS -- a copy of the input is treated as no reference" : "FAIL");
    return pass;
}

// Hours of live use must not grow memory: everything is allocated in
// prepareToPlay, so processBlock must never touch the heap, whatever the
// host does -- every control automated on every block (Tail Length, HPF,
// Strength/Range/Time, Trim, Mix, Bypass), and host blocks that are
// smaller than a frame, off the frame grid and larger than promised.
bool testProcessBlockNeverAllocates(int sampleRate) {
    printf("\n=== No heap allocation in processBlock, every control moving (%d Hz) ===\n", sampleRate);
    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    const int preparedBlock = 128;
    proc.prepareToPlay(sampleRate, preparedBlock);
    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, 4 * preparedBlock);
    juce::MidiBuffer midi;
    auto params = proc.getParameters();

    std::mt19937 rng(17);
    std::uniform_real_distribution<float> audio(-0.5f, 0.5f), unit(0.0f, 1.0f);
    static const int pattern[] = { 128, 37, 1, 129, 512, 64 }; // 512 is over the promised 128
    uint64_t allocations = 0;
    const int numBlocks = static_cast<int>(20.0 * sampleRate / 128);
    for (int b = 0; b < numBlocks; ++b) {
        const int bs = pattern[b % 6];
        buffer.setSize(totalChannels, bs, false, false, true);
        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int s = 0; s < bs; ++s) {
            const float r = audio(rng);
            refIn.setSample(0, s, r);
            mainIn.setSample(0, s, 0.4f * r + 0.1f * audio(rng));
        }
        // Like a host's automation, set between blocks (hosts set parameters
        // from their own threads); processBlock then applies the change.
        if (b > numBlocks / 4)
            params[static_cast<int>(static_cast<size_t>(b) % static_cast<size_t>(params.size()))]
                ->setValueNotifyingHost(unit(rng));

        gAllocCount.store(0, std::memory_order_relaxed);
        gAllocCounting.store(true, std::memory_order_relaxed);
        proc.processBlock(buffer, midi);
        gAllocCounting.store(false, std::memory_order_relaxed);
        allocations += gAllocCount.load(std::memory_order_relaxed);
    }
    proc.releaseResources();

    printf("  %d blocks, heap allocations inside processBlock: %llu\n", numBlocks,
           static_cast<unsigned long long>(allocations));
    const bool pass = allocations == 0;
    printf("  %s\n", pass ? "PASS -- the audio path never allocates" : "CHECK -- processBlock allocated");
    return pass;
}

// A session file (or preset) with a NaN, an infinity or an out-of-range
// value must never reach the DSP as one: a NaN Strength or Range would
// make the suppressor's gains NaN, which latches in its smoothing and
// silences the mic for the rest of the show.
bool testCorruptStateRestore() {
    printf("\n=== Corrupt saved state is sanitised on restore ===\n");
    const double badValues[] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(), 1e30, -1e30, 7.0, -3.0 };
    bool pass = true;
    for (double bad : badValues) {
        PAEchoCancellerAudioProcessor proc;
        if (!setMonoLayout(proc)) {
            printf("  FAILED to set mono layout\n");
            return false;
        }
        juce::ValueTree state("PAEchoCancellerState");
        for (auto* param : proc.getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
                state.setProperty(withId->paramID, bad, nullptr);
        juce::MemoryOutputStream stream;
        state.writeToStream(stream);
        proc.setStateInformation(stream.getData(), static_cast<int>(stream.getDataSize()));

        bool paramsOk = true;
        for (auto* param : proc.getParameters()) {
            const float v = param->getValue();
            paramsOk = paramsOk && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
        }

        const int sampleRate = 48000, blockSize = 256;
        proc.prepareToPlay(sampleRate, blockSize);
        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        juce::MidiBuffer midi;
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> audio(-0.5f, 0.5f);
        int nonFinite = 0;
        double outSq = 0.0;
        for (int b = 0; b < 2 * sampleRate / blockSize; ++b) {
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int s = 0; s < blockSize; ++s) {
                const float r = audio(rng);
                refIn.setSample(0, s, r);
                mainIn.setSample(0, s, 0.4f * r + 0.1f * audio(rng));
            }
            proc.processBlock(buffer, midi);
            for (int s = 0; s < blockSize; ++s) {
                const float v = proc.getBusBuffer(buffer, false, 0).getSample(0, s);
                if (!std::isfinite(v)) ++nonFinite;
                else outSq += static_cast<double>(v) * v;
            }
        }
        proc.releaseResources();
        const bool ok = paramsOk && nonFinite == 0 && outSq > 0.0;
        printf("  saved value %-8g: parameters %s, output %s\n", bad, paramsOk ? "in range" : "OUT OF RANGE",
               nonFinite > 0 ? "NON-FINITE" : (outSq > 0.0 ? "finite" : "SILENT"));
        pass = pass && ok;
    }

    // Damaged files, not just damaged values: every truncation of a valid
    // state, then random bytes, and random bytes behind a valid header.
    {
        PAEchoCancellerAudioProcessor source;
        juce::MemoryBlock valid;
        source.getStateInformation(valid);
        PAEchoCancellerAudioProcessor proc;
        const auto inRange = [&proc] {
            for (auto* param : proc.getParameters())
                if (!(param->getValue() >= 0.0f && param->getValue() <= 1.0f))
                    return false;
            return true;
        };
        bool fuzzOk = true;
        for (size_t len = 0; len <= valid.getSize(); ++len) {
            proc.setStateInformation(valid.getData(), static_cast<int>(len));
            fuzzOk = fuzzOk && inRange();
        }
        std::mt19937 rng(11);
        std::vector<uint8_t> blob;
        for (int i = 0; i < 4000; ++i) {
            blob.resize(static_cast<size_t>(rng() % 512));
            for (auto& byte : blob) byte = static_cast<uint8_t>(rng());
            if (i % 2 == 1) // keep the real header so the parser gets further in
                std::copy_n(static_cast<const uint8_t*>(valid.getData()), std::min(blob.size(), size_t{ 24 }), blob.begin());
            proc.setStateInformation(blob.data(), static_cast<int>(blob.size()));
            fuzzOk = fuzzOk && inRange();
        }
        printf("  %zu truncations and 4000 random states: %s\n", valid.getSize() + 1,
               fuzzOk ? "no crash, parameters in range" : "PARAMETERS OUT OF RANGE");
        pass = pass && fuzzOk;
    }
    printf("  %s\n", pass ? "PASS -- corrupt values are ignored or clamped, audio keeps flowing"
                          : "CHECK -- a corrupt saved value reached the DSP");
    return pass;
}

// ---------------------------------------------------------------------------
// --soak: hours of live-style processing on real recordings, looped, at a
// 128-sample host block, with the process's resident memory, heap
// allocations inside processBlock, non-finite output and the PA reduction
// reported for every pass through the material. The files are streamed
// from disk a chunk at a time so the harness itself holds no growing state.
// ---------------------------------------------------------------------------

double residentMegabytes() {
#if JUCE_LINUX
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
        if (line.rfind("VmRSS:", 0) == 0)
            return std::atof(line.c_str() + 6) / 1024.0;
#endif
    return -1.0; // not measured on this platform; the allocation count still is
}

int runSoakMode(const char* refPath, const char* micPath, const char* mic2Path, double hours) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> refReader(formats.createReaderFor(juce::File(juce::String(refPath))));
    std::unique_ptr<juce::AudioFormatReader> micReader(formats.createReaderFor(juce::File(juce::String(micPath))));
    std::unique_ptr<juce::AudioFormatReader> mic2Reader(
        mic2Path != nullptr ? formats.createReaderFor(juce::File(juce::String(mic2Path))) : nullptr);
    if (refReader == nullptr || micReader == nullptr || (mic2Path != nullptr && mic2Reader == nullptr)) {
        printf("ERROR: cannot read the input files\n");
        return 1;
    }
    const int sampleRate = static_cast<int>(refReader->sampleRate);
    int64_t loopLength = std::min<int64_t>(refReader->lengthInSamples, micReader->lengthInSamples);
    if (mic2Reader != nullptr)
        loopLength = std::min<int64_t>(loopLength, mic2Reader->lengthInSamples);
    const int numMics = mic2Reader != nullptr ? 2 : 1;

    PAEchoCancellerAudioProcessor proc;
    PAEchoCancellerAudioProcessor::BusesLayout layout;
    const auto micSet = numMics == 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
    layout.inputBuses.add(micSet);
    layout.inputBuses.add(juce::AudioChannelSet::mono());
    layout.outputBuses.add(micSet);
    if (!proc.setBusesLayout(layout)) {
        printf("ERROR: layout refused\n");
        return 1;
    }
    const int hostBlock = 128;
    proc.prepareToPlay(sampleRate, hostBlock);
    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, hostBlock);
    juce::MidiBuffer midi;

    const int chunk = 64 * hostBlock;
    juce::AudioBuffer<float> refChunk(1, chunk), micChunk(2, chunk);

    const int64_t total = static_cast<int64_t>(hours * 3600.0 * sampleRate);
    printf("=== Soak: %.2f h of live processing, %d mic channel(s), %d Hz, %d-sample blocks, %.0f s loop ===\n",
           hours, numMics, sampleRate, hostBlock, static_cast<double>(loopLength) / sampleRate);
    printf("%6s %9s %10s %12s %10s %13s %9s %8s\n", "pass", "audio", "RSS (MB)", "allocations", "non-finite",
           "level drop", "delay", "speed");

    const double rssStart = residentMegabytes();
    double rssAfterFirstPass = -1.0, rssMax = rssStart;
    uint64_t allocationsTotal = 0, nonFiniteTotal = 0;
    std::vector<double> reductions;
    const auto wallStart = std::chrono::steady_clock::now();
    auto passWallStart = wallStart;

    int64_t done = 0, posInLoop = 0;
    double micSq = 0.0, outSq = 0.0;
    uint64_t passAllocations = 0, passNonFinite = 0;
    int pass = 1;
    while (done < total) {
        const int n = static_cast<int>(std::min<int64_t>({ static_cast<int64_t>(chunk), loopLength - posInLoop, total - done }));
        refReader->read(&refChunk, 0, n, posInLoop, true, false);
        micReader->read(&micChunk, 0, n, posInLoop, true, false);
        if (mic2Reader != nullptr)
            mic2Reader->read(micChunk.getArrayOfWritePointers() + 1, 1, posInLoop, n);

        for (int off = 0; off < n; off += hostBlock) {
            const int bs = std::min(hostBlock, n - off);
            buffer.setSize(totalChannels, bs, false, false, true);
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int ch = 0; ch < numMics; ++ch) {
                mainIn.copyFrom(ch, 0, micChunk, ch, off, bs);
                for (int s = 0; s < bs; ++s) {
                    const double v = micChunk.getSample(ch, off + s);
                    micSq += v * v;
                }
            }
            refIn.copyFrom(0, 0, refChunk, 0, off, bs);

            gAllocCount.store(0, std::memory_order_relaxed);
            gAllocCounting.store(true, std::memory_order_relaxed);
            proc.processBlock(buffer, midi);
            gAllocCounting.store(false, std::memory_order_relaxed);
            passAllocations += gAllocCount.load(std::memory_order_relaxed);

            auto mainOut = proc.getBusBuffer(buffer, false, 0);
            for (int ch = 0; ch < numMics; ++ch)
                for (int s = 0; s < bs; ++s) {
                    const float v = mainOut.getSample(ch, s);
                    if (!std::isfinite(v)) ++passNonFinite;
                    else outSq += static_cast<double>(v) * v;
                }
        }
        done += n;
        posInLoop += n;

        if (posInLoop >= loopLength || done >= total) {
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(now - passWallStart).count();
            passWallStart = now;
            const double rss = residentMegabytes();
            rssMax = std::max(rssMax, rss);
            if (pass == 1) rssAfterFirstPass = rss;
            const double reductionDb = 10.0 * std::log10(std::max(1e-30, micSq) / std::max(1e-30, outSq));
            reductions.push_back(reductionDb);
            const double audioS = static_cast<double>(done) / sampleRate;
            printf("%6d %5d:%02d:%02d %10.1f %12llu %10llu %10.2f dB %6d ms %7.0fx\n", pass,
                   static_cast<int>(audioS / 3600), static_cast<int>(audioS / 60) % 60, static_cast<int>(audioS) % 60,
                   rss, static_cast<unsigned long long>(passAllocations), static_cast<unsigned long long>(passNonFinite),
                   reductionDb, proc.getEstimatedEchoPathDelayMs(),
                   static_cast<double>(posInLoop) / sampleRate / std::max(1e-9, wall));
            fflush(stdout);
            allocationsTotal += passAllocations;
            nonFiniteTotal += passNonFinite;
            passAllocations = passNonFinite = 0;
            micSq = outSq = 0.0;
            posInLoop = 0;
            ++pass;
        }
    }
    proc.releaseResources();

    // Pass 1 includes the start-up (learning from zero); from pass 2 on the
    // material repeats, so the reduction should repeat too.
    double reductionSpread = 0.0;
    for (size_t i = 1; i < reductions.size(); ++i)
        reductionSpread = std::max(reductionSpread, std::abs(reductions[i] - reductions[std::min<size_t>(1, reductions.size() - 1)]));
    const double rssGrowth = rssAfterFirstPass >= 0.0 ? rssMax - rssAfterFirstPass : 0.0;
    printf("\nRSS start %.1f MB, after pass 1 %.1f MB, peak %.1f MB (growth after pass 1: %.2f MB)\n", rssStart,
           rssAfterFirstPass, rssMax, rssGrowth);
    printf("Allocations in processBlock: %llu; non-finite output samples: %llu; reduction spread after pass 1: %.3f dB\n",
           static_cast<unsigned long long>(allocationsTotal), static_cast<unsigned long long>(nonFiniteTotal),
           reductionSpread);
    const bool ok = allocationsTotal == 0 && nonFiniteTotal == 0 && rssGrowth < 1.0 && reductionSpread < 0.5;
    printf("%s\n", ok ? "PASS -- flat memory, no allocations, steady cancellation" : "CHECK -- see above");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --thread-stress: the host's threads at once, for ThreadSanitizer. One
// thread plays the audio callback; another plays the message thread: it
// polls every getter the editor reads, moves every parameter and saves and
// restores the state, as hosts do while audio runs. Build with
// -fsanitize=thread; the run itself only checks the output stays finite.
// ---------------------------------------------------------------------------
int runThreadStressMode(double seconds) {
    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("ERROR: layout refused\n");
        return 1;
    }
    const int sampleRate = 48000, blockSize = 128;
    proc.prepareToPlay(sampleRate, blockSize);
    std::atomic<bool> running{ true };
    std::atomic<uint64_t> nonFinite{ 0 }, blocks{ 0 };

    std::thread audio([&] {
        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, blockSize);
        juce::MidiBuffer midi;
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> audioDist(-0.5f, 0.5f);
        while (running.load()) {
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int s = 0; s < blockSize; ++s) {
                const float r = audioDist(rng);
                refIn.setSample(0, s, r);
                mainIn.setSample(0, s, 0.4f * r + 0.1f * audioDist(rng));
            }
            proc.processBlock(buffer, midi);
            for (int s = 0; s < blockSize; ++s)
                if (!std::isfinite(proc.getBusBuffer(buffer, false, 0).getSample(0, s)))
                    nonFinite.fetch_add(1);
            blocks.fetch_add(1);
        }
    });

    std::mt19937 rng(2);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    float sink = 0.0f;
    int round = 0;
    while (std::chrono::steady_clock::now() < end) {
        sink += proc.getInputPeakLevelPre() + proc.getInputPeakLevelPost() + proc.getSidechainPeakLevelPre()
              + proc.getSidechainPeakLevelPost() + proc.getOutputPeakLevel()
              + proc.getInputPeakLevelPostDelayed(proc.getLatencySamples())
              + static_cast<float>(proc.getEstimatedEchoPathDelayMs() + static_cast<int>(proc.getProcessBlockCallCount() & 1u)
                                   + (proc.isReferenceCopyOfMainInput() ? 1 : 0));
        auto params = proc.getParameters();
        params[round % params.size()]->setValueNotifyingHost(unit(rng));
        if (round % 50 == 0) {
            juce::MemoryBlock state;
            proc.getStateInformation(state);
            proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        ++round;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    running.store(false);
    audio.join();
    proc.releaseResources();
    printf("Thread stress: %llu audio blocks, %d message-thread rounds, non-finite output samples: %llu (readout sum %g)\n",
           static_cast<unsigned long long>(blocks.load()), round, static_cast<unsigned long long>(nonFinite.load()),
           static_cast<double>(sink));
    return nonFinite.load() == 0 ? 0 : 1;
}

} // namespace

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

    // --soak <ref> <mic> [--mic2 <mic>] [--hours H]: hours of live-style
    // processing on looped real recordings, with memory and allocations
    // measured (see runSoakMode).
    if (argc >= 4 && std::string(argv[1]) == "--soak") {
        const char* mic2 = nullptr;
        double hours = 1.0;
        for (int i = 4; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--mic2" && i + 1 < argc)
                mic2 = argv[++i];
            else if (arg == "--hours" && i + 1 < argc)
                hours = std::atof(argv[++i]);
        }
        return runSoakMode(argv[2], argv[3], mic2, hours);
    }

    if (argc >= 2 && std::string(argv[1]) == "--thread-stress")
        return runThreadStressMode(argc >= 3 ? std::atof(argv[2]) : 10.0);

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
    allPass = testClickFreeTailLengthChange(48000) && allPass;
    allPass = testLongTailBenefit(48000) && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 0.0f, "Strength 0%") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 25.0f, "Strength 25%") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 80.0f, "Strength 80%, default") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 100.0f, "Strength 100%") && allPass;
    allPass = testSuppressionMeterTransientAlignment(48000) && allPass;
    allPass = testOfflineVsLiveDynamicHfContent(48000) && allPass;
    allPass = testOfflineVsLiveReverberantRoom(48000) && allPass;
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
    for (int rate : rates) allPass = testReferenceIsCopyOfMainInput(rate) && allPass;
    for (int rate : rates) allPass = testOversizedHostBlock(rate) && allPass;
    for (int rate : rates) allPass = testNonFiniteInputRecovery(rate) && allPass;
    for (int rate : rates) allPass = testNonFiniteInputRecovery(rate, true) && allPass;
    allPass = testLatencyInvariantAcrossTailLengths() && allPass;
    for (int rate : rates) allPass = testLiveTailLengthChangeNonBlocking(rate) && allPass;
    allPass = testFastBounceTailLengthAppliedPromptly(48000) && allPass;
    allPass = testDelayStatsExposed(48000) && allPass;
    for (int rate : rates) allPass = testProcessBlockNeverAllocates(rate) && allPass;
    allPass = testCorruptStateRestore() && allPass;

    printf("\n%s\n", allPass ? "ALL TESTS PASS" : "SOME TESTS FAILED -- see CHECK above");

    benchmarkTailLengthCpuCost(48000);

    return allPass ? 0 : 1;
}
