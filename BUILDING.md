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

It must report 49 individual `PASS` results and end with `ALL TESTS PASS`.
(`grep -c PASS` reports 50, because it counts the closing `ALL TESTS PASS`
line as well — that 50 is the figure earlier release notes quoted.)

For the AU, Apple's own validation tool should also succeed:

```sh
auval -v aufx LesP Sgtm
```

Look for `AU VALIDATION SUCCEEDED`.

## Code signing

A plain local build is ad-hoc signed (`Signature=adhoc` from `codesign -dv`),
which is fine for development on the machine that built it. Released
artifacts are signed with an Apple Developer ID and notarized by Apple; the
packaging scripts do that automatically, so a release is just a matter of
running them.

### What a release needs

Two certificates in the login keychain, both from the same team
(`ZVP9U3LWAJ`, "Sounds Good To Me AS"):

| Certificate | Signs |
| --- | --- |
| Developer ID **Application** | the `.component` and `.vst3` bundles |
| Developer ID **Installer** | the final `.pkg` |

Check they are present with:

```sh
security find-identity -v -p basic
```

Note the `-p basic`: the Installer certificate does **not** appear under
`security find-identity -v -p codesigning`, which lists only the Application
one. That absence is normal and not a sign of a missing certificate.

Plus a `notarytool` keychain profile, created once:

```sh
xcrun notarytool store-credentials notarytool \
    --apple-id <apple-id> --team-id ZVP9U3LWAJ --password <app-specific-password>
```

The password is an [app-specific
password](https://support.apple.com/en-us/102654), not the Apple ID password.
It is stored in the keychain by that command and is never read by, passed to,
or written by anything in this repository. Confirm the profile works with:

```sh
xcrun notarytool history --keychain-profile notarytool
```

All three identifiers can be overridden with the `LESSPA_SIGN_IDENTITY`,
`LESSPA_INSTALLER_IDENTITY` and `LESSPA_NOTARY_PROFILE` environment
variables, so a fork can sign with its own credentials without editing the
scripts.

### What the scripts do

`packaging/signing.sh` holds the shared logic; both packaging scripts source
it. Running either one will:

1. Verify the bundles link nothing outside `/System/Library` and `/usr/lib`
   (see [A note on Abseil](#a-note-on-abseil)).
2. Sign both bundles with the Developer ID Application certificate, with the
   hardened runtime (`--options runtime`) and a secure timestamp.
3. Submit both bundles to Apple in a **single** notarization submission —
   a ticket covers the cdhash of each binary in the archive, so one
   submission serves both — and staple the resulting ticket to each bundle.

`build_installer.sh` then additionally signs the finished `.pkg` with the
Developer ID Installer certificate, notarizes it, and staples that ticket
too. Only the combined product from `productbuild` is signed; the two
intermediate `pkgbuild` components are embedded into it and need no signature
of their own.

Step 3 is idempotent — it checks for a valid stapled ticket first — so
running both scripts against one build costs a single trip to the notary
service. This also matters for correctness, not just speed: stapling adds a
`Contents/CodeResources` ticket to the bundle, so a second `codesign --force`
pass would re-seal its resources and invalidate the ticket that had just been
stapled.

### No entitlements

The bundles are signed with the hardened runtime and **no entitlements
file**, which is deliberate. Each bundle contains exactly one Mach-O and no
nested code, and the plugin needs no special capability of its own — the
microphone access it operates on is declared and owned by the host
application. `auval` passes against the hardened, signed build, which is the
check that would fail if an entitlement were actually required.

### Verifying signed artifacts

```sh
codesign --verify --deep --strict --verbose=2 "<bundle>"
codesign --verify --strict --test-requirement="=notarized" "<bundle>"
xcrun stapler validate "<bundle>"
```

For a plugin bundle, use the `=notarized` requirement above rather than
`spctl -a -t exec`. `spctl`'s `exec` assessment only applies to applications
and reports `rejected (the code is valid but does not seem to be an app)` for
a `.component` or `.vst3` no matter how correctly it is signed — that message
is not a signing failure. `spctl` *is* the right tool for the installer:

```sh
pkgutil --check-signature "<pkg>"
xcrun stapler validate "<pkg>"
spctl -a -vvv -t install "<pkg>"   # expect: accepted, source=Notarized Developer ID
```

## Building the installer

With a completed Release build in place:

```sh
./packaging/build_installer.sh
```

This writes `packaging/build/Less-PA-<version>.pkg`, containing the AU and
VST3 as two components that install into the current user's
`~/Library/Audio/Plug-Ins` — no administrator password required. It is signed,
notarized and stapled as described above, so it installs with no Gatekeeper
detour.

```sh
./packaging/build_zip.sh
```

writes `packaging/build/Less-PA-<version>.zip` — the same two bundles for
people who would rather copy them into place by hand. Because the bundles are
stapled individually, they load with no quarantine-clearing step either.

The two scripts can be run in either order; neither disturbs the other's
output.

Note that `COPY_PLUGIN_AFTER_BUILD` copies the plugins to
`~/Library/Audio/Plug-Ins` at *build* time, which means those installed
copies are the ad-hoc signed ones. Re-copy them from
`plugin/build/PAEchoCanceller_artefacts/Release/` after running a packaging
script if you want to test against the signed build locally.

## Not currently supported

Intel/universal builds, Windows, Linux, and AAX. The plugin is Apple Silicon,
AU + VST3 only.
