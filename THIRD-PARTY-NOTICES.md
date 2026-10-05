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

## AAX SDK (not used in this build)

Earlier development builds of Less PA linked against Avid's proprietary AAX
SDK to support Pro Tools. The public 1.0.0 release does not include AAX and
does not build against or redistribute any part of the AAX SDK.

## Earlier versions

Less PA 1.0.x used Google's WebRTC audio processing module (AEC3, BSD-3-Clause)
via a patched fork of freedesktop.org's `webrtc-audio-processing`
(https://github.com/okarlsen/webrtc-audio-processing), which in turn used
Abseil (Apache License 2.0). From 1.1.0 neither is part of the source tree or
the build.
