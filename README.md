# Less PA

An audio plugin that cancels PA speaker bleed out of an audience
microphone, so a mic picking up crowd noise at a live show doesn't also
pick up the PA feed itself.

It works by acoustic echo cancellation (AEC) — the same technique phones
and conferencing software use to remove speaker bleed — using the actual
PA feed as a reference signal (wired into the plugin's Reference sidechain
input), not a guess at the room's acoustics.

Built by SGTM on top of [JUCE](https://juce.com) and a patched fork of
[WebRTC's AEC3](https://github.com/okarlsen/webrtc-audio-processing).

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

The canceller adapts best when the PA feed is not recorded very quietly. The
**PA-ref** meter has a wide marked target zone — peaks of about −36 to −3 dBFS —
and its bar is amber below the zone, green inside it and red above it. A PA
feed is very dynamic, so the zone is wide: keep the loud parts inside it. If
the feed sits below the zone, raise **PA Reference Trim**; a quiet feed makes
the canceller adapt slowly and unevenly, especially at high frequencies, so a
bounce can end up with noticeably less PA removed than the same audio played
back after the plugin has settled. Above the zone nothing breaks, but a hotter
reference also suppresses a little more of the audience (about 1–2 dB near
0 dBFS), so there is no benefit in going further.

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
