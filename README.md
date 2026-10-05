# Less PA

An audio plugin that cancels PA speaker bleed out of an audience
microphone, so a mic picking up crowd noise at a live show doesn't also
pick up the PA feed itself.

It works by acoustic echo cancellation (AEC) — the same technique phones
and conferencing software use to remove speaker bleed — using the actual
PA feed as a reference signal (wired into the plugin's Reference sidechain
input), not a guess at the room's acoustics.

The canceller is a full-band frequency-domain Kalman adaptive filter with a
light cleanup stage, adding about 4 ms of latency. Built by SGTM on top of
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

The plugin first subtracts its own copy of the PA bleed. That copy is never
perfect, so some bleed is left, and an extra removal step turns down the
frequencies where it is still audible. **Amount** sets how much of that
leftover is removed: 0% turns the extra step off and sounds most natural,
higher removes more PA and more of the crowd with it. **Max Reduction**
limits how far it may turn down any frequency, and **Response** how quickly
it follows the sound. Start at the defaults (25%, −12 dB, 20 ms) and adjust
by ear.

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
