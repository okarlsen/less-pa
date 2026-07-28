#!/usr/bin/env python3
"""Synthesize a fake PA reference + a mic-with-leakage WAV pair to test AEC3.

No numpy dependency -- pure stdlib (wave, math, random).

Signals (all mono, 16-bit PCM, 48 kHz, 6 seconds):
  pa_reference.wav  - the PA feed: a continuous two-tone "music" signal.
  voice_only.wav    - ground truth for what should survive cancellation:
                       independent tone bursts, active only 2s-4s, standing
                       in for the audience member's voice / clapping.
  mic_input.wav     - what the audience mic actually picks up: PA leakage
                       (reference, delayed + attenuated, simulating the
                       acoustic path) + the voice bursts + a small noise floor.
"""

import math
import random
import struct
import wave

SAMPLE_RATE = 48000
DURATION_S = 6.0
NUM_SAMPLES = int(SAMPLE_RATE * DURATION_S)

DELAY_MS = 5.0
DELAY_SAMPLES = int(SAMPLE_RATE * DELAY_MS / 1000)
LEAKAGE_GAIN = 0.5

VOICE_START_S = 2.0
VOICE_END_S = 4.0

random.seed(42)


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def clamp16(x):
    return max(-32768, min(32767, int(x)))


def make_reference():
    # Broadband "music-like" signal: white noise through a couple of
    # cascaded one-pole low-pass stages. Pure tones are a pathological
    # (highly periodic, spectrally sparse) input for an adaptive filter --
    # real PA/music content is much richer, so this is both more realistic
    # and lets AEC3's adaptive filter converge properly.
    white = [random.uniform(-1.0, 1.0) for _ in range(NUM_SAMPLES)]

    def one_pole_lowpass(x, alpha):
        y = [0.0] * len(x)
        prev = 0.0
        for i, v in enumerate(x):
            prev = alpha * prev + (1 - alpha) * v
            y[i] = prev
        return y

    filtered = one_pole_lowpass(white, 0.90)
    filtered = one_pole_lowpass(filtered, 0.90)

    peak = max(abs(v) for v in filtered)
    return [0.8 * v / peak for v in filtered]


def make_voice():
    # Independent content in a different band, active only 2s-4s, with
    # short on/off bursts so it doesn't look like a steady tone.
    out = [0.0] * NUM_SAMPLES
    start = int(VOICE_START_S * SAMPLE_RATE)
    end = int(VOICE_END_S * SAMPLE_RATE)
    burst_len = int(0.25 * SAMPLE_RATE)
    gap_len = int(0.15 * SAMPLE_RATE)
    i = start
    on = True
    while i < end:
        seg_len = burst_len if on else gap_len
        seg_end = min(i + seg_len, end)
        if on:
            for n in range(i, seg_end):
                t = n / SAMPLE_RATE
                out[n] = 0.6 * math.sin(2 * math.pi * 1200.0 * t)
        i = seg_end
        on = not on
    return out


def main():
    reference = make_reference()
    voice = make_voice()

    # Leakage: reference delayed and attenuated, as if picked up acoustically.
    leakage = [0.0] * NUM_SAMPLES
    for i in range(NUM_SAMPLES):
        src = i - DELAY_SAMPLES
        if src >= 0:
            leakage[i] = LEAKAGE_GAIN * reference[src]

    mic = [0.0] * NUM_SAMPLES
    for i in range(NUM_SAMPLES):
        noise = random.uniform(-0.005, 0.005)
        mic[i] = leakage[i] + voice[i] + noise

    ref_i16 = [clamp16(x * 32767) for x in reference]
    voice_i16 = [clamp16(x * 32767) for x in voice]
    mic_i16 = [clamp16(x * 32767) for x in mic]

    write_wav("pa_reference.wav", ref_i16)
    write_wav("voice_only.wav", voice_i16)
    write_wav("mic_input.wav", mic_i16)

    print(f"Wrote pa_reference.wav, voice_only.wav, mic_input.wav "
          f"({NUM_SAMPLES} samples, {SAMPLE_RATE} Hz, {DURATION_S}s)")
    print(f"Leakage: {DELAY_MS}ms delay, {LEAKAGE_GAIN} gain. Voice active {VOICE_START_S}s-{VOICE_END_S}s.")


if __name__ == "__main__":
    main()
