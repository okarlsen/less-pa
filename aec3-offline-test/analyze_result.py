#!/usr/bin/env python3
"""Compare mic_input.wav vs cleaned_output.wav to quantify AEC3's effect.

Reports RMS energy (dBFS) in two windows:
  - "PA-only" window (0s-2s): reference is playing, no voice. AEC3 should
    drive this down a lot -- this is the leakage getting cancelled.
  - "voice" window (2.25s-3.75s, inside a burst): reference is still
    playing but voice is also present. AEC3 should leave this roughly
    where voice_only.wav says it should be -- proof it isn't just muting
    the mic.
"""

import math
import struct
import wave

SAMPLE_RATE = 48000


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == SAMPLE_RATE
        assert w.getsampwidth() == 2
        n = w.getnframes()
        data = w.readframes(n)
        return list(struct.unpack("<%dh" % n, data))


def rms_dbfs(samples, start_s, end_s):
    a = int(start_s * SAMPLE_RATE)
    b = int(end_s * SAMPLE_RATE)
    seg = samples[a:b]
    if not seg:
        return float("-inf")
    mean_sq = sum((s / 32768.0) ** 2 for s in seg) / len(seg)
    rms = math.sqrt(mean_sq)
    if rms <= 0:
        return float("-inf")
    return 20 * math.log10(rms)


def main():
    mic = read_wav("mic_input.wav")
    cleaned = read_wav("cleaned_output.wav")
    voice = read_wav("voice_only.wav")

    print("=== PA-only window (0.0s - 2.0s): no voice, just PA leakage ===")
    mic_pa = rms_dbfs(mic, 0.0, 2.0)
    cleaned_pa = rms_dbfs(cleaned, 0.0, 2.0)
    print(f"  mic_input.wav (leakage):     {mic_pa:6.1f} dBFS")
    print(f"  cleaned_output.wav:          {cleaned_pa:6.1f} dBFS")
    print(f"  reduction:                   {mic_pa - cleaned_pa:6.1f} dB")

    print()
    print("=== Voice window (2.25s - 3.75s): PA still playing + voice burst ===")
    mic_v = rms_dbfs(mic, 2.25, 3.75)
    cleaned_v = rms_dbfs(cleaned, 2.25, 3.75)
    voice_v = rms_dbfs(voice, 2.25, 3.75)
    print(f"  voice_only.wav (ground truth): {voice_v:6.1f} dBFS")
    print(f"  mic_input.wav (voice+leakage): {mic_v:6.1f} dBFS")
    print(f"  cleaned_output.wav:            {cleaned_v:6.1f} dBFS")
    print(f"  cleaned vs ground truth:       {cleaned_v - voice_v:6.1f} dB")

    print()
    if (mic_pa - cleaned_pa) > 10.0 and abs(cleaned_v - voice_v) < 6.0:
        print("PASS: leakage reduced >10 dB in PA-only window, "
              "voice preserved within 6 dB of ground truth.")
    else:
        print("CHECK: numbers above don't clearly show cancellation working as expected.")


if __name__ == "__main__":
    main()
