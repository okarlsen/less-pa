// Standalone harness that drives PAEchoCancellerAudioProcessor directly
// (bypassing the AU/VST3 host wrapper) to prove the FIFO frame-accumulator
// bridges arbitrary/irregular host block sizes into AEC3's fixed 10ms
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

// Drives a raw webrtc::AudioProcessing instance directly with the given
// config, bypassing PAEchoCancellerAudioProcessor entirely. Needed for
// comparisons against configs the plugin itself can no longer produce (e.g.
// pre-fix dominant_nearend_detection thresholds) -- fixed 10ms frames only,
// no irregular-block-size bridging, since that's not what's being tested
// here.
std::vector<float> runRawAec3(const webrtc::EchoCanceller3Config& config, int sampleRate,
                               const std::vector<float>& reference, const std::vector<float>& mic) {
    auto apm = webrtc::AudioProcessingBuilder()
                   .SetEchoControlFactory(std::make_unique<TailLengthEchoControlFactory>(config))
                   .Create();
    webrtc::AudioProcessing::Config apmConfig;
    apmConfig.echo_canceller.enabled = true;
    apmConfig.echo_canceller.mobile_mode = false;
    apmConfig.gain_controller1.enabled = false;
    apmConfig.gain_controller2.enabled = false;
    apmConfig.high_pass_filter.enabled = false;
    apmConfig.noise_suppression.enabled = false;
    apm->ApplyConfig(apmConfig);

    const int frameSize = sampleRate / 100;
    const int n = static_cast<int>(mic.size());
    std::vector<float> output(static_cast<size_t>(n), 0.0f);
    std::vector<float> refFrame(static_cast<size_t>(frameSize));
    std::vector<float> micFrame(static_cast<size_t>(frameSize));
    float* refPtr = refFrame.data();
    float* micPtr = micFrame.data();
    const webrtc::StreamConfig streamConfig(sampleRate, 1);

    int pos = 0;
    while (pos + frameSize <= n) {
        std::copy_n(reference.data() + pos, frameSize, refFrame.data());
        std::copy_n(mic.data() + pos, frameSize, micFrame.data());
        apm->ProcessReverseStream(&refPtr, streamConfig, streamConfig, &refPtr);
        apm->ProcessStream(&micPtr, streamConfig, streamConfig, &micPtr);
        std::copy_n(micFrame.data(), frameSize, output.data() + pos);
        pos += frameSize;
    }
    return output;
}

bool setMonoLayout(PAEchoCancellerAudioProcessor& proc) {
    PAEchoCancellerAudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::mono());
    layout.inputBuses.add(juce::AudioChannelSet::mono());
    layout.outputBuses.add(juce::AudioChannelSet::mono());
    return proc.setBusesLayout(layout);
}

// Confirms AEC3's ERLE (echo_return_loss_enhancement) stat actually updates
// in real time rather than sitting stale, before we trust it for a live
// suppression meter. Runs ~15s (well past AEC3's 2.5s initial phase) with a
// simple fixed 512-sample block size, polling getSuppressionDb() ~1x/second.
bool testSuppressionMeterUpdates(int sampleRate) {
    printf("\n=== Suppression meter (ERLE) update test (%d Hz) ===\n", sampleRate);
    const double durationS = 15.0;
    auto signals = makeSignals(sampleRate, durationS);

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }

    const int blockSize = 512;
    proc.prepareToPlay(sampleRate, blockSize);

    const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer(totalChannels, blockSize);
    juce::MidiBuffer midi;

    const int n = static_cast<int>(signals.mic.size());
    const int pollEvery = sampleRate; // ~once per second
    int nextPoll = 0;
    std::vector<float> polledDb;

    int pos = 0;
    while (pos + blockSize <= n) {
        buffer.setSize(totalChannels, blockSize, false, false, true);
        buffer.clear();

        auto mainIn = proc.getBusBuffer(buffer, true, 0);
        auto refIn = proc.getBusBuffer(buffer, true, 1);
        for (int ch = 0; ch < mainIn.getNumChannels(); ++ch)
            mainIn.copyFrom(ch, 0, signals.mic.data() + pos, blockSize);
        for (int ch = 0; ch < refIn.getNumChannels(); ++ch)
            refIn.copyFrom(ch, 0, signals.reference.data() + pos, blockSize);

        proc.processBlock(buffer, midi);
        pos += blockSize;

        if (pos >= nextPoll) {
            float db = proc.getSuppressionDb();
            polledDb.push_back(db);
            printf("  t=%.1fs  ERLE=%.1f dB\n", static_cast<double>(pos) / sampleRate, db);
            nextPoll += pollEvery;
        }
    }
    proc.releaseResources();

    // Pass if the value moved off zero/stuck and settled to something
    // plausible (positive dB, i.e. genuine suppression) by the end.
    bool allZero = true;
    for (float db : polledDb) if (db != 0.0f) allZero = false;
    float lastDb = polledDb.empty() ? 0.0f : polledDb.back();

    const bool pass = !allZero && lastDb > 3.0f;
    printf("  %s\n", pass ? "PASS -- ERLE updates in real time and settles to a plausible value"
                           : "CHECK -- ERLE looks stuck or implausible, don't trust it for a live meter");
    return pass;
}

// User report: muting the reference track (so no PA signal reaches the
// sidechain) still shows gain reduction on the Suppression meter, even
// though far more audio is audibly getting through. Reproduces that exact
// scenario -- converge on real leak+audience material, then zero the
// reference while audience content continues -- and polls the *editor's*
// measured-suppression formula (getInputPeakLevelPostDelayed() minus
// getOutputPeakLevel(), floored, matching PluginEditor.cpp's timerCallback)
// to see how long any residual reading actually persists post-mute.
bool testSuppressionMeterAfterReferenceMute(int sampleRate, int suppressionStrengthIndex,
                                             const char* suppressionStrengthName) {
    printf("\n=== Suppression meter after reference mute test (%d Hz, %s) ===\n",
           sampleRate, suppressionStrengthName);
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
    // no reverberant tail -- AEC3's ResidualEchoEstimator has almost nothing
    // to build a reverb-decay estimate from, so it can't exercise the
    // mechanism this test is actually after. This uses the same spread-out,
    // decaying multi-tap room response as testLargeRoomBenefit (5ms direct
    // plus reflections out to 320ms) so the adaptive filter -- and AEC3's
    // internal reverb model -- has a genuine decaying tail to learn, the
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
    // Everything else stays at its plugin default (Tail Length 800ms,
    // Near-end Sensitivity 75%, Protection Hold Time 100ms, Meters Post
    // Filter on) -- exactly what a user hears out of the box.
    juce::AudioProcessorParameter* suppressionParam = proc.getSuppressionStrengthParameter();
    suppressionParam->setValueNotifyingHost(static_cast<float>(suppressionStrengthIndex) /
                                            static_cast<float>(suppressionParam->getNumSteps() - 1));

    const int blockSize = sampleRate / 100; // 10ms, matches AEC3's own frame
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
// earlier (the internal frame-buffering latency, plus AEC3's own internal
// processing delay -- getLatencySamples() reports the true total, see that
// getter's comment) -- comparing it against
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

// Confirms that changing Tail Length mid-stream never produces a spike, only
// a brief mute -- the "small mute is always better than any noise" rule.
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
    // "800ms") or the parameter "change" is a no-op and never triggers a
    // rebuild at all.
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

    // The rebuild now happens on a background thread (see
    // PAEchoCancellerAudioProcessor::run()), so the clean restart doesn't
    // land at the block where the parameter was set -- the audio thread
    // keeps playing the old instance until the new one is staged (~tens of
    // wall-clock ms; a large span of *stream* samples here, since this
    // harness processes much faster than realtime). So instead of checking
    // a fixed window at the change point, scan everything after it for the
    // restart's signature: a run of exact zeros (FIFO reset -> zero-padded
    // reads) at least half an AEC3 frame long. That both proves the swap
    // actually applied and locates it, and the existing no-spike check
    // still covers the "click/pop" half of the test's original purpose.
    const int minZeroRun = sampleRate / 200; // half an AEC3 frame, ~5ms
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
    printf("  Clean-restart zero gap (>=%d samples): %s at sample %d\n", minZeroRun,
           foundZeroRunAt >= 0 ? "found" : "NOT FOUND", foundZeroRunAt);

    const bool noSpike = (spikeIndex < 0);
    const bool swapApplied = (foundZeroRunAt >= 0);
    const bool pass = noSpike && swapApplied;
    printf("  %s%s%s\n", pass ? "PASS" : "CHECK", noSpike ? "" : " -- SPIKE DETECTED",
           swapApplied ? "" : " -- BACKGROUND REBUILD NEVER APPLIED");
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

    // AEC3 uses a separate, short "initial" filter for the first
    // filter.initial_state_seconds (default 2.5s) regardless of room size --
    // that's intentional (fast bootstrap convergence), but it means the
    // Room Size setting can't show its effect until after that window. Voice
    // occupies 2.0-4.0s here, so measure the PA-only tail (4.0-6.0s):
    // past both the initial phase and the voice burst.
    const double picS0 = durationS * (4.0 / 6.0), picS1 = durationS;
    const double mic_pa = rmsDbfs(signals.mic, sampleRate, picS0, picS1);
    const double small_pa = rmsDbfs(smallOutput, sampleRate, picS0, picS1);
    const double large_pa = rmsDbfs(largeOutput, sampleRate, picS0, picS1);

    printf("  PA-only window: mic=%.1f dBFS  Small=%.1f dBFS (%.1f dB reduction)  Large=%.1f dBFS (%.1f dB reduction)\n",
           mic_pa, small_pa, mic_pa - small_pa, large_pa, mic_pa - large_pa);

    // Note: AEC3 isn't purely the linear adaptive filter -- it also has a
    // nonlinear residual suppressor doing broadband gating on top, which
    // compensates a lot for a too-short filter when the reference is
    // stationary noise (as here). So the gap between Small and Large is
    // real but modest in this synthetic test; a dramatic dB gap isn't the
    // right bar. What matters is that Large is measurably, consistently
    // better, never worse, on a tail the Small filter physically can't
    // reach (echo taps out to 320ms vs its 52ms window).
    const bool pass = (large_pa < small_pa);
    printf("  %s\n", pass ? "PASS -- Large outperforms Small on a long room tail (as expected)" : "CHECK");
    return pass;
}

// Confirms the Suppression Strength presets actually do what they claim.
// Neither a PA-only window nor a voice tone loud enough to dominate the
// leak discriminates them -- AEC3's suppressor squashes a frame based on
// overall energy match against the reference, not source separation, so
// the artifact this control targets only shows up when a *quiet, PA-
// uncorrelated* signal (crowd murmur, ambient room noise -- exactly what
// the user's real recording had) shares frames with real leak at a
// comparable level. This builds that scenario directly: leak and an
// independent low-level ambient noise bed run continuously together, and
// since the mic input is byte-identical across the three runs, any
// difference in output level in steady state is attributable to how much
// of the (uncorrelated, "wanted") ambient bed each preset's suppressor let
// through -- Gentle should measurably outscore Hard.
bool testSuppressionStrengthPresets(int sampleRate) {
    printf("\n=== Suppression Strength presets test (%d Hz) ===\n", sampleRate);
    const double durationS = 8.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(1), ambientRng(2);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.90f);
    reference = onePoleLowpass(reference, 0.90f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    // Kept well below the leak level (not just "quieter but comparable" as
    // before the dominant_nearend_detection fix) so this test stays isolated
    // to normal_tuning's own behavior. With that fix, a louder/more
    // comparable ambient bed now correctly engages dominant-nearend
    // protection (nearend_tuning, itself much more permissive across all
    // three presets), compressing the gap between presets -- which is
    // correct new behavior, just no longer what this specific test is
    // isolating (see testDominantNearendDetectionFix for that).
    std::vector<float> ambient(static_cast<size_t>(n));
    for (auto& v : ambient) v = dist(ambientRng);
    ambient = onePoleLowpass(ambient, 0.6f);
    float ambientPeak = 1e-9f;
    for (float v : ambient) ambientPeak = std::max(ambientPeak, std::abs(v));
    for (auto& v : ambient) v = 0.12f * v / ambientPeak;

    const int delaySamples = static_cast<int>(sampleRate * 5.0 / 1000.0);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = (s - delaySamples >= 0) ? 0.5f * reference[static_cast<size_t>(s - delaySamples)] : 0.0f;
        mic[static_cast<size_t>(s)] = leak + ambient[static_cast<size_t>(s)];
    }

    auto runWithStrength = [&](int strengthIndex) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        proc.getParameters()[1]->setValueNotifyingHost(static_cast<float>(strengthIndex) / 2.0f);
        return runThroughProcessor(proc, sampleRate, reference, mic);
    };

    auto gentleOutput = runWithStrength(0);
    auto moderateOutput = runWithStrength(1);
    auto hardOutput = runWithStrength(2);

    // Past AEC3's 2.5s initial-phase bootstrap, well into steady state.
    const double picS0 = 4.0, picS1 = durationS;
    const double mic_level = rmsDbfs(mic, sampleRate, picS0, picS1);
    const double gentle_level = rmsDbfs(gentleOutput, sampleRate, picS0, picS1);
    const double moderate_level = rmsDbfs(moderateOutput, sampleRate, picS0, picS1);
    const double hard_level = rmsDbfs(hardOutput, sampleRate, picS0, picS1);

    printf("  Steady-state (leak+ambient) window: mic=%.1f dBFS  Gentle=%.1f dBFS  Moderate=%.1f dBFS  Hard=%.1f dBFS\n",
           mic_level, gentle_level, moderate_level, hard_level);

    // Gentle should retain measurably more level (more of the ambient bed
    // surviving) than Hard; Moderate should sit strictly in between.
    const bool pass = (gentle_level > moderate_level + 0.5) && (moderate_level > hard_level + 0.5);
    printf("  %s\n", pass ? "PASS -- Gentle > Moderate > Hard in how much of the ambient bed survives, as expected"
                          : "CHECK");
    return pass;
}

// A non-realtime warm-up/priming mechanism used to live here (primed AEC3
// on the first 3s, replayed that exact clip through the primed filter, then
// streamed normally) to give offline renders full cancellation from sample
// 0. Removed 2026-07-28: it caused a measurable regression instead -- see
// testOfflineVsLiveSpectralShiftAfterWarmup below. Offline (isNonRealtime())
// and live now take the exact same processBlock code path unconditionally;
// there is deliberately no special-cased "instant convergence" behavior for
// offline rendering to test anymore.

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
// its default for both runs. This sets deliberately non-default,
// HF-suppressing settings (Hard + Limit HF Gain on, vs. the plugin's actual
// defaults of Moderate + Limit HF Gain off) *before* prepareToPlay ever
// runs, then renders the same material offline both ways -- if the
// non-default settings are being ignored by the warm-up path, the two
// offline renders would come out sounding the same (both defaulting);
// if they're correctly applied, Hard+Limit should show clearly less HF.
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

    auto runOffline = [&](bool useHardAndLimitHf) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        if (useHardAndLimitHf) {
            proc.getSuppressionStrengthParameter()->setValueNotifyingHost(1.0f); // Hard (index 2 of 0-2)
            proc.getLimitHfGainParameter()->setValueNotifyingHost(1.0f);         // on
        }
        // else: leave everything at its constructor default (Moderate, Limit HF Gain off)
        return runThroughProcessor(proc, sampleRate, reference, mic,
                                    -1, -1, nullptr, /*nonRealtime*/ true);
    };

    auto defaultOfflineOutput = runOffline(false);
    auto hardLimitOfflineOutput = runOffline(true);

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

    const double hfDefault = measureHfRms(defaultOfflineOutput);
    const double hfHardLimit = measureHfRms(hardLimitOfflineOutput);
    const double reducedByDb = 20.0 * std::log10(hfDefault / std::max(hfHardLimit, 1e-12));

    printf("  Offline HF (>6kHz) content: default settings=%.6f  Hard+LimitHF=%.6f  (%.1f dB less with Hard+LimitHF)\n",
           hfDefault, hfHardLimit, reducedByDb);

    const bool pass = reducedByDb > 3.0;
    printf("  %s\n", pass ? "PASS -- offline render correctly applies non-default settings, not constructor defaults"
                           : "CHECK -- offline render doesn't seem to be picking up non-default settings");
    return pass;
}

// Confirms the Dry/Wet blend's dry path is correctly delay-matched against
// the wet (AEC3) path. This is the critical risk in any dry/wet design: if
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

// Diagnostic: does AEC3/APM's own ProcessStream introduce internal
// algorithmic delay (beyond simply needing a full frame's worth of input
// before producing output), separate from anything the plugin's own
// FrameFifo bookkeeping does? Uses runRawAec3, which processes fixed,
// exactly-frameSize-aligned 10ms frames with none of the plugin's own
// host-block/FIFO complexity -- if a click's position still shifts from
// input to output here, that shift is coming from inside APM itself.
bool diagnoseAec3InternalDelay(int sampleRate) {
    printf("\n=== Diagnostic: AEC3/APM internal processing delay (%d Hz) ===\n", sampleRate);
    const int frameSize = sampleRate / 100;
    const double durationS = 2.0;
    const int n = static_cast<int>(sampleRate * durationS);
    const int clickSample = n / 2;
    const int clickLenSamples = static_cast<int>(sampleRate * 0.001);

    std::mt19937 clickRng(71);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < clickLenSamples; ++i) {
        const int s = clickSample + i;
        if (s >= n) break;
        const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(clickLenSamples);
        mic[static_cast<size_t>(s)] = 0.8f * envelope * dist(clickRng);
    }
    const std::vector<float> reference(static_cast<size_t>(n), 0.0f);

    // Align mic/reference to an exact frame boundary so runRawAec3's
    // fixed-frame loop processes the click cleanly.
    const int alignedClickSample = (clickSample / frameSize) * frameSize;

    const auto config = makeEchoCanceller3Config(3, 1, false, 75.0f, 100.0f); // plugin defaults: 800ms, Moderate
    auto output = runRawAec3(config, sampleRate, reference, mic);

    int inputPeakPos = clickSample, outputPeakPos = clickSample;
    float inputPeakVal = 0.0f, outputPeakVal = 0.0f;
    for (int i = alignedClickSample - frameSize; i < alignedClickSample + 4 * frameSize && i < n; ++i) {
        if (std::abs(mic[static_cast<size_t>(i)]) > inputPeakVal) {
            inputPeakVal = std::abs(mic[static_cast<size_t>(i)]);
            inputPeakPos = i;
        }
        if (std::abs(output[static_cast<size_t>(i)]) > outputPeakVal) {
            outputPeakVal = std::abs(output[static_cast<size_t>(i)]);
            outputPeakPos = i;
        }
    }

    const int shiftSamples = outputPeakPos - inputPeakPos;
    printf("  Click peak position: input=%d  output=%d  (APM-internal shift: %d samples, %.2fms)\n",
           inputPeakPos, outputPeakPos, shiftSamples, 1000.0 * shiftSamples / sampleRate);
    printf("  (informational only -- not pass/fail)\n");
    return true;
}

bool diagnoseAec3InternalDelayAllRates() {
    for (int rate : { 44100, 48000, 96000 }) diagnoseAec3InternalDelay(rate);
    return true;
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

// Confirms Limit HF Gain actually costs high-frequency content when on, and
// that it's independent of Suppression Strength (both runs use Gentle).
// This is the control split out after conservative_hf_suppression turned
// out to be clamping HF gain down (a safety measure against an under-
// converged filter estimate), not a gentleness knob -- it was previously
// forced on for Gentle/Moderate, which was audibly stealing high end
// exactly when those presets were trying to let more through.
bool testLimitHfGainToggle(int sampleRate) {
    printf("\n=== Limit HF Gain toggle test (%d Hz) ===\n", sampleRate);
    const double durationS = 8.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(5), ambientRng(6);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.90f);
    reference = onePoleLowpass(reference, 0.90f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    // Left unfiltered (full-bandwidth white noise) rather than lowpassed --
    // real crowd noise/clapping/cymbals have plenty of genuine high-frequency
    // energy, and a heavily-lowpassed synthetic bed would leave the HF gain
    // clamp little to actually clamp, masking the effect being tested here.
    std::vector<float> ambient(static_cast<size_t>(n));
    for (auto& v : ambient) v = dist(ambientRng);
    float ambientPeak = 1e-9f;
    for (float v : ambient) ambientPeak = std::max(ambientPeak, std::abs(v));
    for (auto& v : ambient) v = 0.15f * v / ambientPeak;

    // Stronger leak-to-ambient ratio than the Suppression Strength presets
    // test -- pushes the suppressor into actively pulling gain down for
    // more of the signal (rather than staying near-transparent), which is
    // where an HF-specific clamp on top of that gain actually has material
    // to act on.
    const int delaySamples = static_cast<int>(sampleRate * 5.0 / 1000.0);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = (s - delaySamples >= 0) ? 0.8f * reference[static_cast<size_t>(s - delaySamples)] : 0.0f;
        mic[static_cast<size_t>(s)] = leak + ambient[static_cast<size_t>(s)];
    }

    auto runWithLimitHfGain = [&](bool limitHfGain) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        proc.getSuppressionStrengthParameter()->setValueNotifyingHost(0.0f); // Gentle
        proc.getLimitHfGainParameter()->setValueNotifyingHost(limitHfGain ? 1.0f : 0.0f);
        return runThroughProcessor(proc, sampleRate, reference, mic);
    };

    auto outputWithLimit = runWithLimitHfGain(true);
    auto outputWithoutLimit = runWithLimitHfGain(false);

    auto measureHfRms = [&](const std::vector<float>& output) {
        HighPassFilterChain hfMeasure;
        hfMeasure.setCutoff(sampleRate, 6000.0f);
        double sumSq = 0.0;
        int count = 0;
        const int start = static_cast<int>(4.0 * sampleRate); // past AEC3's initial-phase bootstrap
        for (int i = start; i < static_cast<int>(output.size()); ++i) {
            float f = hfMeasure.processSample(output[static_cast<size_t>(i)]);
            sumSq += static_cast<double>(f) * f;
            ++count;
        }
        return std::sqrt(sumSq / count);
    };

    const double hfRmsWithLimit = measureHfRms(outputWithLimit);
    const double hfRmsWithoutLimit = measureHfRms(outputWithoutLimit);
    const double gainedDb = 20.0 * std::log10(hfRmsWithoutLimit / std::max(hfRmsWithLimit, 1e-12));

    printf("  HF (>6kHz) content: Limit ON=%.6f  Limit OFF=%.6f  (OFF retains %.1f dB more HF)\n",
           hfRmsWithLimit, hfRmsWithoutLimit, gainedDb);

    const bool pass = gainedDb > 1.0;
    printf("  %s\n", pass ? "PASS -- Limit HF Gain OFF retains more high-frequency content, as expected"
                          : "CHECK");
    return pass;
}

// Confirms Suppression Strength now updates live via
// EchoCanceller3::UpdateSuppressorConfig() -- unlike a Tail Length change
// (which resets the FIFOs and reads back zero-padded silence for a frame,
// verified in testClickFreeRoomChange), this should show NO dip at all at
// the switch point, since only the suppressor is replaced -- the FIFOs,
// adaptive filter, and delay estimator are never touched. Also confirms the
// switch actually changes suppression behavior (Gentle lets more of a
// steady ambient bed through than Hard), so this isn't a no-op live update.
bool testLiveSuppressorConfigUpdate(int sampleRate) {
    printf("\n=== Live suppressor config update test (%d Hz) ===\n", sampleRate);
    const double durationS = 8.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(13), ambientRng(14);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    reference = onePoleLowpass(reference, 0.9f);
    reference = onePoleLowpass(reference, 0.9f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    // Ambient bed deliberately quiet relative to the leak (0.06 vs 0.8x
    // leak below): the Hard-vs-Gentle discriminator at the end of this test
    // measures how much of this bed each preset's suppressor gates away,
    // and raising erle.min to its measured real-material value (see
    // TailLengthEchoControl.h) legitimately backed the suppressor off
    // across the board -- with the original 0.15 ambient / 0.5 leak mix the
    // steady-state Hard/Gentle delta collapsed from >1dB to ~0.3dB and the
    // test tripped, not because the live update stopped working but
    // because both presets had less left to disagree about. A more
    // echo-dominant mix restores the pressure difference the test needs.
    std::vector<float> ambient(static_cast<size_t>(n));
    for (auto& v : ambient) v = dist(ambientRng);
    float ambientPeak = 1e-9f;
    for (float v : ambient) ambientPeak = std::max(ambientPeak, std::abs(v));
    for (auto& v : ambient) v = 0.06f * v / ambientPeak;

    const int delaySamples = static_cast<int>(sampleRate * 5.0 / 1000.0);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = (s - delaySamples >= 0) ? 0.8f * reference[static_cast<size_t>(s - delaySamples)] : 0.0f;
        mic[static_cast<size_t>(s)] = leak + ambient[static_cast<size_t>(s)];
    }

    PAEchoCancellerAudioProcessor proc;
    if (!setMonoLayout(proc)) {
        printf("  FAILED to set mono layout\n");
        return false;
    }
    proc.getSuppressionStrengthParameter()->setValueNotifyingHost(
        proc.getSuppressionStrengthParameter()->convertTo0to1(2.0f)); // start at Hard

    const int changeAtSample = static_cast<int>(sampleRate * durationS / 2.0);
    int changeSampleIndex = -1;
    auto output = runThroughProcessor(proc, sampleRate, reference, mic,
                                      changeAtSample, /* Gentle */ 0, &changeSampleIndex,
                                      /*nonRealtime*/ false, /*paramIndexToChange*/ 1);

    const int checkSamples = sampleRate / 10; // 100ms
    double sumSqBefore = 0.0, sumSqAfter = 0.0;
    for (int i = changeSampleIndex - checkSamples; i < changeSampleIndex; ++i)
        sumSqBefore += static_cast<double>(output[static_cast<size_t>(i)]) * output[static_cast<size_t>(i)];
    for (int i = changeSampleIndex; i < changeSampleIndex + checkSamples; ++i)
        sumSqAfter += static_cast<double>(output[static_cast<size_t>(i)]) * output[static_cast<size_t>(i)];
    const double rmsBefore = std::sqrt(sumSqBefore / checkSamples);
    const double rmsAfter = std::sqrt(sumSqAfter / checkSamples);

    const double levelBefore = rmsDbfs(output, sampleRate, 3.0, durationS / 2.0 - 0.2);
    const double levelAfter = rmsDbfs(output, sampleRate, durationS / 2.0 + 0.2, durationS - 0.5);

    printf("  Around the switch (100ms each side): before=%.4f RMS  after=%.4f RMS (no FIFO reset expected)\n",
           rmsBefore, rmsAfter);
    printf("  Steady-state level: Hard (before)=%.1f dBFS  Gentle (after)=%.1f dBFS\n", levelBefore, levelAfter);

    const bool noDip = rmsAfter > rmsBefore * 0.3;
    const bool behaviorChanged = levelAfter > levelBefore + 1.0;

    printf("  %s\n", (noDip && behaviorChanged)
                         ? "PASS -- suppressor updated live with no dip, and behavior actually changed"
                         : "CHECK");
    return noDip && behaviorChanged;
}

// Confirms the dominant_nearend_detection fix actually protects genuine
// double-talk (audience loud together with real PA bleed) better than the
// stock AEC3 thresholds -- this is the "whole signal gets gated whenever
// the reference is present" symptom, reproduced here with a PA reference
// and an "audience" signal that overlap in frequency (so there's real
// suppression pressure) but skew differently across the detector's own
// low-frequency-only energy sums (reference more LF-heavy, audience more
// mid-heavy) -- exactly the profile a bass-heavy concert PA vs. crowd
// noise/clapping/voices would have. Compares stock vs fixed thresholds
// directly on raw AEC3 at the exact settings (50ms tail, Hard suppression)
// the user reported the symptom at, since the plugin itself can no longer
// produce the stock (pre-fix) config. (Verified directly during
// development that stock thresholds never once entered nearend-state
// across the whole test, while the fixed thresholds did ~60% of the time
// -- confirming this isn't just a coincidental level difference.)
bool testDominantNearendDetectionFix(int sampleRate) {
    printf("\n=== Double-talk (dominant near-end) protection test (%d Hz) ===\n", sampleRate);
    const double durationS = 8.0;
    const int n = static_cast<int>(sampleRate * durationS);

    std::mt19937 refRng(9), audienceRng(10);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> reference(static_cast<size_t>(n));
    for (auto& v : reference) v = dist(refRng);
    for (int stage = 0; stage < 2; ++stage) reference = onePoleLowpass(reference, 0.9f);
    float refPeak = 1e-9f;
    for (float v : reference) refPeak = std::max(refPeak, std::abs(v));
    for (auto& v : reference) v = 0.8f * v / refPeak;

    std::vector<float> audience(static_cast<size_t>(n));
    for (auto& v : audience) v = dist(audienceRng);
    HighPassFilterChain audienceHpf;
    audienceHpf.setCutoff(sampleRate, 1000.0f);
    for (auto& v : audience) v = audienceHpf.processSample(v);
    float audiencePeak = 1e-9f;
    for (float v : audience) audiencePeak = std::max(audiencePeak, std::abs(v));
    for (auto& v : audience) v = 0.5f * v / audiencePeak; // genuinely loud, comparable to the leak

    const int delaySamples = static_cast<int>(sampleRate * 5.0 / 1000.0);
    std::vector<float> mic(static_cast<size_t>(n), 0.0f);
    for (int s = 0; s < n; ++s) {
        float leak = (s - delaySamples >= 0) ? 0.6f * reference[static_cast<size_t>(s - delaySamples)] : 0.0f;
        mic[static_cast<size_t>(s)] = leak + audience[static_cast<size_t>(s)];
    }

    webrtc::EchoCanceller3Config oldConfig = makeEchoCanceller3Config(0, 2, false, 70.0f, 200.0f); // 50ms, Hard
    oldConfig.suppressor.dominant_nearend_detection.enr_threshold = .25f;
    oldConfig.suppressor.dominant_nearend_detection.enr_exit_threshold = 10.f;
    oldConfig.suppressor.dominant_nearend_detection.snr_threshold = 30.f;

    const webrtc::EchoCanceller3Config newConfig = makeEchoCanceller3Config(0, 2, false, 70.0f, 200.0f); // same, fix already baked in

    auto oldOutput = runRawAec3(oldConfig, sampleRate, reference, mic);
    auto newOutput = runRawAec3(newConfig, sampleRate, reference, mic);

    const double picS0 = 4.0, picS1 = durationS;
    const double mic_level = rmsDbfs(mic, sampleRate, picS0, picS1);
    const double old_level = rmsDbfs(oldOutput, sampleRate, picS0, picS1);
    const double new_level = rmsDbfs(newOutput, sampleRate, picS0, picS1);

    printf("  Steady-state (leak+audience) window: mic=%.1f dBFS  Old detector=%.1f dBFS  New detector=%.1f dBFS\n",
           mic_level, old_level, new_level);

    const bool pass = (new_level > old_level + 1.0);
    printf("  %s\n", pass ? "PASS -- looser dominant-nearend thresholds preserve more real double-talk content"
                          : "CHECK");
    return pass;
}

// Confirms Near-end Sensitivity and Protection Hold Time actually reach the
// config fields they're documented to control, and move in the expected
// direction. The underlying detector behavior itself (does loosening these
// actually protect double-talk better) is already proven by
// testDominantNearendDetectionFix -- this just checks the GUI-facing
// mapping is wired correctly.
//
// Note: a follow-up attempt to also synthetically reproduce "does a shorter
// hold time audibly pulse more" (the user's real-world report after
// dropping to 100ms) did not pan out -- three different signal designs
// (continuous ambient, then 300ms/300ms bursts, then 300ms/150ms bursts
// with early/late bleed measurement within the gap) all showed no
// measurable difference between 100ms and 200ms hold in this offline
// harness, despite the underlying hold_counters_ decrement logic being
// confirmed correct by direct instrumentation earlier (see
// testDominantNearendDetectionFix's history). Removed rather than keep a
// test that doesn't actually confirm what it claims to -- the mechanism
// explanation given to the user rests on the source-level logic, not on a
// synthetic proof of the specific audible consequence.
bool testNearendSensitivityAndHoldTime() {
    printf("\n=== Near-end Sensitivity / Protection Hold Time mapping test ===\n");

    const auto low = makeEchoCanceller3Config(2, 2, false, 0.0f, 40.0f);
    const auto high = makeEchoCanceller3Config(2, 2, false, 100.0f, 800.0f);

    printf("  Sensitivity 0%%:   enr_threshold=%.3f snr_threshold=%.3f\n",
           low.suppressor.dominant_nearend_detection.enr_threshold,
           low.suppressor.dominant_nearend_detection.snr_threshold);
    printf("  Sensitivity 100%%: enr_threshold=%.3f snr_threshold=%.3f\n",
           high.suppressor.dominant_nearend_detection.enr_threshold,
           high.suppressor.dominant_nearend_detection.snr_threshold);
    printf("  Hold time 40ms:  hold_duration=%d blocks\n", low.suppressor.dominant_nearend_detection.hold_duration);
    printf("  Hold time 800ms: hold_duration=%d blocks\n", high.suppressor.dominant_nearend_detection.hold_duration);

    // Higher sensitivity = easier to trigger = higher enr_threshold, lower
    // snr_threshold (see applyNearendSensitivity's comment).
    const bool sensitivityOk =
        high.suppressor.dominant_nearend_detection.enr_threshold >
            low.suppressor.dominant_nearend_detection.enr_threshold &&
        high.suppressor.dominant_nearend_detection.snr_threshold <
            low.suppressor.dominant_nearend_detection.snr_threshold;
    // 40ms / 800ms at 4ms per block -> 10 / 200 blocks.
    const bool holdTimeOk = low.suppressor.dominant_nearend_detection.hold_duration == 10 &&
                            high.suppressor.dominant_nearend_detection.hold_duration == 200;

    const bool pass = sensitivityOk && holdTimeOk;
    printf("  %s\n", pass ? "PASS -- both controls reach the expected config fields in the expected direction"
                          : "CHECK");
    return pass;
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

// Proves the assumption the live Tail Length swap rests on: AEC3's internal
// processing delay (measured once per prepareToPlay and *reused* for every
// live swap -- see aec3InternalDelaySamples in PluginProcessor.h) is a
// function of sample rate only, not of the filter length. If a future AEC3
// change ever made the delay tail-dependent, silently reusing a stale
// measurement would corrupt both the reported latency and the dry-path
// alignment -- this catches that before it ships.
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
    printf("  %s\n", pass ? "PASS -- safe to measure once per prepareToPlay and reuse across live tail swaps"
                          : "CHECK -- delay depends on Tail Length; the cached measurement is WRONG");
    return pass;
}

// The regression test for the audio-thread stall this replaced: changing
// Tail Length mid-stream used to run the whole rebuild inside processBlock,
// including a ~3-second calibration simulation -- ~150ms+ of blocking work
// in one audio callback, a guaranteed live-rig dropout. Now processBlock
// only requests the rebuild and later adopts the result, so no single
// callback may take longer than a generous "something is very wrong"
// threshold, and the change must still actually apply (background thread
// delivers, audio thread adopts) while audio keeps flowing.
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
    const int changeAtSample = static_cast<int>(sampleRate * 4.0); // past AEC3's 2.5s initial phase
    bool changeApplied = false;
    double maxBlockMsAfterChange = 0.0;
    int adoptedAtSample = -1;

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
            if (adoptedAtSample < 0 && proc.getAppliedTailLengthIndex() == 0)
                adoptedAtSample = pos;
        }
    }

    const int latencyAfter = proc.getLatencySamples();
    const bool adopted = adoptedAtSample >= 0;
    proc.releaseResources();

    // 80ms: far above any sane per-block cost (the steady-state cost of a
    // 512-sample block is well under 1ms even at 96kHz) but far below the
    // ~150ms+ the old inline rebuild burned -- loose enough to not flake on
    // a loaded machine, tight enough that a reintroduced inline rebuild
    // fails it every time.
    const bool noStall = maxBlockMsAfterChange < 80.0;
    const bool latencyStable = (latencyAfter == latencyBefore);

    printf("  Change requested at sample %d, adopted at sample %d (%s)\n", changeAtSample, adoptedAtSample,
           adopted ? "applied" : "NEVER APPLIED");
    printf("  Max single processBlock after change: %.2f ms (limit 80ms; old inline rebuild: ~150ms+)\n",
           maxBlockMsAfterChange);
    printf("  Reported latency: %d -> %d samples (%s)\n", latencyBefore, latencyAfter,
           latencyStable ? "unchanged, as designed" : "CHANGED");

    const bool pass = adopted && noStall && latencyStable;
    printf("  %s\n", pass ? "PASS -- live tail change applies without blocking the audio thread" : "CHECK");
    return pass;
}

// The delay readout added to the editor is only useful if AEC3's delay
// stats actually flow through the new getters -- confirm an estimate
// appears once the canceller has signal to lock onto, and that it lands in
// a sane range (the synthetic leak is at 5ms, and AEC3's estimate also
// absorbs its own internal buffering, so accept anything from 0 up to
// ~100ms -- the point is "present and plausible", not a precise figure).
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
    const int medianMs = proc.getEchoPathDelayMedianMs();
    proc.releaseResources();
    const int delayAfterRelease = proc.getEstimatedEchoPathDelayMs();

    printf("  Before audio: %d   After 8s of leak: instantaneous=%d ms, median=%d ms   After release: %d\n",
           delayBeforeAudio, delayMs, medianMs, delayAfterRelease);

    const bool pass = delayMs >= 0 && delayMs <= 100 && delayAfterRelease == -1;
    printf("  %s\n", pass ? "PASS -- delay estimate is exposed and plausible"
                          : "CHECK -- delay estimate missing or implausible");
    return pass;
}

bool testStateSaveRestore() {
    printf("\n=== State save/restore round-trip test ===\n");

    PAEchoCancellerAudioProcessor proc;
    proc.getTailLengthParameter()->setValueNotifyingHost(
        proc.getTailLengthParameter()->convertTo0to1(2.0f));                 // "400ms"
    proc.getSuppressionStrengthParameter()->setValueNotifyingHost(0.0f);     // "Gentle"
    proc.getHpfFrequencyParameter()->setValueNotifyingHost(
        proc.getHpfFrequencyParameter()->convertTo0to1(150.0f));
    proc.getMetersPostFilterParameter()->setValueNotifyingHost(0.0f);        // off
    proc.getDryWetMixParameter()->setValueNotifyingHost(
        proc.getDryWetMixParameter()->convertTo0to1(37.0f));
    proc.getLimitHfGainParameter()->setValueNotifyingHost(1.0f);             // on
    proc.getNearendSensitivityParameter()->setValueNotifyingHost(
        proc.getNearendSensitivityParameter()->convertTo0to1(42.0f));
    proc.getProtectionHoldTimeParameter()->setValueNotifyingHost(
        proc.getProtectionHoldTimeParameter()->convertTo0to1(333.0f));

    juce::MemoryBlock state;
    proc.getStateInformation(state);

    PAEchoCancellerAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    const bool pass =
        restored.getTailLengthParameter()->getIndex() == 2 &&
        restored.getSuppressionStrengthParameter()->getIndex() == 0 &&
        std::abs(restored.getHpfFrequencyParameter()->get() - 150.0f) < 0.5f &&
        restored.getMetersPostFilterParameter()->get() == false &&
        std::abs(restored.getDryWetMixParameter()->get() - 37.0f) < 0.5f &&
        restored.getLimitHfGainParameter()->get() == true &&
        std::abs(restored.getNearendSensitivityParameter()->get() - 42.0f) < 0.5f &&
        std::abs(restored.getProtectionHoldTimeParameter()->get() - 333.0f) < 0.5f;

    printf("  Tail Length=%d Suppression=%d HPF=%.1fHz MetersPost=%d DryWet=%.1f%% LimitHfGain=%d "
           "NearendSensitivity=%.1f%% ProtectionHoldTime=%.1fms\n",
           restored.getTailLengthParameter()->getIndex(),
           restored.getSuppressionStrengthParameter()->getIndex(),
           static_cast<double>(restored.getHpfFrequencyParameter()->get()),
           static_cast<int>(restored.getMetersPostFilterParameter()->get()),
           static_cast<double>(restored.getDryWetMixParameter()->get()),
           static_cast<int>(restored.getLimitHfGainParameter()->get()),
           static_cast<double>(restored.getNearendSensitivityParameter()->get()),
           static_cast<double>(restored.getProtectionHoldTimeParameter()->get()));
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
// cost scales with filter length (length_blocks) -- this gives a concrete
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
// objective per-config metrics, so tuning experiments (ERLE caps, comfort
// noise, suppressor presets...) can be measured on the material that
// matters instead of judged by ear alone.
//
//   PAEchoCancellerVerify --real <mic.wav> <reference.wav> [--seconds N] [--write-outputs]
//
// The two files must share one sample rate; channel 0 of each is used.
// --seconds N trims the material to its first N seconds -- a 60-90s slice
// of a real show is plenty to rank configs, and 6 configs over a full
// half-hour recording is minutes of pointless waiting.
// Metrics deliberately skip the first 5 seconds (AEC3's initial-state
// bootstrap + convergence). "Reduction" here is mic-vs-output level over
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

    const double skipS = 5.0; // AEC3 bootstrap + convergence
    if (lengthS < skipS + 5.0) {
        printf("ERROR: need at least ~10s of material (%.1fs given)\n", lengthS);
        return 1;
    }

    // >=2kHz band isolation for the HF metric -- the range where residual
    // PA bleed (cymbals, vocal consonants, "hiss") is most audible and
    // where AEC3's behavior has historically been the most contentious.
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

    struct Config { int tailIndex; int suppressionIndex; const char* name; };
    // Tail sweep at the Moderate default, suppression sweep at the 400ms
    // default -- 6 runs covering both axes through the daily-driver setting.
    const Config configs[] = {
        { 0, 1, "tail50ms_moderate" },
        { 1, 1, "tail200ms_moderate" },
        { 2, 1, "tail400ms_moderate" },
        { 3, 1, "tail800ms_moderate" },
        { 2, 0, "tail400ms_gentle" },
        { 2, 2, "tail400ms_hard" },
    };

    // The irregular run writes its aggregate report to a separate file so a
    // cadence comparison never clobbers the uniform baseline it's being
    // compared against.
    juce::File csvFile(juce::File::getCurrentWorkingDirectory().getChildFile(
        irregularCadence ? "real_material_report_irregular.csv" : "real_material_report.csv"));
    juce::String csv("config,tail,suppression,fullband_reduction_db,highband_reduction_db,aec3_erle_db,aec3_delay_ms\n");

    printf("\n%-22s %14s %14s %10s %10s\n", "config", "full-band red.", ">=2kHz red.", "ERLE", "delay est.");
    bool allRan = true;
    for (const auto& cfg : configs) {
        PAEchoCancellerAudioProcessor proc;
        if (!setMonoLayout(proc)) { printf("  FAILED to set mono layout\n"); return 1; }
        {
            auto* tailParam = proc.getTailLengthParameter();
            tailParam->setValueNotifyingHost(tailParam->convertTo0to1(static_cast<float>(cfg.tailIndex)));
            proc.getSuppressionStrengthParameter()->setValueNotifyingHost(static_cast<float>(cfg.suppressionIndex) / 2.0f);
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

        // AEC3-internal state polled every ~100ms of material (GetStatistics
        // takes a mutex, fine in this offline harness), on the same 100ms
        // grid the RMS traces below use -- fine enough to resolve a ~1s
        // periodicity (10 bins per cycle), coarse enough that the poll
        // itself is negligible.
        const int binSize = sampleRate / 10;
        struct StatSample { size_t pos; double erleDb; int delayMs; };
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
                statSamples.push_back({ pos, static_cast<double>(proc.getSuppressionDb()),
                                        proc.getEstimatedEchoPathDelayMs() });
                while (nextStatPoll <= pos) nextStatPoll += static_cast<size_t>(binSize);
            }
        }

        const double erleDb = proc.getSuppressionDb();
        const int delayMs = proc.getEstimatedEchoPathDelayMs();
        proc.releaseResources();

        // 100ms-binned RMS traces + the stat polls resampled onto the same
        // grid, written per config for plotting and autocorrelated right here
        // for the periodicity verdict.
        const size_t numBins = n / static_cast<size_t>(binSize);
        std::vector<double> outTraceDb(numBins), micTraceDb(numBins), erleTrace(numBins), delayTrace(numBins);
        {
            size_t statIdx = 0;
            double lastErle = 0.0;
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
                    lastErle = statSamples[statIdx].erleDb;
                    lastDelay = statSamples[statIdx].delayMs;
                    ++statIdx;
                }
                erleTrace[b] = lastErle;
                delayTrace[b] = lastDelay;
            }
        }
        {
            juce::String trace("t_s,mic_rms_db,out_rms_db,erle_db,delay_ms\n");
            for (size_t b = 0; b < numBins; ++b)
                trace << juce::String(0.1 * static_cast<double>(b + 1), 1) << ","
                      << juce::String(micTraceDb[b], 2) << "," << juce::String(outTraceDb[b], 2) << ","
                      << juce::String(erleTrace[b], 2) << "," << static_cast<int>(delayTrace[b]) << "\n";
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
        reportPeriodicity("ERLE dB", erleTrace, 10);
        reportPeriodicity("delay est ms", delayTrace, 10);

        const double outFullRms = fullBandRms(output, skipS);
        const double outHighRms = highBandRms(output, skipS);
        const double fullRedDb = 20.0 * std::log10(std::max(1e-12, micFullRms) / std::max(1e-12, outFullRms));
        const double highRedDb = 20.0 * std::log10(std::max(1e-12, micHighRms) / std::max(1e-12, outHighRms));

        printf("%-22s %11.1f dB %11.1f dB %7.1f dB %7d ms\n", cfg.name, fullRedDb, highRedDb, erleDb, delayMs);
        csv << cfg.name << "," << (cfg.tailIndex == 0 ? 50 : cfg.tailIndex == 1 ? 200 : cfg.tailIndex == 2 ? 400 : 800)
            << "," << (cfg.suppressionIndex == 0 ? "gentle" : cfg.suppressionIndex == 1 ? "moderate" : "hard")
            << "," << juce::String(fullRedDb, 2) << "," << juce::String(highRedDb, 2)
            << "," << juce::String(erleDb, 2) << "," << delayMs << "\n";

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

// ---------------------------------------------------------------------------
// --linear-probe: measure what AEC3's LINEAR filter alone achieves on real
// material, separately from the nonlinear suppressor that runs after it.
//
//   PAEchoCancellerVerify --linear-probe <mic.wav> <reference.wav> [--seconds N] [--write-outputs]
//
// Motivation: on real concert material AEC3's reported ERLE reads ~0.2dB
// while total reduction is 15-18dB. That ERLE is a measurement-gate
// artifact (see the erle.min comment in TailLengthEchoControl.h), so it
// says nothing about what the linear filter actually removes -- this mode
// measures that directly, via AEC3's own linear-output export
// (filter.export_linear_aec_output + the matching AudioProcessing::Config
// flag; BOTH are required -- the APM flag allocates the exchange buffer,
// the AEC3-config flag makes EchoCanceller3 fill it). The export costs an
// extra buffer per frame, so it lives only here, never in the shipping
// plugin path (makeConfiguredApm).
//
// The exported linear output is 16kHz only (160 samples/10ms frame --
// AEC3's linear stage operates on the 0-8kHz band). For a like-for-like
// comparison the mic and the normal fullband output are band-limited and
// decimated to 16kHz too, so mic / linear-only / final are all measured in
// the same domain. Mic and reference get the plugin's own front-end first
// (150Hz HPF + reference trim) so AEC3 sees exactly what the plugin would
// feed it; the FIFO block-bridging is irrelevant to this measurement, so
// the raw 10ms-frame driver is used (same reasoning as runRawAec3).
//
// Also logs a per-second time series (ERLE, delay estimate, mic/linear/
// final RMS) per config -- direct evidence for or against the still-open
// ~1s periodic dropout lead (TailLengthEchoControl.h, the
// excess_render_detection_interval_blocks note).
// ---------------------------------------------------------------------------

// Windowed-sinc FIR lowpass (cutoff safely under 8kHz) + pick-every-Nth
// decimation to 16kHz. Not a production resampler -- just enough
// anti-aliasing for RMS comparisons against AEC3's 16kHz linear output.
static std::vector<float> decimateTo16k(const std::vector<float>& x, int sampleRate) {
    const int factor = sampleRate / 16000;
    if (factor * 16000 != sampleRate || factor < 1) {
        printf("ERROR: %d Hz is not an integer multiple of 16kHz -- cannot decimate\n", sampleRate);
        return {};
    }
    if (factor == 1) return x;

    constexpr int taps = 129; // odd, so the group delay is an integer (64 samples)
    const double cutoff = 7200.0 / sampleRate; // normalized; leaves margin below the 8kHz Nyquist of the target rate
    std::vector<double> h(taps);
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const int m = i - taps / 2;
        const double sinc = m == 0 ? 2.0 * cutoff
                                   : std::sin(2.0 * juce::MathConstants<double>::pi * cutoff * m)
                                         / (juce::MathConstants<double>::pi * m);
        const double window = 0.54 - 0.46 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (taps - 1)); // Hamming
        h[static_cast<size_t>(i)] = sinc * window;
        sum += h[static_cast<size_t>(i)];
    }
    for (auto& v : h) v /= sum; // unity DC gain

    std::vector<float> out;
    out.reserve(x.size() / static_cast<size_t>(factor) + 1);
    const int n = static_cast<int>(x.size());
    for (int center = 0; center < n; center += factor) {
        double acc = 0.0;
        for (int i = 0; i < taps; ++i) {
            const int idx = center + i - taps / 2;
            if (idx >= 0 && idx < n)
                acc += h[static_cast<size_t>(i)] * x[static_cast<size_t>(idx)];
        }
        out.push_back(static_cast<float>(acc));
    }
    return out;
}

struct LinearProbeResult {
    std::vector<float> final48;  // normal fullband APM output, input rate
    std::vector<float> linear16; // exported linear-only output, 16kHz
    std::vector<double> erlePerSecondDb;
    std::vector<int> delayPerSecondMs;
};

// Raw-APM driver (same shape as runRawAec3) with the linear-output export
// enabled on both required flags. `config` must already have
// filter.export_linear_aec_output set -- the factory copies it verbatim.
static LinearProbeResult runLinearProbe(const webrtc::EchoCanceller3Config& config, int sampleRate,
                                  const std::vector<float>& reference, const std::vector<float>& mic) {
    auto apm = webrtc::AudioProcessingBuilder()
                   .SetEchoControlFactory(std::make_unique<TailLengthEchoControlFactory>(config))
                   .Create();
    webrtc::AudioProcessing::Config apmConfig;
    apmConfig.echo_canceller.enabled = true;
    apmConfig.echo_canceller.mobile_mode = false;
    apmConfig.echo_canceller.export_linear_aec_output = true;
    apmConfig.gain_controller1.enabled = false;
    apmConfig.gain_controller2.enabled = false;
    apmConfig.high_pass_filter.enabled = false;
    apmConfig.noise_suppression.enabled = false;
    apm->ApplyConfig(apmConfig);

    const int frameSize = sampleRate / 100;
    const int framesPerSecond = 100;
    const int n = static_cast<int>(mic.size());
    LinearProbeResult result;
    result.final48.assign(static_cast<size_t>(n), 0.0f);
    result.linear16.reserve(static_cast<size_t>(n / (sampleRate / 16000) + 160));
    std::vector<float> refFrame(static_cast<size_t>(frameSize));
    std::vector<float> micFrame(static_cast<size_t>(frameSize));
    float* refPtr = refFrame.data();
    float* micPtr = micFrame.data();
    const webrtc::StreamConfig streamConfig(sampleRate, 1);
    std::array<float, 160> linearFrame{};

    int pos = 0;
    int frameCount = 0;
    while (pos + frameSize <= n) {
        std::copy_n(reference.data() + pos, frameSize, refFrame.data());
        std::copy_n(mic.data() + pos, frameSize, micFrame.data());
        apm->ProcessReverseStream(&refPtr, streamConfig, streamConfig, &refPtr);
        apm->ProcessStream(&micPtr, streamConfig, streamConfig, &micPtr);
        std::copy_n(micFrame.data(), frameSize, result.final48.data() + pos);

        if (apm->GetLinearAecOutput(rtc::ArrayView<std::array<float, 160>>(&linearFrame, 1)))
            result.linear16.insert(result.linear16.end(), linearFrame.begin(), linearFrame.end());

        ++frameCount;
        if (frameCount % framesPerSecond == 0) {
            const auto stats = apm->GetStatistics();
            result.erlePerSecondDb.push_back(stats.echo_return_loss_enhancement.value_or(0.0));
            result.delayPerSecondMs.push_back(static_cast<int>(stats.delay_ms.value_or(-1)));
        }
        pos += frameSize;
    }
    return result;
}

static int runLinearProbeMode(const char* micPath, const char* refPath, double maxSeconds, bool writeOutputs) {
    printf("=== Linear-filter probe (mic->linear vs mic->final) ===\n");
    std::vector<float> mic, ref;
    double micRate = 0.0, refRate = 0.0;
    if (!loadWavMono(juce::File(juce::String(micPath)), mic, micRate)) return 1;
    if (!loadWavMono(juce::File(juce::String(refPath)), ref, refRate)) return 1;
    if (!juce::exactlyEqual(micRate, refRate)) {
        printf("ERROR: sample rates differ (mic %.0f Hz, reference %.0f Hz) -- resample one first\n", micRate, refRate);
        return 1;
    }
    const int sampleRate = static_cast<int>(micRate);
    if (sampleRate % 16000 != 0) {
        printf("ERROR: %d Hz material cannot be compared against the 16kHz linear export\n", sampleRate);
        return 1;
    }
    size_t n = std::min(mic.size(), ref.size());
    if (maxSeconds > 0.0)
        n = std::min(n, static_cast<size_t>(maxSeconds * sampleRate));
    mic.resize(n);
    ref.resize(n);
    const double lengthS = static_cast<double>(n) / sampleRate;
    printf("Material: %.1fs at %d Hz\n", lengthS, sampleRate);

    const double skipS = 5.0; // AEC3 bootstrap + convergence, same span as --real

    // Plugin front-end: per-stream 150Hz HPF (the hpfFrequency default) and
    // reference trim (0dB default -- the multiply is kept so this mirrors
    // the processBlock code shape, see PluginProcessor.cpp).
    const float refGainLinear = juce::Decibels::decibelsToGain(0.0f);
    {
        HighPassFilterChain micHpf, refHpf;
        micHpf.setCutoff(sampleRate, 150.0f);
        refHpf.setCutoff(sampleRate, 150.0f);
        for (size_t i = 0; i < n; ++i) {
            mic[i] = micHpf.processSample(mic[i]);
            ref[i] = refHpf.processSample(ref[i]) * refGainLinear;
        }
    }

    const auto mic16 = decimateTo16k(mic, sampleRate);
    if (mic16.empty()) return 1;
    const double micRms16 = rmsDbfs(mic16, 16000, skipS, lengthS);

    struct ProbeConfig {
        int tailIndex;
        float erleMin;
        float comfortNoiseFloorDbfs;
        const char* name;
    };
    // erle.min sweep at the real-material tail (400ms, Moderate) plus a
    // comfort-noise A/B and tail-length context runs at stock erle.min.
    // mic->linear MUST be identical across the erle.min sweep -- erle.min
    // only feeds the residual-echo estimate driving the suppressor and
    // cannot touch the linear filter. If it moves, the measurement is
    // broken and its numbers are not to be trusted.
    const ProbeConfig probeConfigs[] = {
        { 2, 1.0f, -96.03406f, "tail400_erlemin1" },
        { 2, 2.0f, -96.03406f, "tail400_erlemin2" },
        { 2, 4.0f, -96.03406f, "tail400_erlemin4" },
        { 2, 8.0f, -96.03406f, "tail400_erlemin8" },
        { 2, 1.0f, -70.0f,     "tail400_erlemin1_cn70" },
        { 0, 1.0f, -96.03406f, "tail50_erlemin1" },
        { 1, 1.0f, -96.03406f, "tail200_erlemin1" },
        { 3, 1.0f, -96.03406f, "tail800_erlemin1" },
    };

    printf("\n%-24s %14s %14s %12s %10s\n", "config", "mic->linear", "mic->final", "final(48k)", "delay est.");
    for (const auto& cfg : probeConfigs) {
        // Plugin-default sensitivity/hold (75%/100ms) and Moderate
        // suppression, matching what the plugin would actually run.
        auto config = makeEchoCanceller3Config(cfg.tailIndex, 1, false, 75.0f, 100.0f,
                                               cfg.erleMin, cfg.comfortNoiseFloorDbfs);
        config.filter.export_linear_aec_output = true;

        const auto probe = runLinearProbe(config, sampleRate, ref, mic);
        if (probe.linear16.empty()) {
            printf("%-24s  ERROR: no linear output exported\n", cfg.name);
            return 1;
        }

        const auto final16 = decimateTo16k(probe.final48, sampleRate);
        const double linearRms16 = rmsDbfs(probe.linear16, 16000, skipS, lengthS);
        const double finalRms16 = rmsDbfs(final16, 16000, skipS, lengthS);
        const double finalRms48 = rmsDbfs(probe.final48, sampleRate, skipS, lengthS);
        const double micRms48 = rmsDbfs(mic, sampleRate, skipS, lengthS);
        const int lastDelay = probe.delayPerSecondMs.empty() ? -1 : probe.delayPerSecondMs.back();

        printf("%-24s %11.1f dB %11.1f dB %9.1f dB %7d ms\n", cfg.name,
               micRms16 - linearRms16, micRms16 - finalRms16, micRms48 - finalRms48, lastDelay);

        // Per-second trace: the mic columns repeat across configs but keep
        // each CSV self-contained for plotting.
        juce::String csv("second,erle_db,delay_ms,mic16_rms_db,linear16_rms_db,final16_rms_db\n");
        const size_t numSeconds = probe.erlePerSecondDb.size();
        for (size_t sec = 0; sec < numSeconds; ++sec) {
            const double t0 = static_cast<double>(sec);
            const double t1 = t0 + 1.0;
            csv << static_cast<int>(sec) << ","
                << juce::String(probe.erlePerSecondDb[sec], 2) << ","
                << probe.delayPerSecondMs[sec] << ","
                << juce::String(rmsDbfs(mic16, 16000, t0, t1), 2) << ","
                << juce::String(rmsDbfs(probe.linear16, 16000, t0, t1), 2) << ","
                << juce::String(rmsDbfs(final16, 16000, t0, t1), 2) << "\n";
        }
        juce::File::getCurrentWorkingDirectory()
            .getChildFile(juce::String("linear_probe_") + cfg.name + ".csv")
            .replaceWithText(csv);

        if (writeOutputs) {
            writeWav16("linear_probe_" + std::string(cfg.name) + "_linear16k.wav", probe.linear16, 16000);
            writeWav16("linear_probe_" + std::string(cfg.name) + "_final.wav", probe.final48, sampleRate);
        }
    }

    printf("\nWrote linear_probe_<config>.csv per-second traces (ERLE, delay, RMS)%s.\n",
           writeOutputs ? " and linear/final WAVs" : "");
    printf("mic->linear is the linear filter's real contribution; mic->final adds the suppressor.\n"
           "Both measured in the 16kHz domain (the export's native rate); final(48k) is the\n"
           "fullband figure for cross-checking against --real. Span %.0fs..end.\n", skipS);
    return 0;
}

int main(int argc, char* argv[]) {
    // --linear-probe <mic.wav> <ref.wav>: linear-filter contribution
    // measurement on real material (see the block comment above
    // runLinearProbeMode).
    if (argc >= 2 && std::string(argv[1]) == "--linear-probe") {
        if (argc < 4) {
            printf("Usage: %s --linear-probe <mic.wav> <reference.wav> [--seconds N] [--write-outputs]\n", argv[0]);
            return 1;
        }
        double maxSeconds = 0.0;
        bool writeOutputs = false;
        for (int i = 4; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--seconds" && i + 1 < argc)
                maxSeconds = std::atof(argv[++i]);
            else if (arg == "--write-outputs")
                writeOutputs = true;
        }
        return runLinearProbeMode(argv[2], argv[3], maxSeconds, writeOutputs);
    }

    // --real <mic.wav> <ref.wav>: run the real-material harness instead of
    // the synthetic suite (see the block comment above runRealMaterialMode).
    if (argc >= 2 && std::string(argv[1]) == "--real") {
        if (argc < 4) {
            printf("Usage: %s --real <mic.wav> <reference.wav> [--seconds N] [--write-outputs]\n", argv[0]);
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

    for (int rate : rates) {
        printf("\n=== Sample rate: %d Hz ===\n", rate);

        auto signals = makeSignals(rate, durationS);

        PAEchoCancellerAudioProcessor proc;
        PAEchoCancellerAudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::mono());
        layout.inputBuses.add(juce::AudioChannelSet::mono());
        layout.outputBuses.add(juce::AudioChannelSet::mono());
        if (!proc.setBusesLayout(layout)) {
            printf("  FAILED to set mono buses layout\n");
            allPass = false;
            continue;
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

        bool pass = reductionDb > 10.0 && std::abs(voiceErrDb) < 6.0;
        printf("  %s\n", pass ? "PASS" : "CHECK");
        allPass = allPass && pass;

        writeWav16("plugin_verify_" + std::to_string(rate) + "_mic.wav", signals.mic, rate);
        writeWav16("plugin_verify_" + std::to_string(rate) + "_cleaned.wav", output, rate);
    }

    allPass = testClickFreeRoomChange(48000) && allPass;
    allPass = testLiveSuppressorConfigUpdate(48000) && allPass;
    allPass = testLargeRoomBenefit(48000) && allPass;
    allPass = testSuppressionStrengthPresets(48000) && allPass;
    allPass = testSuppressionMeterUpdates(48000) && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 0, "Gentle") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 1, "Moderate, default") && allPass;
    allPass = testSuppressionMeterAfterReferenceMute(48000, 2, "Hard") && allPass;
    allPass = testSuppressionMeterTransientAlignment(48000) && allPass;
    allPass = testOfflineVsLiveDynamicHfContent(48000) && allPass;
    allPass = testOfflineVsLiveSpectralShiftAfterWarmup(48000) && allPass;
    allPass = testOfflineRenderUsesCurrentSettingsNotDefaults(48000) && allPass;
    allPass = testHighPassFilterResponse() && allPass;
    for (int rate : rates) allPass = testDryWetAlignment(rate) && allPass;
    for (int rate : rates) allPass = testDryWetCombFiltering(rate) && allPass;
    allPass = testGetLatencySamplesAccuracyAllRates() && allPass;
    diagnoseAec3InternalDelayAllRates();
    allPass = testLimitHfGainToggle(48000) && allPass;
    allPass = testDominantNearendDetectionFix(48000) && allPass;
    allPass = testNearendSensitivityAndHoldTime() && allPass;
    allPass = testStateSaveRestore() && allPass;
    for (int rate : rates) allPass = testBypassRawPassthrough(rate) && allPass;
    allPass = testBypassToggleClickFree(48000) && allPass;
    allPass = testReferenceGainTrim() && allPass;
    for (int rate : rates) allPass = testOversizedHostBlock(rate) && allPass;
    for (int rate : rates) allPass = testNonFiniteInputRecovery(rate) && allPass;
    allPass = testLatencyInvariantAcrossTailLengths() && allPass;
    for (int rate : rates) allPass = testLiveTailLengthChangeNonBlocking(rate) && allPass;
    allPass = testDelayStatsExposed(48000) && allPass;

    printf("\n%s\n", allPass ? "ALL TESTS PASS" : "SOME TESTS FAILED -- see CHECK above");

    benchmarkTailLengthCpuCost(48000);

    return allPass ? 0 : 1;
}
