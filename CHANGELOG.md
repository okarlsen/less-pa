# Changelog

All notable changes to Less PA are documented here. Versions follow
[Semantic Versioning](https://semver.org/).

## [1.0.0]

First public release.

Less PA cancels PA speaker bleed out of an audience microphone, using the
PA feed itself (not a room estimate of it) as a reference signal — wire the
PA feed into the plugin's Reference sidechain input and it removes the
leakage using acoustic echo cancellation (AEC), the same technique phones
and conferencing software use to remove speaker bleed.

### Signal chain

- **Input Conditioning** — a high-pass filter (80–300Hz) applied identically
  to the microphone and PA reference before cancellation, plus a PA
  Reference Trim gain stage.
- **Adaptive Filter** — a Tail Length control (50ms–800ms) matches the
  canceller's internal filter to your venue's reverb, from a dry room up to
  a very large hall.
- **Residual Suppression** — a Suppression Strength control (Gentle /
  Moderate / Hard) tunes how aggressively residual echo is cleaned up after
  the main cancellation, plus an optional HF gain limit.
- **Double-Talk Protection** — decides when a moment is genuine audience
  sound worth protecting rather than PA leakage to remove, with a choice of
  two detector modes (Classic / Subband), adjustable sensitivity, hold time,
  and transition smoothing to avoid audible pumping.

### Output and metering

- Dry/Wet Mix to blend the cancelled signal back with the original.
- Live Input / PA-reference / Suppression / Output meters, switchable
  between pre- and post-filter levels.
- A live PA delay readout — the canceller's own estimate of how far the PA
  reference leads the leakage arriving at the mic, useful as both a wiring
  check and a health check for the echo path.

### Platform

- AU and VST3, macOS 13 or newer (Apple Silicon).
- Runs at 44.1kHz, 48kHz and 96kHz, at 16-, 24- and 32-bit, with host block
  sizes down to very small buffers.
- Built on a patched fork of WebRTC's AEC3
  (github.com/okarlsen/webrtc-audio-processing) that applies most control
  changes live, without interrupting audio — only Tail Length rebuilds the
  adaptive filter and briefly interrupts audio when changed.

Less PA is free to use, provided as-is with no warranty of any kind — see
`LICENSE`.
