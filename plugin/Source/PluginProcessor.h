#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <webrtc/modules/audio_processing/include/audio_processing.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "FrameFifo.h"
#include "HighPassFilterChain.h"
#include "TailLengthEchoControl.h"

class PAEchoCancellerAudioProcessor : public juce::AudioProcessor,
                                       private juce::Thread
{
public:
    PAEchoCancellerAudioProcessor();
    ~PAEchoCancellerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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
    juce::AudioParameterChoice* getSuppressionStrengthParameter() const noexcept { return suppressionStrengthParam; }
    juce::AudioParameterFloat* getHpfFrequencyParameter() const noexcept { return hpfFrequencyParam; }
    juce::AudioParameterFloat* getReferenceGainParameter() const noexcept { return referenceGainParam; }
    juce::AudioParameterBool* getMetersPostFilterParameter() const noexcept { return metersPostFilterParam; }
    juce::AudioParameterFloat* getDryWetMixParameter() const noexcept { return dryWetMixParam; }
    juce::AudioParameterBool* getLimitHfGainParameter() const noexcept { return limitHfGainParam; }
    juce::AudioParameterFloat* getNearendSensitivityParameter() const noexcept { return nearendSensitivityParam; }
    juce::AudioParameterFloat* getProtectionHoldTimeParameter() const noexcept { return protectionHoldTimeParam; }

    // Latency-matched internal bypass. Without this override, hosts
    // synthesize their own bypass by routing around the plugin entirely,
    // which does NOT delay-match: toggling it live shifts this track by the
    // full ~19-20ms reported latency and combs/flams against every other
    // mic on the rig. Exposing our own bypass parameter makes hosts drive
    // it instead, and the internal implementation (see the bypass FIFOs
    // below) keeps the bypassed signal delayed by exactly
    // getLatencySamples(), so toggling never moves the track in time.
    juce::AudioProcessorParameter* getBypassParameter() const override;

    // Linear peak level (0..1+) of the most recent block, for the editor's
    // meters. Updated on the audio thread, read on the message thread.
    // Input/PA-ref track both pre- and post-HPF levels so the editor's
    // pre/post checkbox can switch instantly either way; Output is never
    // touched by the HPF, so it only has one reading.
    float getInputPeakLevelPre() const noexcept { return inputPeakLevelPre.load(std::memory_order_relaxed); }
    float getInputPeakLevelPost() const noexcept { return inputPeakLevelPost.load(std::memory_order_relaxed); }
    float getSidechainPeakLevelPre() const noexcept { return sidechainPeakLevelPre.load(std::memory_order_relaxed); }
    float getSidechainPeakLevelPost() const noexcept { return sidechainPeakLevelPost.load(std::memory_order_relaxed); }
    float getOutputPeakLevel() const noexcept { return outputPeakLevel.load(std::memory_order_relaxed); }

    // The Input (post-HPF) peak from approximately delaySamples samples ago,
    // for time-aligning against the current Output peak. Output at any
    // instant reflects Input from roughly getLatencySamples() earlier (the
    // internal frame-buffering latency plus AEC3's own internal processing
    // delay -- getLatencySamples() reports the true total, see that
    // getter's comment), so comparing it against the *live* Input peak
    // makes fast transients (kick/snare) look heavily suppressed when they
    // simply haven't reached the output yet -- pass getLatencySamples()
    // here to correct for that.
    //
    // Internally looks back exactly delaySamples, no extra margin (see
    // .cpp) -- getLatencySamples() already includes both AEC3's own
    // measured internal processing delay (a precise figure, see
    // aec3InternalDelaySamples) and the nominal one-frame FIFO buffering
    // delay, which together already land safely at or past the true delay
    // without needing extra slack. Looking back too little would match a
    // transient before the output actually reflects it, which reads as a
    // *bigger* false suppression spike than doing no compensation at all;
    // looking back too much (confirmed: adding a full extra frame of
    // margin here, appropriate before aec3InternalDelaySamples was folded
    // into the reported figure) overshoots past where the output has
    // already returned to quiet, reintroducing a false reading the other
    // way. Falls back to the latest known peak if the history doesn't
    // reach back far enough (only possible right after prepareToPlay, or
    // with unusually tiny host block sizes).
    float getInputPeakLevelPostDelayed(int delaySamples) const noexcept;

    // AEC3's own internal echo return loss enhancement (ERLE) stat, in dB.
    // Not used for the editor's Suppression meter (that's measured directly
    // from input/output peak levels instead -- ERLE only reflects the linear
    // adaptive filter's contribution, not the nonlinear suppression stage
    // after it, so it doesn't match what's actually audible). Kept as an
    // internal diagnostic and exercised by verify_main.cpp to confirm AEC3's
    // stats plumbing still works. Only ever call this from the message/UI
    // thread: AudioProcessing::GetStatistics() takes a mutex, so it isn't
    // real-time safe to call from processBlock.
    float getSuppressionDb() const;

    // Incremented once per processBlock call. The editor polls this to tell
    // "no new audio this tick" (normal, host just hasn't called yet) apart
    // from "the host has stopped calling processBlock at all" (transport
    // stopped) -- the meters' last stored readings otherwise freeze forever
    // with no signal that they've gone stale.
    uint32_t getProcessBlockCallCount() const noexcept { return processBlockCallCount.load(std::memory_order_relaxed); }

    // The Tail Length the audio path is *actually* running right now, as
    // opposed to what the parameter is set to. The two differ briefly after
    // a live Tail Length change: the heavy AEC3 rebuild happens on a
    // background thread (see run()), and the audio thread keeps processing
    // on the old instance until the freshly-built one is ready to adopt --
    // so a mid-stream change is no longer an audio-thread stall, just a
    // short (tens of ms) window where this still reports the old index.
    int getAppliedTailLengthIndex() const noexcept { return appliedTailLengthIndex.load(std::memory_order_relaxed); }

    // AEC3's instantaneous echo-path delay estimate in ms (how far the PA
    // reference leads the leakage arriving in the mic, as the delay
    // estimator currently sees it), and the 1s-aggregated median of the
    // same. Nearly free to surface (same GetStatistics() call the
    // Suppression diagnostic already makes) and answers questions nothing
    // else on the panel can: "is my sidechain actually wired?" (no estimate
    // ever appears -- exactly the Reaper pin-connector misrouting failure
    // mode), "is the estimate stable or hunting?" (a fixed PA-to-mic
    // geometry should hold a rock-steady value; jumping around means the
    // reference/mic relationship itself is unstable), and "roughly what
    // delay does this room have?". Returns -1 when unavailable (no apm yet,
    // or AEC3 hasn't produced an estimate). Message/UI thread only --
    // GetStatistics() takes a mutex, same as getSuppressionDb().
    int getEstimatedEchoPathDelayMs() const;
    int getEchoPathDelayMedianMs() const;

private:
    // AEC3, via WebRTC's AudioProcessing module. Configured fresh in
    // prepareToPlay for the host's current sample rate (44.1/48/96kHz all
    // handled internally by AudioProcessing's own resamplers). Only Tail
    // Length actually requires tearing the whole thing down and rebuilding
    // (it changes the adaptive filter's length) -- Suppression
    // Strength/Limit HF Gain/Near-end Sensitivity/Protection Hold Time now
    // apply live via activeEchoControlFactory below, since a full rebuild
    // discarded the filter's convergence for changes that never touched the
    // filter at all. Discovered via a user report: every one of these 5
    // controls used to force a full rebuild, and re-converging a long
    // (400/800ms) Tail Length filter from scratch on every settings tweak
    // made testing feel inconsistent ("have to stop and start a couple of
    // times before it sounds correct").
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;

    // Non-owning: AudioProcessing owns the actual EchoControlFactory (and,
    // through it, the EchoCanceller3 the factory creates) once handed to
    // SetEchoControlFactory(). This raw pointer is only how we reach back
    // into the concrete EchoCanceller3 to call its AEC3-specific
    // UpdateSuppressorConfig() -- AudioProcessing's own API only exposes the
    // abstract EchoControl interface, which has no such method. Only valid
    // between a rebuildEchoCanceller() call and the next one (or
    // releaseResources()); must not be used outside that window.
    TailLengthEchoControlFactory* activeEchoControlFactory = nullptr;

    juce::AudioParameterChoice* tailLengthParam = nullptr;
    // Atomic because the background rebuild thread (run()) reads it to
    // decide whether a requested Tail Length still needs building; written
    // only where the audio path actually switches instances
    // (rebuildEchoCanceller / the adoption step in processBlock).
    std::atomic<int> appliedTailLengthIndex{ -1 };

    // How hard AEC3's nonlinear suppressor is allowed to lean on frames it
    // isn't fully sure are echo -- separate from Dry/Wet, which blends back
    // the *unprocessed* signal (including any real PA leakage). This instead
    // makes AEC3 itself gentler, trading some cancellation for fewer
    // suppressor artifacts (musical noise, pumping) on correlated ambient
    // content. Applies live (see applySuppressorConfigLive below) -- it only
    // touches the suppressor, never the adaptive filter.
    juce::AudioParameterChoice* suppressionStrengthParam = nullptr;
    int appliedSuppressionStrengthIndex = -1;

    // Independent of Suppression Strength: clamps high-frequency suppressor
    // gain down to the average gain of a "known accurate" mid-band region
    // (AEC3's conservative_hf_suppression, see suppression_gain.cc
    // LimitHighFrequencyGains), a safety measure against an under-converged
    // filter's HF estimate. This used to be forced on for Gentle/Moderate,
    // which was wrong -- it only ever reduces HF content further, so it was
    // audibly stealing high end specifically when the presets were trying to
    // let more through. Off by default (AEC3's own stock value), applies to
    // any Suppression Strength setting when enabled.
    juce::AudioParameterBool* limitHfGainParam = nullptr;
    bool appliedLimitHfGain = false;

    // How readily AEC3's double-talk detector (DominantNearendDetector)
    // decides a moment is genuine audience content worth protecting via
    // nearend_tuning, and how long that protection lingers once triggered.
    // Continuous sliders that only commit a change at drag-end or on a
    // discrete text entry, never per-pixel mid-drag (see PluginEditor.cpp) --
    // even though this now applies live rather than rebuilding, there's no
    // reason to reconstruct SuppressionGain on every pixel of a drag either.
    juce::AudioParameterFloat* nearendSensitivityParam = nullptr;
    juce::AudioParameterFloat* protectionHoldTimeParam = nullptr;
    float appliedNearendSensitivity = -1.0f;
    float appliedProtectionHoldTime = -1.0f;

    // Constructs a fresh apm for the given Tail Length (and whatever the
    // other 4 controls currently are) and resets the FIFOs so the pipeline
    // restarts from clean silence (same zero-padded ramp-up as the very
    // first prepareToPlay) rather than glitching on stale state.
    //
    // prepareToPlay-only. A *live* Tail Length change (mid-stream, from
    // processBlock) must never call this: constructing an AudioProcessing
    // instance heap-allocates its way through AEC3's whole filter/buffer
    // setup, which has no place inside an audio callback. Live changes
    // instead request a build from the background thread (see run()) and
    // adopt the result a few blocks later -- processing simply continues on
    // the old instance in the meantime.
    void rebuildEchoCanceller(int tailLengthIndex, int suppressionStrengthIndex, bool limitHfGain,
                              float nearendSensitivity, float protectionHoldTime);

    // Everything a live Tail Length change needs, built off the audio
    // thread and handed over through the two atomic slots below. The box
    // travels a full cycle without any thread ever allocating or freeing
    // where it mustn't: the rebuild thread news it up and stages it in
    // stagedApmSwap; the audio thread claims it, swaps its *contents* (the
    // old apm moves into the box, the new one out -- a pointer swap, no
    // allocation), and parks the box in retiredApmSwap; the rebuild thread
    // finally deletes it there, releasing the old apm off the audio thread.
    struct StagedApm {
        rtc::scoped_refptr<webrtc::AudioProcessing> apm;
        TailLengthEchoControlFactory* factory = nullptr; // owned by apm, same non-owning deal as activeEchoControlFactory
        int tailLengthIndex = -1;
        int suppressionStrengthIndex = -1;
        bool limitHfGain = false;
        float nearendSensitivity = 0.0f;
        float protectionHoldTime = 0.0f;
        double sampleRate = 0.0; // guards against adopting a build from before a re-prepare at a new rate
    };

    // The background rebuild thread. Wakes every ~30ms (plain polling, no
    // signalling from the audio thread -- even a condition-variable notify
    // is a lock the audio callback shouldn't take, and tens of ms of extra
    // wait on a knob turn is imperceptible), garbage-collects
    // retiredApmSwap, and builds whatever requestedTailLengthIndex asks for
    // that isn't already applied or staged. A plain thread rather than a
    // juce::Timer/AsyncUpdater deliberately: those need a running message
    // loop, which the verify console harness doesn't have -- a thread keeps
    // live Tail Length changes working (and testable) everywhere.
    void run() override;

    std::atomic<StagedApm*> stagedApmSwap{ nullptr };  // rebuild thread -> audio thread
    std::atomic<StagedApm*> retiredApmSwap{ nullptr }; // audio thread -> rebuild thread (for deletion)
    std::atomic<int> requestedTailLengthIndex{ -1 };   // what processBlock wants built; -1 = nothing
    std::atomic<double> sampleRateForRebuildThread{ 0.0 };

    // Swaps in a new SuppressionGain (tuning, HF gain clamp, near-end
    // detection) on the *existing* apm via EchoCanceller3::
    // UpdateSuppressorConfig(), leaving the adaptive filter, delay
    // estimation, and render buffer completely untouched -- no silence, no
    // re-convergence. Requires activeEchoControlFactory to be valid (i.e.
    // rebuildEchoCanceller must have run at least once).
    void applySuppressorConfigLive(int suppressionStrengthIndex, bool limitHfGain,
                                    float nearendSensitivity, float protectionHoldTime);

    // Dry/Wet: AEC3 can be too good, stripping correlated ambient content
    // (crowd noise, clapping) along with the real PA leakage and leaving
    // musical-noise/white-noise-like suppressor artifacts behind. Blending
    // back a delay-matched copy of the post-HPF (pre-AEC) dry signal lets
    // that be dialed back by ear.
    //
    // Getting the dry path sample-aligned with wet needs two separate
    // fixes, addressing two separate sources of delay:
    //
    // 1. FIFO bookkeeping: dryDelayFifos is written in lockstep with
    //    micOutFifos -- both get exactly frameSize samples appended at the
    //    exact same point in the frame-processing loop below
    //    (dryFrameScratch stashes each frame's pre-ProcessStream content
    //    immediately before AEC3 overwrites that same buffer in place with
    //    the wet result), and both are drained by the same numSamples
    //    every host block. Since they receive and release identical
    //    amounts at identical times, their backlogs stay identical by
    //    construction -- no latency estimate to get wrong here, regardless
    //    of host block size.
    //
    // 2. AEC3's own internal processing delay: even with (1) perfectly
    //    solved, AEC3/APM itself still delays the wet signal's actual
    //    content by aec3InternalDelaySamples (subband analysis/synthesis
    //    framing -- confirmed by measurement, ~9-10ms, present even with a
    //    silent reference and zero FIFO involvement). Dry bypasses AEC3
    //    entirely, so resyncDryDelay() pre-fills dryDelayFifos with exactly
    //    that many samples of silence to compensate.
    //
    // This used to be just a single plain delay line, pre-filled with
    // reportedLatencySamples (one AEC3 frame) worth of silence, covering
    // neither of the above correctly -- undercounting whenever the host's
    // block size doesn't evenly divide the frame size, and not accounting
    // for AEC3's own internal delay at all. Confirmed by measurement to be
    // off by several ms, which is exactly what showed up as audible comb
    // filtering at Dry/Wet settings other than 0/100%.
    juce::AudioParameterFloat* dryWetMixParam = nullptr;
    int reportedLatencySamples = 0;
    // Measured once per prepareToPlay and reused for every live Tail Length
    // swap after that: the delay is architectural (AEC3's analysis/synthesis
    // framing plus its internal resampling at non-48kHz rates), a function
    // of the sample rate, not of the filter length -- verified by a
    // dedicated test asserting getLatencySamples() is identical across all
    // four Tail Lengths at all three supported rates. Re-measuring used to
    // happen on *every* rebuild "because it's cheap", which was true at
    // prepareToPlay but catastrophically false once rebuilds ran from
    // processBlock: the measurement simulates 3+ seconds of audio through a
    // throwaway AudioProcessing instance, a guaranteed dropout when run
    // inside an audio callback.
    int aec3InternalDelaySamples = 0;
    std::vector<float> dryPrefillSilence; // sized to aec3InternalDelaySamples in prepareToPlay, so resyncDryDelay never allocates
    std::vector<FrameFifo> dryDelayFifos;               // one per mic channel -- see (1) above
    std::vector<FrameFifo> aec3DelayCompensationFifos;  // one per mic channel -- see (2) above
    std::vector<std::vector<float>> dryOutputScratch;   // [channel][sample], sized to samplesPerBlock
    std::vector<std::vector<float>> dryFrameScratch;    // [channel][sample], sized to frameSize
    void resyncDryDelay();

    // Bypass path: a delay-matched copy of the TRULY raw input -- captured
    // before inputHpfChains runs, unlike the Dry/Wet dry tap
    // (dryFrameScratch), which is post-HPF and therefore can't double as a
    // bypass (a bypassed plugin must pass the input through completely
    // unmodified, HPF included). Same three-FIFO delay-matching mechanism
    // already proven for Dry/Wet, one stage per source of delay:
    //   bypassInFifos      mirrors micInFifos' frame-accumulation wait (raw
    //                      samples only leave when a full AEC3 frame leaves
    //                      micInFifos -- identical counts at identical
    //                      times, so the backlogs match by construction);
    //   bypassDelayFifos   mirrors micOutFifos (written frameSize per
    //                      processed frame, drained numSamples per block);
    //   bypassCompFifos    a plain fixed delay of aec3InternalDelaySamples,
    //                      same silence-prefill arrangement (and same
    //                      reason for being a separate FIFO) as
    //                      aec3DelayCompensationFifos.
    // All three are written and drained unconditionally every block, even
    // with bypass inactive -- skipping them while inactive would let the
    // backlogs drift and misalign the first toggle (the same silent-backup
    // failure the Dry/Wet path's unconditional drain exists to prevent).
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
    // the wetMix blend does. wetMix gets away with an instant switch
    // because a human dragging a slider is its own ramp; bypass is clicked
    // or automated by the host as a step, which must not click. The two
    // streams being crossfaded are latency-matched, so the short overlap
    // can't comb.
    std::vector<float> bypassMixScratch; // per-sample ramp values, sized to samplesPerBlock
    float bypassMixCurrent = 0.0f; // 0 = processing, 1 = bypassed; audio thread only
    float bypassRampStep = 0.0f;   // per-sample increment for the ~5ms ramp

    // Shared 80-300Hz, 24dB/octave high-pass applied identically to Input
    // and Reference before AEC3 ever sees them -- unlike Tail Length, a
    // coefficient change here doesn't need a silence-and-rebuild treatment;
    // swapping an IIR filter's coefficients while its state persists is a
    // normal, click-free way to handle live parameter changes.
    juce::AudioParameterFloat* hpfFrequencyParam = nullptr;
    juce::AudioParameterBool* metersPostFilterParam = nullptr;

    // Plain gain-staging utility for the reference feed: for a reference
    // that's clipping, or too quiet to give AEC3 a usable signal, this
    // corrects it before AEC3 ever sees it (the same effect as AEC3's own
    // internal render_levels.render_power_gain_db, but applied in our own
    // code so it's a live, click-free multiply -- no config rebuild).
    //
    // NOT a fix for over-suppression of correlated-but-wanted content
    // (crowd noise/clapping bleeding into other mics) -- that was the
    // original motivation, but measurement disproved it: AEC3's absolute-
    // level gates (echo_audibility, render_levels) sit around -70dBFS-
    // equivalent, so a reference that's merely loud (not clipping, not
    // near-silent) never crosses them, and the masking-threshold ratios
    // that decide suppression strength are computed from quantities that
    // rescale together with the reference, cancelling out a pure gain
    // change. Suppression Strength (see suppressionStrengthParam) is the
    // control that actually addresses that.
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
    // render, which is exactly this plugin's workflow). Exceeding it used to
    // walk straight off the end of the scratch buffers -- a confirmed
    // segfault, not a theoretical one. processBlock now splits any oversized
    // block into chunks of at most this many samples, which keeps every
    // buffer within its allocation without allocating on the audio thread.
    int preparedBlockSize = 0;

    // AEC3 processes fixed ~10ms frames (frameSize = sampleRate / 100, exact
    // for 44100/48000/96000). These FIFOs bridge the host's arbitrary block
    // sizes to that fixed frame size, adding no more buffering than the gap
    // between the two requires.
    int frameSize = 0;

    std::vector<FrameFifo> micInFifos;
    std::vector<FrameFifo> micOutFifos;
    FrameFifo refInFifo;

    // Pre-allocated per-frame scratch buffers, reused every call so
    // processBlock never allocates.
    std::vector<std::vector<float>> micFrameBuffers;
    std::vector<float*> micFramePtrs;
    std::vector<float> refFrameBuffer;
    std::vector<float*> refFramePtrs;

    std::vector<float> silenceBuffer; // fed to AEC3 when the reference bus is disconnected

    std::atomic<float> inputPeakLevelPre{ 0.0f };
    std::atomic<float> inputPeakLevelPost{ 0.0f };
    std::atomic<float> sidechainPeakLevelPre{ 0.0f };
    std::atomic<float> sidechainPeakLevelPost{ 0.0f };
    std::atomic<float> outputPeakLevel{ 0.0f };
    std::atomic<uint32_t> processBlockCallCount{ 0 };

    // Ring buffer backing getInputPeakLevelPostDelayed() -- see that
    // getter's comment. Fixed capacity, no allocation: written once per
    // processBlock call on the audio thread, scanned (cheap, tiny fixed
    // size, only from the 30Hz UI timer) on the message thread. Sized to
    // comfortably cover the getter's ~2x-latency lookback (up to ~40ms at
    // 96kHz) even at unusually small host block sizes.
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
