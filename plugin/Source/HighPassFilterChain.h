#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

// A proper 24dB/octave (4th-order) Butterworth high-pass, built from JUCE's
// own IIR filter rather than custom filter math: two cascaded 2nd-order
// sections using the standard Butterworth Q pair for a 4th-order design
// (0.5412 / 1.3066), not two identical stages approximating one.
class HighPassFilterChain {
public:
    void reset() {
        stage1.reset();
        stage2.reset();
    }

    // Called from the audio thread when HPF Frequency moves, so it must not
    // allocate: ArrayCoefficients returns the values by value and they are
    // written into the existing coefficient storage (Coefficients::makeHighPass
    // would heap-allocate a new object on every change).
    void setCutoff(double sampleRate, float frequencyHz) {
        using Array = juce::dsp::IIR::ArrayCoefficients<float>;
        *stage1.coefficients = Array::makeHighPass(sampleRate, frequencyHz, 0.5411961f);
        *stage2.coefficients = Array::makeHighPass(sampleRate, frequencyHz, 1.3065630f);
    }

    // Guards both ways against non-finite samples, because an IIR's feedback
    // state latches them permanently: a single NaN/Inf entering the delay
    // line keeps recirculating, so every subsequent output is NaN forever.
    // Confirmed by measurement before this guard existed -- a 5ms NaN burst
    // on the mic input left the plugin's output 100% non-finite for the
    // entire rest of the session, with no recovery short of reloading the
    // plugin. For a live-use plugin that means one glitched frame from
    // anything upstream (a misbehaving plugin, a driver hiccup, a corrupted
    // digital input) silently kills an audience mic for the rest of the
    // show. Substituting silence on the way in stops the state from ever
    // being poisoned; resetting on a non-finite *output* additionally
    // self-heals a filter whose state has somehow already gone bad (e.g.
    // from a coefficient update mid-blowup), rather than propagating it
    // downstream into the canceller's adaptive filter, which would latch it too.
    float processSample(float x) {
        if (!std::isfinite(x))
            x = 0.0f;
        // A huge but finite sample (an upstream filter on its way to
        // blowing up) passes the check above, then overflows to Inf when
        // the canceller squares it, and that latches there for good. Nothing
        // real is anywhere near +60 dBFS, so the clamp never touches audio.
        x = std::clamp(x, -maxInputLevel, maxInputLevel);

        const float y = stage2.processSample(stage1.processSample(x));

        if (!std::isfinite(y)) {
            reset();
            return 0.0f;
        }
        return y;
    }

private:
    static constexpr float maxInputLevel = 1000.0f; // +60 dBFS

    juce::dsp::IIR::Filter<float> stage1;
    juce::dsp::IIR::Filter<float> stage2;
};
