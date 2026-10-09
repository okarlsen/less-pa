#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "FrameFifo.h"
#include "HighPassFilterChain.h"
#include "KalmanEchoCanceller.h"

class PAEchoCancellerAudioProcessor : public juce::AudioProcessor
{
public:
    PAEchoCancellerAudioProcessor();
    ~PAEchoCancellerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock; // the double-precision overload stays JUCE's (not supported)

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioParameterChoice* getTailLengthParameter() const noexcept { return tailLengthParam; }
    juce::AudioParameterFloat* getAmountParameter() const noexcept { return amountParam; }
    juce::AudioParameterFloat* getMaxReductionParameter() const noexcept { return maxReductionParam; }
    juce::AudioParameterFloat* getResponseParameter() const noexcept { return responseParam; }
    juce::AudioParameterFloat* getHpfFrequencyParameter() const noexcept { return hpfFrequencyParam; }
    juce::AudioParameterFloat* getReferenceGainParameter() const noexcept { return referenceGainParam; }
    juce::AudioParameterFloat* getDryWetMixParameter() const noexcept { return dryWetMixParam; }

    // Latency-matched internal bypass. Without this override, hosts
    // synthesize their own bypass by routing around the plugin entirely,
    // which does NOT delay-match: toggling it live shifts this track by the
    // full reported latency and combs/flams against every other mic on the
    // rig. Exposing our own bypass parameter makes hosts drive it instead,
    // and the internal implementation (see the bypass FIFOs below) keeps the
    // bypassed signal delayed by exactly getLatencySamples(), so toggling
    // never moves the track in time.
    juce::AudioProcessorParameter* getBypassParameter() const override;

    // Linear peak level (0..1+) of the most recent block, for the editor's
    // meters. Updated on the audio thread, read on the message thread.
    // Input/PA-ref track both pre- and post-HPF levels: the meters show
    // post-HPF, while the editor's silence detection uses pre-HPF so an
    // aggressive cutoff can't make real signal look like nothing. Output is
    // never touched by the HPF, so it only has one reading.
    float getInputPeakLevelPre() const noexcept { return inputPeakLevelPre.load(std::memory_order_relaxed); }
    float getInputPeakLevelPost() const noexcept { return inputPeakLevelPost.load(std::memory_order_relaxed); }
    float getSidechainPeakLevelPre() const noexcept { return sidechainPeakLevelPre.load(std::memory_order_relaxed); }
    float getSidechainPeakLevelPost() const noexcept { return sidechainPeakLevelPost.load(std::memory_order_relaxed); }
    float getOutputPeakLevel() const noexcept { return outputPeakLevel.load(std::memory_order_relaxed); }

    // True while the Reference bus is carrying a copy of the main input
    // rather than a real PA feed. Logic and MainStage do this to an AU when
    // the track's Side Chain menu is set to None: they keep the sidechain
    // bus active and feed it the track's own input. A real PA reference is
    // never sample-identical to the mic, so the plugin treats that case as
    // no reference at all (see referenceDuplicatesMainInput). Safe from any
    // thread.
    bool isReferenceCopyOfMainInput() const noexcept { return referenceIsMainInput.load(std::memory_order_relaxed); }

    // The Input (post-HPF) peak from approximately delaySamples samples ago,
    // for time-aligning against the current Output peak. Output at any
    // instant reflects Input from getLatencySamples() earlier, so comparing
    // it against the *live* Input peak makes fast transients (kick/snare)
    // look heavily suppressed when they simply haven't reached the output
    // yet -- pass getLatencySamples() here to correct for that. Looks back
    // exactly delaySamples (the reported latency is the true delay, see
    // resyncDryDelay); falls back to the latest known peak if the history
    // doesn't reach back far enough (only possible right after
    // prepareToPlay, or with unusually tiny host block sizes).
    float getInputPeakLevelPostDelayed(int delaySamples) const noexcept;

    // Incremented once per processBlock call. The editor polls this to tell
    // "no new audio this tick" (normal, host just hasn't called yet) apart
    // from "the host has stopped calling processBlock at all" (transport
    // stopped) -- the meters' last stored readings otherwise freeze forever
    // with no signal that they've gone stale.
    uint32_t getProcessBlockCallCount() const noexcept { return processBlockCallCount.load(std::memory_order_relaxed); }

    // The echo-path delay (PA to mic) in ms, read off the adaptive filter's
    // first arrival and held steady over ~2 s (see KalmanEchoCanceller).
    // Answers "is my sidechain actually wired?" (no estimate ever appears)
    // and "roughly what delay does this room have?". -1 until the filter
    // has found the PA in the mic. Safe from any thread.
    int getEstimatedEchoPathDelayMs() const noexcept { return kalman.getDelayEstimateMs(); }

private:
    // The echo canceller: a full-band partitioned-block frequency-domain
    // Kalman filter plus a low-latency Wiener suppressor. Allocated for the
    // longest Tail Length in prepareToPlay, so every control below applies
    // live from processBlock without allocating, without silence and
    // without losing the filter's convergence.
    KalmanEchoCanceller kalman;
    void updateKalmanSettings(); // parameters -> kalman; audio thread, allocation-free

    // How long an echo the filter models (50/200/400/800 ms). Shorter tails
    // drop the filter's far partitions; longer ones start the new ones
    // from zero. The near part, where most of the echo is, is kept.
    juce::AudioParameterChoice* tailLengthParam = nullptr;

    // The bleed suppressor. Strength (ID amount, 0..100%): how hard it
    // ducks what the filter leaves behind; 0% is the adaptive filter alone.
    // Range (ID maxReduction, dB): the deepest cut it may make in any band.
    // Time (ID response, ms): how fast its gains follow the signal; short is
    // tighter on the PA, long is smoother on the crowd. See
    // KalmanEchoCanceller::settingsForAmount for the mapping.
    juce::AudioParameterFloat* amountParam = nullptr;
    juce::AudioParameterFloat* maxReductionParam = nullptr;
    juce::AudioParameterFloat* responseParam = nullptr;

    // Mix: blends back a delay-matched copy of the post-HPF (pre-canceller)
    // dry signal, so how much of the room survives can be dialed back by ear.
    //
    // Getting the dry path sample-aligned with wet needs two separate
    // fixes, addressing two separate sources of delay:
    //
    // 1. FIFO bookkeeping: dryDelayFifos is written in lockstep with
    //    micOutFifos -- both get exactly frameSize samples appended at the
    //    exact same point in the frame-processing loop (dryFrameScratch
    //    stashes each frame's content immediately before the canceller
    //    overwrites that same buffer in place with the wet result), and
    //    both are drained by the same numSamples every host block. Their
    //    backlogs stay identical by construction, regardless of host block
    //    size.
    //
    // 2. The canceller's own delay inside the frame: the suppressor's
    //    linear-phase FIR (KalmanEchoCanceller::suppressorDelaySamples).
    //    Dry bypasses it, so resyncDryDelay() pre-fills a separate fixed
    //    delay line (internalDelayFifos) with exactly that much silence.
    juce::AudioParameterFloat* dryWetMixParam = nullptr;
    std::vector<float> dryPrefillSilence; // pre-sized in prepareToPlay, so resyncDryDelay never allocates
    std::vector<FrameFifo> dryDelayFifos;               // one per mic channel -- see (1) above
    std::vector<FrameFifo> internalDelayFifos;          // one per mic channel -- see (2) above
    std::vector<std::vector<float>> dryOutputScratch;   // [channel][sample], sized to samplesPerBlock
    std::vector<std::vector<float>> dryFrameScratch;    // [channel][sample], sized to frameSize
    void resyncDryDelay();

    // Bypass path: a delay-matched copy of the TRULY raw input -- captured
    // before inputHpfChains runs, unlike the Mix dry tap (dryFrameScratch),
    // which is post-HPF and therefore can't double as a bypass (a bypassed
    // plugin must pass the input through completely unmodified, HPF
    // included). Same delay-matching mechanism as Mix, one stage per source
    // of delay:
    //   bypassInFifos      mirrors micInFifos' frame-accumulation wait (raw
    //                      samples only leave when a full frame leaves
    //                      micInFifos -- identical counts at identical
    //                      times, so the backlogs match by construction);
    //   bypassDelayFifos   mirrors micOutFifos (written frameSize per
    //                      processed frame, drained numSamples per block);
    //   bypassCompFifos    a plain fixed delay of the suppressor FIR delay,
    //                      same silence-prefill arrangement as
    //                      internalDelayFifos.
    // All three are written and drained unconditionally every block, even
    // with bypass inactive -- skipping them while inactive would let the
    // backlogs drift and misalign the first toggle.
    //
    // NOT persisted in getStateInformation/setStateInformation: hosts own
    // bypass state via AU/VST3's bypass mechanism, and a saved project
    // restoring the plugin's private copy of it would fight the host's.
    juce::AudioParameterBool* bypassParam = nullptr;
    std::vector<FrameFifo> bypassInFifos;
    std::vector<FrameFifo> bypassDelayFifos;
    std::vector<FrameFifo> bypassCompFifos;
    std::vector<std::vector<float>> bypassFrameScratch;  // [channel][sample], sized to frameSize
    std::vector<std::vector<float>> bypassOutputScratch; // [channel][sample], sized to samplesPerBlock
    // Toggling crossfades over ~5ms rather than switching per-sample like
    // the Mix blend does. Mix gets away with an instant switch
    // because a human dragging a slider is its own ramp; bypass is clicked
    // or automated by the host as a step, which must not click. The two
    // streams being crossfaded are latency-matched, so the short overlap
    // can't comb.
    std::vector<float> bypassMixScratch; // per-sample ramp values, sized to samplesPerBlock
    float bypassMixCurrent = 0.0f; // 0 = processing, 1 = bypassed; audio thread only
    float bypassRampStep = 0.0f;   // per-sample increment for the ~5ms ramp

    // Shared 80-300Hz, 24dB/octave high-pass applied identically to Input
    // and Reference before the canceller sees them. A coefficient change
    // is a live, click-free swap: the IIR state persists across it.
    juce::AudioParameterFloat* hpfFrequencyParam = nullptr;

    // Plain gain-staging utility for the reference feed: for a reference
    // that's clipping, or too quiet to sit in the meter's target zone.
    // A live, click-free multiply.
    juce::AudioParameterFloat* referenceGainParam = nullptr;
    float appliedHpfFrequency = -1.0f;
    std::vector<HighPassFilterChain> inputHpfChains; // one per mic channel
    HighPassFilterChain referenceHpfChain;
    std::vector<std::vector<float>> inputFilterScratch; // [channel][sample], sized to samplesPerBlock
    std::vector<float> referenceFilterScratch;           // [sample], sized to samplesPerBlock

    double currentSampleRate = 0.0;

    // The samplesPerBlock the host promised in prepareToPlay -- every scratch
    // buffer and FIFO above is sized against it. JUCE documents that figure
    // as the *expected* block size, not a hard guarantee, and hosts do exceed
    // it in practice (notably when switching between playback and offline
    // render). processBlock splits any oversized block into chunks of at
    // most this many samples, which keeps every buffer within its
    // allocation without allocating on the audio thread.
    int preparedBlockSize = 0;

    // The canceller processes fixed frames of one Kalman block (128 samples
    // at 44.1/48 kHz, 256 at 88.2 kHz and above). These FIFOs bridge the host's
    // arbitrary block sizes to that frame size.
    int frameSize = 0;

    std::vector<FrameFifo> micInFifos;
    std::vector<FrameFifo> micOutFifos;
    FrameFifo refInFifo;

    // Pre-allocated per-frame scratch buffers, reused every call so
    // processBlock never allocates.
    std::vector<std::vector<float>> micFrameBuffers;
    std::vector<float*> micFramePtrs;
    std::vector<float> refFrameBuffer;

    std::vector<float> silenceBuffer; // fed as the reference when the reference bus is disconnected or a copy of the input

    // Whether this block's reference is the main input again (see
    // isReferenceCopyOfMainInput). An all-zero reference block can't tell
    // either way and keeps the previous answer, so the status line doesn't
    // flicker through silence.
    static bool referenceDuplicatesMainInput(const juce::AudioBuffer<float>& mainIn, const float* ref, int numSamples) noexcept;
    std::atomic<bool> referenceIsMainInput{ false };

    std::atomic<float> inputPeakLevelPre{ 0.0f };
    std::atomic<float> inputPeakLevelPost{ 0.0f };
    std::atomic<float> sidechainPeakLevelPre{ 0.0f };
    std::atomic<float> sidechainPeakLevelPost{ 0.0f };
    std::atomic<float> outputPeakLevel{ 0.0f };
    std::atomic<uint32_t> processBlockCallCount{ 0 };

    // Ring buffer backing getInputPeakLevelPostDelayed() -- see that
    // getter's comment. Fixed capacity, no allocation: written once per
    // processBlock call on the audio thread, scanned (cheap, tiny fixed
    // size, only from the 30Hz UI timer) on the message thread.
    static constexpr int peakHistoryCapacity = 512;
    struct PeakHistoryEntry {
        std::atomic<int64_t> samplePosition{ -1 }; // -1 = never written
        std::atomic<float> peak{ 0.0f };
    };
    std::array<PeakHistoryEntry, peakHistoryCapacity> inputPeakHistory;
    std::atomic<int> inputPeakHistoryWriteIndex{ 0 };
    std::atomic<int64_t> currentSamplePosition{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PAEchoCancellerAudioProcessor)
};
