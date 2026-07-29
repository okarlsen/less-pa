#pragma once

#include <juce_core/juce_core.h>
#include <webrtc/api/audio/echo_control.h>
#include <webrtc/modules/audio_processing/aec3/echo_canceller3.h>

#include <cmath>
#include <memory>

// "Tail Length" maps directly to AEC3's adaptive filter length. Each
// length_blocks unit is 4ms (WebRTC's kNumBlocksPerSecond = 250), so the
// four choices correspond to:
//   50ms  (studio/dry room):         13 blocks -- WebRTC's own stock default.
//   200ms (indoor theatre/venue):    50 blocks.
//   400ms (hall/outdoor PA rig):    100 blocks -- short of hardware AEC units'
//                                    ~500ms ceiling; AEC3 itself is tuned and
//                                    tested around much shorter tails, so this
//                                    is already pushing outside its normal
//                                    range and costs real CPU per 10ms frame.
//   800ms (very large hall):       200 blocks -- experimental, for venues
//                                    like a 10,000-capacity hall where the
//                                    real reverb tail may genuinely run this
//                                    long. Only viable now that the
//                                    excitation-recovery gate below is
//                                    decoupled from filter length (see
//                                    kMaxPoorExcitationRequirementBlocks in
//                                    refined/coarse_filter_update_gain.cc) --
//                                    otherwise this length would need 800ms
//                                    of uninterrupted broadband signal to
//                                    ever resume adapting after any
//                                    narrowband content, which real PA
//                                    material essentially never provides.
inline size_t tailLengthToFilterLengthBlocks(int tailLengthIndex) {
    switch (tailLengthIndex) {
        case 0: return 13;  // 50ms
        case 1: return 50;  // 200ms
        case 2: return 100; // 400ms
        case 3: return 200; // 800ms
        default: return 13;
    }
}

// Near-end Sensitivity: how readily DominantNearendDetector (see the long
// comment in makeEchoCanceller3Config below) decides a moment is genuine
// audience/near-end content worth protecting via nearend_tuning instead of
// normal_tuning. 0% is close to AEC3's own (broken-for-this-use-case) stock
// thresholds; 100% is deliberately more sensitive than the fix originally
// shipped with, for headroom. Interpolated in log space since enr_threshold
// and snr_threshold are ratio-like quantities spanning more than an order
// of magnitude. enr_exit_threshold (the "how much stronger must echo be to
// exit protection early" safety valve) is kept at a fixed 10x
// enr_threshold, preserving the same hysteresis shape at every position.
inline void applyNearendSensitivity(float sensitivityPercent, webrtc::EchoCanceller3Config& config) {
    const float t = juce::jlimit(0.0f, 1.0f, sensitivityPercent / 100.0f);
    const float enrThreshold = 0.25f * std::pow(32.0f, t);        // 0.25 .. 8.0
    const float snrThreshold = 30.0f * std::pow(1.0f / 60.0f, t); // 30 .. 0.5
    config.suppressor.dominant_nearend_detection.enr_threshold = enrThreshold;
    config.suppressor.dominant_nearend_detection.snr_threshold = snrThreshold;
    config.suppressor.dominant_nearend_detection.enr_exit_threshold = enrThreshold * 10.0f;
}

// Protection Hold Time: once the active near-end detector triggers, how
// long (hold_duration, in 4ms blocks) it keeps using nearend_tuning before
// reverting to normal_tuning. Too long and real PA echo can slip through
// in the tail after genuine audience content stops -- a likely source of
// artifacts appearing right after the near-end-protection fix started
// actually working. Set on BOTH detectors' config unconditionally (only
// the active one reads its copy): the subband detector's hold field is our
// own vendored patch (see subband_nearend_detector.cc), added because the
// stock subband detector recomputes its state every 4ms block and flips
// ~14/s on real material vs the dominant detector's 0.82/s.
inline void applyProtectionHoldTime(float holdTimeMs, webrtc::EchoCanceller3Config& config) {
    const int holdBlocks = juce::jmax(1, static_cast<int>(std::round(holdTimeMs / 4.0f)));
    config.suppressor.dominant_nearend_detection.hold_duration = holdBlocks;
    config.suppressor.subband_nearend_detection.hold_duration = holdBlocks;
}

// Near-end Detector "Subband (2-4kHz)": swaps DominantNearendDetector for
// AEC3's SubbandNearendDetector with a band pair measured on this plugin's
// reference venue (90s of real 10k-capacity-hall audience-mic/PA-feed
// recordings, --subband-probe in verify_main.cpp, 2026-07-28): comparing
// 125-1875Hz (subband1, the same band the dominant detector hardcodes)
// against 2-4kHz (subband2) discriminates that venue's crowd from its PA
// roughly an order of magnitude more sharply than the shipping detector
// (quiet-PA-quartile vs loud-PA-quartile trigger selectivity 11-17:1 at
// nearend_threshold 32-64, vs 1.2:1 for the dominant baseline). The bands
// are deliberately NOT exposed as UI: the measurement validated exactly
// this one pair on one venue, and a bin-index control would be an
// invitation to misconfigure. Measured dead ends, so they don't get
// re-proposed: 4-8kHz as subband2 (0.4% trigger rate -- crowd energy on a
// distant audience mic doesn't reach it), 2-6kHz (just a diluted 2-4kHz),
// and nearend_threshold 1-8 (~0% triggers everywhere -- the post-HPF low
// band far outpowers the mid/high averages on this material, so useful
// ratios start above that).
//
// The two detectors have opposite postures on the same material (dominant
// ~88% of blocks in protected state, subband ~32%): this is a change of
// character, not a tuning tweak, which is why it ships as an explicit
// user-facing A/B toggle defaulting to Classic rather than as a silent
// replacement (the comfort-noise regression is the precedent for why the
// listening test, not these numbers, is the acceptance gate).
inline void applyNearendDetectorChoice(int nearendDetectorIndex, float sensitivityPercent,
                                        webrtc::EchoCanceller3Config& config) {
    if (nearendDetectorIndex != 1)
        return; // Classic: keep AEC3's DominantNearendDetector (fields set by applyNearendSensitivity)

    config.suppressor.use_subband_nearend_detection = true;
    auto& sd = config.suppressor.subband_nearend_detection;
    sd.subband1 = { 1, 15 };  // 125-1875Hz -- same band the dominant detector uses
    sd.subband2 = { 16, 32 }; // 2-4kHz -- where this venue's crowd energy actually sits
    // 64ms input smoothing: the probe's nearend_average_blocks 1/16 A/B
    // showed 16 cuts raw flip rate 14/s -> 4.0/s before the hold even acts;
    // smoothing and the hold are complementary (see subband_nearend_detector.cc).
    sd.nearend_average_blocks = 16;
    // Matches the noise gate the dominant detector effectively runs at the
    // 75% Near-end Sensitivity default (snr = 30*(1/60)^0.75 ~= 1.4), so the
    // band-ratio test stays the discriminating variable.
    sd.snr_threshold = 1.0f;
    // Near-end Sensitivity remaps to nearend_threshold here (the dominant
    // fields it also set are simply unread on this path): log-spaced 8->128,
    // i.e. 8 * 16^t, chosen so the measured sweet spot 32-64 spans the
    // 50%-75% positions and the shipped 75% default lands exactly on 64 --
    // the plan's first guess of 16->128 would put 75% at ~76, just outside
    // the measured sweet spot, so the range was widened one octave down
    // instead. The bottom quarter (8-16) measured near-dead (~0% triggers),
    // which mirrors the Classic path's own 0% = "close to stock, barely
    // triggers" character rather than wasting travel.
    const float t = juce::jlimit(0.0f, 1.0f, sensitivityPercent / 100.0f);
    sd.nearend_threshold = 8.0f * std::pow(16.0f, t);
    // hold_duration comes from applyProtectionHoldTime (both detectors);
    // trigger_threshold stays at the vendored default (12 blocks), matching
    // the dominant detector's, so the two A/B sides debounce identically.
}

// Suppression Strength dials back AEC3's nonlinear suppressor -- the part
// that decides, per time-frequency bin, how much of the (already linearly-
// filtered) signal is "probably echo" and squashes it. The actual gain
// formula (SuppressionGain::GainToNoAudibleEcho in suppression_gain.cc) is:
//   enr = echo / (nearend + 1)     -- echo-to-nearend ratio for this bin
//   emr = echo / (masker + 1)      -- echo-to-masking-noise ratio
//   gain = 1.0 (fully transparent) UNLESS enr > enr_transparent AND
//          emr > emr_transparent, in which case gain ramps from 1 down to 0
//          as enr goes from enr_transparent to enr_suppress.
// So a bin is left completely untouched unless the estimated residual echo
// clearly outweighs the actual (near-end) signal in it -- AEC3's stock
// thresholds (enr_transparent .3 for LF / .07 for HF) are very low, meaning
// suppression engages on only mild evidence. The two knobs that matter:
//   - enr_transparent/enr_suppress (mask_lf/mask_hf): raising these raises
//     the bar for suppression to engage at all -- push far enough and the
//     suppressor stops doing much beyond cleaning up genuinely dominant
//     echo, leaning almost entirely on the linear filter instead.
//   - max_dec_factor_lf: the max factor gain is allowed to drop by per
//     block. Lower = only a slow ease-down, directly softening the
//     "pumping" artifact of gain snapping shut and reopening.
// Three presets, from most to least permissive: Gentle, Moderate, Hard.
// Hard (index 2) is untouched AEC3 stock tuning -- the most aggressive of
// the three, and the one that shipped before this control existed.
//
// Real-world testing (2026-07-27) found the original Gentle/Moderate split
// too close together to tell apart, and that Gentle pushed all the way to
// "barely suppresses anything" gave the best-sounding, most natural result
// on real audience-mic/PA material -- at the cost of occasional periodic
// breakthrough of actual band material once the real echo got strong enough
// to cross even Gentle's much higher bar. So the numbering shifted one
// notch: what was Moderate is now Gentle, and a new Moderate was inserted
// as a geometric-mean midpoint between Hard and the new Gentle, giving a
// genuine middle ground rather than a re-run of the same two extremes.
// Moderate is also now the plugin's startup default (see the AudioParameterChoice
// construction in PluginProcessor.cpp) -- Hard's aggressiveness was the
// original source of the musical-noise complaints that started this whole
// thread, so it's no longer the safest thing to default to.
inline void applySuppressionStrength(int suppressionStrengthIndex, webrtc::EchoCanceller3Config& config) {
    using Suppressor = webrtc::EchoCanceller3Config::Suppressor;
    using Thresholds = Suppressor::MaskingThresholds;
    using Tuning = Suppressor::Tuning;

    switch (suppressionStrengthIndex) {
        case 0: // Gentle -- suppression only kicks in on strong, unambiguous echo.
            config.suppressor.normal_tuning = Tuning(Thresholds(1.2f, 1.4f, .4f), Thresholds(.4f, .6f, .4f), 2.0f, 0.12f);
            config.suppressor.nearend_tuning = Tuning(Thresholds(2.2f, 2.4f, .4f), Thresholds(.6f, .9f, .4f), 2.0f, 0.12f);
            break;
        case 1: // Moderate -- geometric-mean midpoint between Hard and Gentle.
            config.suppressor.normal_tuning = Tuning(Thresholds(.6f, .75f, .35f), Thresholds(.17f, .25f, .35f), 2.0f, 0.17f);
            config.suppressor.nearend_tuning = Tuning(Thresholds(1.55f, 1.65f, .35f), Thresholds(.25f, .52f, .35f), 2.0f, 0.17f);
            break;
        default: // Hard -- AEC3's own stock tuning, left untouched.
            break;
    }
}

// The trailing parameters are defaulted so the verify harness can sweep
// erleMin/comfortNoiseFloorDbfs (--linear-probe in verify_main.cpp) without
// every plugin call site having to name them; the defaults ARE the shipped
// values. nearendDetectorIndex sits after them (appended, not inserted)
// because existing harness call sites pass erleMin positionally, and an int
// parameter appearing at that position would accept a float literal via
// silent implicit conversion -- plugin call sites therefore spell out the
// two shipped defaults to reach it.
inline webrtc::EchoCanceller3Config makeEchoCanceller3Config(int tailLengthIndex, int suppressionStrengthIndex,
                                                              bool limitHfGain, float nearendSensitivityPercent,
                                                              float protectionHoldTimeMs,
                                                              float erleMin = 4.0f,
                                                              float comfortNoiseFloorDbfs = -96.0f,
                                                              int nearendDetectorIndex = 0,
                                                              // Deliberately 0 (= AEC3's instant tuning swap), NOT the
                                                              // 40ms the plugin ships -- unlike erleMin/
                                                              // comfortNoiseFloorDbfs above, whose defaults ARE the
                                                              // shipped values. Every pre-existing call site in
                                                              // verify_main.cpp inherits this default, so a nonzero one
                                                              // would silently change what the regression suite
                                                              // measures and destroy its value as a before/after
                                                              // baseline. The plugin passes its real value explicitly.
                                                              float transitionSmoothingMs = 0.0f) {
    webrtc::EchoCanceller3Config config;
    const size_t lengthBlocks = tailLengthToFilterLengthBlocks(tailLengthIndex);
    config.filter.refined.length_blocks = lengthBlocks;
    config.filter.coarse.length_blocks = lengthBlocks;
    // refined_initial/coarse_initial deliberately left at their defaults:
    // they're the short, fast-converging filter used for the first
    // initial_state_seconds before AEC3 switches to the (possibly much
    // longer) refined/coarse filter above -- that's the whole point of the
    // initial/main split, and applies regardless of tail length.

    // Our reference IS the actual PA feed (not a mic estimate of it), so
    // the echo path really is linear and stable in the way AEC3 means here.
    // Telling it so lets it lean on the cleaner linear filter rather than
    // the more aggressive nonlinear suppressor -- the suppressor is
    // typically the source of musical-noise/white-noise-like artifacts on
    // correlated ambient content (crowd noise, clapping) that happens to
    // share energy with the echo estimate.
    config.echo_removal_control.linear_and_stable_echo_path = true;

    // AEC3 has two complete sets of suppressor tuning -- normal_tuning (used
    // almost always) and nearend_tuning (meant to kick in and protect real
    // near-end content during genuine double-talk). Which one is active is
    // decided entirely by DominantNearendDetector::Update
    // (dominant_nearend_detector.cc), and that decision is ALSO what gates
    // LimitHighFrequencyGains's broader, always-on HF clamp in
    // suppression_gain.cc (independent of our own Limit HF Gain toggle) --
    // so if this detector doesn't trigger, neither the Suppression Strength
    // preset's nearend_tuning nor Limit HF Gain=off ever actually get used,
    // regardless of which preset is selected.
    //
    // The detector's own energy sums (ne_sum/echo_sum/noise_sum) are
    // LOW-FREQUENCY-ONLY (bins 1-15 of 65, roughly under ~2kHz -- see
    // low_frequency_energy() in dominant_nearend_detector.cc). For a
    // bass-heavy concert PA rig, the echo's own low-frequency energy can
    // easily exceed the audience's, even when the audience is genuinely
    // loud overall (their loudness is more mid/high than bass) -- so the
    // stock condition (echo_sum < enr_threshold(.25) * ne_sum, AND
    // ne_sum > snr_threshold(30) * noise_sum) essentially never triggers in
    // that acoustic profile. The result: normal_tuning's low thresholds are
    // applied to EVERY moment, including ones where the audience is loud
    // together with real PA bleed -- audible as the whole signal being
    // broadly gated whenever the reference is present, independent of Tail
    // Length or Suppression Strength (this is why it happened even at 50ms
    // and Hard). Loosened both thresholds so genuine near-end-dominant
    // moments actually get detected and protected -- applied unconditionally
    // (all three presets), since a non-functioning double-talk detector
    // isn't something any preset should have to work around. Now exposed as
    // Near-end Sensitivity/Protection Hold Time so the amount of protection
    // (and its tendency to let a bit of real echo through right after,
    // trading gating for artifacts) can be dialed to taste.
    applyNearendSensitivity(nearendSensitivityPercent, config);
    applyProtectionHoldTime(protectionHoldTimeMs, config);
    // Optionally swap the detector making that normal/nearend decision for
    // the venue-measured subband alternative -- see applyNearendDetectorChoice.
    applyNearendDetectorChoice(nearendDetectorIndex, nearendSensitivityPercent, config);

    // Tried widening buffering.max_allowed_excess_render_blocks and
    // delay.hysteresis_limit_blocks here (theory: buffer-latency drift
    // between the reference and mic streams tripping AEC3's echo-path-change
    // filter reset). Reverted: the user reported it made the periodic
    // dropout MORE frequent, not less, and refined the period down to ~1s --
    // which lined up suspiciously with AEC3's own fixed 1-second
    // excess-render check interval rather than with random clock drift.
    // Left at AEC3's stock defaults (8 blocks / 1 block).
    //
    // The ~1s lead itself has since been eliminated at every level an
    // offline harness can reach, in three rounds, each closing one candidate:
    //   1. Raw AEC3 driven in clean, uniform 10ms frames: no periodicity
    //      near 1s -- AEC3 has no inherent 1s cycle tied to that interval.
    //   2. Static analysis of FrameFifo/processBlock accumulation: all
    //      exact integer arithmetic, no drift mechanism; ring wraparound
    //      happens far more often than 1s at any block size.
    //   3. (2026-07-28) The full plugin path over 90s of the real 10k-hall
    //      material under an IRREGULAR, host-realistic block-size cadence
    //      (--real --irregular in verify_main.cpp: the {512,37,129,1,4096,
    //      256,7} pattern), with 100ms-binned output-RMS/suppression-depth/
    //      ERLE/delay-estimate traces autocorrelated: no ~1s peak in any
    //      trace, all six configs, and the irregular-vs-uniform per-bin
    //      difference is noise (RMS 0.07dB, autocorr max r=0.099, isolated
    //      aperiodic bins).
    // CLOSED (2026-07-28): the user confirmed live in Reaper that they no
    // longer hear any ~1s dropout. Combined with the three offline disproofs
    // above, the working conclusion is that the ORIGINAL fix (decoupling the
    // excitation-recovery gate from Tail Length, see
    // kMaxPoorExcitationRequirementBlocks) already resolved the real,
    // user-reported dropout, and this "~1s lead" was an unconfirmed
    // side-theory left over from that investigation -- not a separate,
    // still-live bug. Don't re-open this without a fresh, specific user
    // report of an audible dropout; if one comes in, get a live repro with
    // logging first (host/driver-level causes were the only remaining
    // candidates), rather than assuming it's this same old lead.

    // erle.min is the floor of AEC3's ERLE estimate, which feeds the
    // residual-echo estimate driving the nonlinear suppressor. On a distant
    // audience mic the estimate never leaves this floor: its update gate
    // (subtractor_output_analyzer.cc, e2_refined < 0.5 * y2) requires the
    // residual to be under half the TOTAL mic energy, and on this material
    // y2 is dominated by uncorrelated crowd/room sound no filter can
    // remove -- so the reported ERLE of ~0.2dB on real concert material is
    // a measurement-gate artifact, not the filter failing. With the floor
    // at stock 1.0, AEC3 assumes its linear filter removed NOTHING and
    // runs the suppressor maximally hard -- the over-suppression/gating
    // mechanism behind the artifact complaints. The floor should instead
    // be what the linear filter measurably achieves (see the --linear-probe
    // mode in verify_main.cpp, which exports the linear-only output and
    // measures mic->linear reduction directly on the real material).
    //
    // Measured basis for the 4.0 default (2026-07-28, 90s of the 10k-cap
    // hall recordings via --linear-probe): the linear filter alone removes
    // 9.0dB (mic->linear, 16kHz domain), identical across the erle.min
    // sweep 1/2/4/8 as it must be (the floor can't touch the linear
    // filter -- that invariance doubles as the measurement's sanity
    // check). 8.0 (9dB) would claim the entire measured average with no
    // headroom for the moments the filter does worse, so ship one sweep
    // step lower: 4.0 = 6dB assumed, 3dB under measured. On the same
    // material this relaxed mic->final from 13.8dB to 12.8dB -- suppressor
    // engagement down, no collapse. The per-second trace shows the
    // fullband ERLE estimate does climb to ~13-18dB mid-material when its
    // update gate passes, but sits pinned at this floor for the first
    // ~10s and during crowd-dominant stretches -- exactly the moments the
    // floor's assumed-cancellation value drives the suppressor.
    //
    // Deliberately NOT raising erle.max_l/max_h: the estimator sits at its
    // FLOOR on this material (the gate above never passes), never at its
    // cap, so raising the caps is a measured no-op -- don't re-propose it.
    config.erle.min = erleMin;

    // Comfort noise fills fully-suppressed moments so they decay into
    // plausible room tone instead of digital silence. Tried raising this
    // from stock -96dBFS to -70dBFS on the theory that -96 is effectively
    // off; REVERTED after a real Reaper listening test reported a constant
    // audible HF hiss that outlasted playback by a few ms. Root cause,
    // confirmed by reading comfort_noise_generator.cc: noise_floor_dbfs
    // maps to a hard per-bin power floor (GetNoiseFloorFactor) applied
    // uniformly across every FFT bin, including the high band (computed as
    // a flat average level, not a shaped spectrum) -- -70dBFS is +26dB of
    // power over -96dBFS (measured: 64*10^((90.309-96)*0.1) vs
    // 64*10^((90.309-70)*0.1)), clearly audible, and definitely not
    // "plausible room tone". It also isn't gated by real signal presence:
    // once the capture-noise estimate N2 decays to this floor during
    // silence it's clamped there every frame regardless, so it keeps
    // injecting noise for as long as the host keeps calling processBlock
    // after playback stops (a few ms of host-side buffer drain) -- exactly
    // the reported symptom. Lesson: this parameter needed a listening test
    // before shipping, not just the dB-reduction numbers the harness
    // reports; don't raise it again without one. Left as a defaulted
    // parameter (rather than removing it) so a future, actually-verified
    // value can still be swept from the harness.
    config.comfort_noise.noise_floor_dbfs = comfortNoiseFloorDbfs;

    // Transition Smoothing: how long AEC3 takes to crossfade between its
    // normal and near-end suppressor tunings when the detector flips,
    // instead of swapping them in a single 4ms block. Motivated by a
    // listening test where BOTH detectors pumped and no combination of the
    // trigger-rate controls (Near-end Sensitivity, Protection Hold Time,
    // Suppression Strength) fixed it -- each setting traded one artifact
    // for another. That is the signature of a discontinuity in the
    // individual transition rather than a problem with how often
    // transitions happen, which is all those controls can influence. See
    // nearend_transition_blocks in the vendored echo_canceller3_config.h
    // for the mechanism and for why 0 is bit-exact upstream behavior.
    config.suppressor.nearend_transition_blocks =
        juce::jmax(0, static_cast<int>(std::round(transitionSmoothingMs / 4.0f)));

    applySuppressionStrength(suppressionStrengthIndex, config);

    // See the comment above applySuppressionStrength() -- this used to be
    // forced on for Gentle/Moderate; now it's independent of Suppression
    // Strength entirely, defaulting to AEC3's own stock value (false).
    config.suppressor.conservative_hf_suppression = limitHfGain;

    return config;
}

// Builds EchoCanceller3 instances with our tail-length-selected config,
// substituted in place of AudioProcessing's default AEC3 factory via
// AudioProcessingBuilder::SetEchoControlFactory.
class TailLengthEchoControlFactory : public webrtc::EchoControlFactory {
public:
    explicit TailLengthEchoControlFactory(const webrtc::EchoCanceller3Config& configIn)
        : config(configIn) {}

    std::unique_ptr<webrtc::EchoControl> Create(int sampleRateHz, int numRenderChannels, int numCaptureChannels) override {
        auto echoCanceller = std::make_unique<webrtc::EchoCanceller3>(
            config, std::nullopt, sampleRateHz,
            static_cast<size_t>(numRenderChannels), static_cast<size_t>(numCaptureChannels));
        // webrtc::AudioProcessing only ever hands back the abstract
        // EchoControl interface, which has no suppressor-update method (that's
        // AEC3-specific) -- stash the concrete pointer here so our own
        // PAEchoCancellerAudioProcessor can reach UpdateSuppressorConfig()
        // directly, without needing AudioProcessing to expose it. Ownership
        // stays with whatever holds the returned unique_ptr (AudioProcessing);
        // this is a non-owning observer that becomes invalid once that's torn
        // down -- callers must not use it past the apm's lifetime.
        activeInstance = echoCanceller.get();
        return echoCanceller;
    }

    webrtc::EchoCanceller3* getActiveInstance() const noexcept { return activeInstance; }

private:
    webrtc::EchoCanceller3Config config;
    webrtc::EchoCanceller3* activeInstance = nullptr;
};
