# Less PA

An audio plugin that cancels PA speaker bleed out of an audience
microphone, so a mic picking up crowd noise at a live show doesn't also
pick up the PA feed itself.

It works by acoustic echo cancellation (AEC) — the same technique phones
and conferencing software use to remove speaker bleed — using the actual
PA feed as a reference signal (wired into the plugin's Reference sidechain
input), not a guess at the room's acoustics.

The canceller is a full-band frequency-domain Kalman adaptive filter followed
by a per-band bleed suppressor, adding about 4 ms of latency. Built by SGTM on top of
[JUCE](https://juce.com).

## Requirements

- macOS 13 (Ventura) or newer, on Apple Silicon.
- A DAW/host supporting AU or VST3 plugins with a sidechain/second input
  bus (e.g. Logic Pro, Reaper, Ableton Live). Some hosts need the PA
  reference explicitly wired to the plugin's second input in their own
  routing/pin-connector UI rather than relying on automatic sidechain
  detection — check your host's routing if the PA-ref meter isn't showing
  signal.
- Works at 44.1kHz, 48kHz and 96kHz, 16/24/32-bit.

## Install

Download the latest `.pkg` from the
[Releases page](https://github.com/okarlsen/less-pa/releases) and open it.
It installs the AU and VST3 into your own
`~/Library/Audio/Plug-Ins/` — no administrator password needed, and you can
deselect either format if you only want one.

If you would rather not run an installer, the `.zip` on the same page has the
two plugin bundles to copy into place by hand; see the `INSTALL.txt` inside.

Both downloads are signed with an Apple Developer ID and notarized by Apple,
so there is no Gatekeeper detour either way — nothing to approve in System
Settings, and no `xattr` command to run.

## Using it

Wire your PA's feed (a send, a matrix output, whatever your desk can give
you) into the plugin's Reference input, and the audience mic into its main
input. Click the **?** button in the plugin for the full control reference.

### Level the PA feed

The **PA-ref** meter has a wide marked target zone — peaks of about −36 to
−3 dBFS — and its bar is amber below the zone, green inside it and red above
it. A PA feed is very dynamic, so the zone is wide: keep the loud parts inside
it. If the feed sits well below the zone, raise **PA Reference Trim** so the
canceller registers it as PA. Above the zone nothing breaks, but a feed that
hot risks clipping, so there is no benefit in going further.

### Set how much is removed

Less PA works in two stages. **Stage 1, the canceller**, is an adaptive
filter that learns the path from the PA feed to the mic (delay, reflections,
reverb tail, speaker and room colouring), builds a copy of the PA as it
arrives at the mic and subtracts it. Subtraction leaves the crowd untouched,
but it cannot remove what isn't a linear copy of the PA feed: distortion, a
changing room, reverb longer than the **Tail Length**, or sound that never
reaches the reference, such as stage monitors and backline.

**Stage 2, the bleed suppressor**, works on what is left, in frequency bands
about 170–190 Hz wide. It estimates how much PA is still in each band and
ducks the band in proportion, like a multiband ducker keyed from the
estimated leftover PA. It turns down everything in a ducked band, crowd
included, so it trades a little crowd for less PA.

- **Strength** (0–100%, default 25%): how hard the suppressor ducks. 0%
  bypasses stage 2. Lower it if the crowd sounds thin or swirly.
- **Range** (0 to −24 dB, default −12 dB): the most any band can be ducked,
  like a gate's range.
- **Time** (3–50 ms, default 20 ms): attack and release of the ducking.

**Defaults** puts these three back to their defaults; the canceller keeps
what it has learned. Start at the defaults and adjust by ear.

It also helps to start a bounce a few seconds early. The plugin keeps
adapting for as long as it runs, and a lead-in of about 10 seconds before the
part you want gets a bounce close to what you hear on playback.

## Building from source

See [BUILDING.md](BUILDING.md).

## License

Less PA is free to use, provided as-is with no warranty of any kind — see
[LICENSE](LICENSE) (AGPLv3). Third-party components are credited in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Copyright © 2026 SGTM.
