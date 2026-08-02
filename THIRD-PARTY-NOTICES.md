# Third-party notices

Less PA is built on top of the following third-party components.

## JUCE

Less PA uses the [JUCE](https://juce.com) framework (version 9.0.0), vendored
unmodified as a git submodule at `JUCE/`. JUCE is dual-licensed under the
GNU Affero General Public License v3 (AGPLv3) and a commercial license from
Raw Material Software Limited. This project uses the AGPLv3 terms, which is
why Less PA itself is distributed under the AGPLv3 (see `LICENSE`). Full
terms: https://juce.com/legal/juce-9-licence/, and `JUCE/LICENSE.md` in the
submodule.

## WebRTC audio processing (AEC3)

Less PA's echo cancellation is provided by Google's WebRTC audio processing
module (the AEC3 algorithm), via
[freedesktop.org's `webrtc-audio-processing`](https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing)
packaging of it. This project vendors a **patched fork** as a git submodule
at `webrtc-audio-processing/`, published at
https://github.com/okarlsen/webrtc-audio-processing. The patches (see that
repository's own git history for the commits on top of upstream) change how
AEC3's suppression-gain tuning applies parameter updates live, and add a
crossfade between suppressor tunings when the near-end detector's decision
changes.

WebRTC audio processing is licensed under BSD-3-Clause (Copyright (c)
Google Inc. and The WebRTC project authors). Full text:
`webrtc-audio-processing/COPYING` in the submodule.

## AAX SDK (not used in this build)

Earlier development builds of Less PA linked against Avid's proprietary AAX
SDK to support Pro Tools. The public 1.0.0 release does not include AAX and
does not build against or redistribute any part of the AAX SDK.

## Abseil

The WebRTC audio processing library depends on
[Abseil](https://github.com/abseil/abseil-cpp) (version 20240722.0), which
release builds compile from source as a Meson subproject and link statically
into the plugin. Abseil is licensed under the Apache License 2.0 (Copyright
Google LLC). Full text:
https://github.com/abseil/abseil-cpp/blob/master/LICENSE
