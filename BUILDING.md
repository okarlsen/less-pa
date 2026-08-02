# Building Less PA from source

Less PA builds on macOS (Apple Silicon) and produces an AU component and a
VST3 bundle. It is a two-stage build: Meson builds the patched WebRTC audio
processing library, then CMake builds the JUCE plugin against it.

## Prerequisites

- **Xcode Command Line Tools** — `xcode-select --install`
- **CMake** 3.22 or newer, **Meson** 0.63 or newer, **Ninja**, and
  **pkg-config**:

  ```sh
  brew install cmake meson ninja pkgconf
  ```

Nothing else needs installing. In particular you do **not** need Homebrew's
`abseil` — see [A note on Abseil](#a-note-on-abseil) below for why that
matters.

An internet connection is needed the first time you configure Meson: it
downloads the Abseil source tarball declared in
`webrtc-audio-processing/subprojects/abseil-cpp.wrap`.

## Get the source

Both dependencies (`JUCE/` and `webrtc-audio-processing/`) are git submodules,
so clone recursively:

```sh
git clone --recurse-submodules https://github.com/okarlsen/less-pa.git
cd less-pa
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

## Stage 1 — build the WebRTC audio processing library

```sh
cd webrtc-audio-processing
meson setup build \
    --default-library=static \
    --force-fallback-for=abseil-cpp \
    -Dc_args=-mmacosx-version-min=13.0 \
    -Dcpp_args=-mmacosx-version-min=13.0 \
    -Dc_link_args=-mmacosx-version-min=13.0 \
    -Dcpp_link_args=-mmacosx-version-min=13.0
meson compile -C build
```

Every option there is load-bearing:

- `--default-library=static` and `--force-fallback-for=abseil-cpp` keep the
  plugin self-contained — see [A note on Abseil](#a-note-on-abseil).
- The four `-mmacosx-version-min=13.0` options match the plugin's own
  deployment target (macOS 13, set in `plugin/CMakeLists.txt`). Meson
  otherwise builds for whatever macOS the build machine runs, which produces
  a plugin that silently refuses to load on anything older. Setting them as
  Meson options rather than via a `MACOSX_DEPLOYMENT_TARGET` environment
  variable means they are recorded in the build directory and apply to
  `meson compile` and to the Abseil subproject too.

This produces `build/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.a`
plus an uninstalled pkg-config file under `build/meson-uninstalled/`, which is
what the CMake stage consumes — there is no `meson install` step.

## Stage 2 — build the plugin

```sh
cd ../plugin
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PAEchoCanceller_AU PAEchoCanceller_VST3 -j8
```

Configure output should include:

```
-- Less PA: linking statically against the vendored Abseil subproject
```

If instead you get a warning that no vendored Abseil was found, stage 1 was
configured without `--force-fallback-for=abseil-cpp`. Delete
`webrtc-audio-processing/build` and redo stage 1.

The build products land in:

```
plugin/build/PAEchoCanceller_artefacts/Release/AU/Less PA.component
plugin/build/PAEchoCanceller_artefacts/Release/VST3/Less PA.vst3
```

`COPY_PLUGIN_AFTER_BUILD` is enabled, so both are also copied into
`~/Library/Audio/Plug-Ins/Components` and `~/Library/Audio/Plug-Ins/VST3`
automatically at the end of the build. A DAW may need a rescan to pick up a
newly built version.

## A note on Abseil

`webrtc-audio-processing` resolves its Abseil dependency (`absl_base`,
`absl_flags`, `absl_strings`, …) through pkg-config. On a machine that happens
to have Homebrew's `abseil` formula installed, plain `meson setup build` will
silently pick that up and link the plugin against roughly fifty dynamic
libraries under `/opt/homebrew/opt/abseil/lib`. That builds and runs fine
locally, but the resulting `.component`/`.vst3` will fail to load on any Mac
without that exact Homebrew installation — Homebrew's Abseil is shipped
dylib-only, so there is no static-linking escape hatch.

`--force-fallback-for=abseil-cpp` makes Meson ignore any system Abseil and
build the pinned `abseil-cpp` subproject as static archives instead, and
`--default-library=static` does the same for the WebRTC library itself. The
result is a plugin binary with no non-system dynamic dependencies at all,
which you can check with:

```sh
otool -L "plugin/build/PAEchoCanceller_artefacts/Release/AU/Less PA.component/Contents/MacOS/Less PA" \
  | grep -v "/System/Library\|/usr/lib"
```

That should print only the file's own name.

In the Abseil-subproject case upstream's `meson.build` deliberately generates
a pkg-config file with an empty `Requires:`, so `plugin/CMakeLists.txt` picks
the subproject's `libabsl_*.a` archives up by glob and adds them to the link
line itself.

## Verifying a build

The project has a standalone regression harness that drives the audio
processor directly at every supported sample rate with irregular block sizes:

```sh
cmake --build build --target PAEchoCancellerVerify -j8
./build/PAEchoCancellerVerify_artefacts/Release/PAEchoCancellerVerify
```

It must report 50 `PASS` results and end with `ALL TESTS PASS`.

For the AU, Apple's own validation tool should also succeed:

```sh
auval -v aufx LesP Sgtm
```

Look for `AU VALIDATION SUCCEEDED`.

## Code signing

Builds are ad-hoc signed (`Signature=adhoc` from `codesign -dv`), not signed
with an Apple Developer ID and not notarized. That is sufficient for locally
built plugins and for plugins installed by the release `.pkg`; it is why the
downloaded installer itself needs a one-time right-click → **Open**.

## Building the installer

With a completed Release build in place:

```sh
./packaging/build_installer.sh
```

This writes `packaging/build/Less-PA-<version>.pkg`, containing the AU and
VST3 as two components that install into the current user's
`~/Library/Audio/Plug-Ins` — no administrator password required.

## Not currently supported

Intel/universal builds, Windows, Linux, and AAX. The plugin is Apple Silicon,
AU + VST3 only.
