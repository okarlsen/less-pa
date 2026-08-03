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

Download the latest `.zip` from the
[Releases page](https://github.com/okarlsen/less-pa/releases), unzip it, and
follow the included `INSTALL.txt` — copy the AU and/or VST3 into your
user's `~/Library/Audio/Plug-Ins/` (no admin password needed), then run one
`xattr` command to clear the quarantine flag macOS puts on anything
downloaded.

The build isn't code-signed with an Apple Developer ID yet, so that
quarantine-clearing step is a one-time requirement — without it, your DAW's
plugin scan will get blocked by Gatekeeper. A signed, notarized `.pkg`
installer that removes this step entirely is in progress.

## Using it

Wire your PA's feed (a send, a matrix output, whatever your desk can give
you) into the plugin's Reference input, and the audience mic into its main
input. Click the **?** button in the plugin for the full control reference.

## Building from source

See [BUILDING.md](BUILDING.md).

## License

Less PA is free to use, provided as-is with no warranty of any kind — see
[LICENSE](LICENSE) (AGPLv3). Third-party components are credited in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Copyright © 2026 SGTM.
