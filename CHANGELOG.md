# Changelog

All notable changes to Less PA are documented here. Versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- A **Reset to defaults** button on the Fine tuning page puts Near-end
  Detector, Protection Hold Time, Transition Smoothing and Limit HF Gain back
  to their defaults. It is greyed out when nothing there has been changed.

## [1.0.4]

### Changed

- **Simpler panel.** Three columns that follow the signal: Input (PA
  Reference Trim, Input HPF, Mic and PA ref meters), Cancellation (Tail
  Length, Suppression Strength, Crowd Protection), and Output (Suppression
  and Output meters, Mix, status). Near-end Detector, Protection Hold Time,
  Transition Smoothing and Limit HF Gain moved behind a **Fine tuning**
  button. They are still automatable parameters, and the button reads
  "adjusted" when any of them is off its default.
- Near-end Sensitivity is now called **Crowd Protection**, and Dry/Wet Mix is
  now called **Mix**. Tail Length choices name the venue they suit (for
  example "800 ms - arena / outdoor"). Saved sessions load unchanged.
- A status line under the output meters says whether the canceller has a PA
  signal, is locking on, or is cancelling.
- The meters always show levels after the HPF and trim, which is what the
  canceller is fed. The PA ref meter, now in the Input column, keeps the
  1.0.3 target zone. The "Meters post HPF" parameter is gone; older sessions
  that saved it ignore the stale value.

### Fixed

- An offline bounce now sounds the same as playback when the session's Tail
  Length differs from the default. Previously, up to about a second at the
  start of a faster-than-realtime bounce ran on the default canceller, with
  around 11 dB less suppression.

## [1.0.3]

- The **PA-ref meter now has a wide target zone** (peaks of about −36 to −3
  dBFS) and colours its bar amber below the zone, green inside it and red
  above it. It always shows the level *after* PA Reference Trim, i.e. what the
  canceller receives, so turning the trim moves the bar toward the zone.
- **Adaptation now works across a much wider reference level range.** AEC3
  only adapts a frequency bin while the reference is above a fixed absolute
  gate, tuned for speech-level input; a PA feed recorded 10–15 dB low sat under
  it and adapted slowly and unevenly, which made a bounce (which always starts
  from a fresh state) differ from playback. The gate is now 100× lower. On a
  real recording the >6 kHz PA removal at reference peaks of −36 dBFS rose from
  4 dB to 12.6 dB, and it now stays within about 1.6 dB of the best level down
  to that point.
- Recommended bounce workflow: start about 10 seconds before the part you want.
- Otherwise unchanged: same cancellation algorithm, parameters and defaults.

## [1.0.2]

The first fully signed and notarized release. No DSP or parameter changes
from 1.0.0 — this release exists to fix distribution, not behaviour.

- Both plugins are now signed with an Apple Developer ID (Sounds Good To Me
  AS) using the hardened runtime, notarized by Apple, and have the
  notarization ticket stapled to each bundle.
- The `.pkg` installer is signed with a Developer ID Installer certificate,
  notarized and stapled, and is back as the recommended download. It installs
  with no Gatekeeper detour — nothing to approve in System Settings, and no
  right-click → Open.
- The `.zip` download no longer needs the one-time `xattr`
  quarantine-clearing step: its bundles carry their own stapled tickets, so
  they load straight away, offline included. It has dropped the `-beta` from
  its name accordingly, since it is no longer an interim workaround.
- Plugins installed from the `.pkg` are stapled too — the bundles are
  notarized before being packaged — so neither distribution route depends on
  a network lookup at first load.
- `packaging/build_installer.sh` and `packaging/build_zip.sh` (renamed from
  `build_beta_zip.sh`) now sign, notarize and staple as part of their normal
  flow, via a shared `packaging/signing.sh`.

## [1.0.1] — beta

Unsigned interim release while Developer ID signing and notarization are in
progress (see the project README/BUILDING.md) — no DSP or parameter changes
from 1.0.0.

- The title bar now shows the running build's version number next to the
  help button, so a screenshot or a bug report is unambiguous about which
  copy is running.
- Added a no-installer `.zip` distribution alongside the `.pkg`
  (`packaging/build_beta_zip.sh`), for anyone hitting Gatekeeper friction
  with the unsigned installer package — the zip still needs a one-time
  quarantine-clearing step (see its included `INSTALL.txt`), but skips
  Installer.app's stricter check entirely.

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
