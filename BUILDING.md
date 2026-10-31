# Building Less PA from source

Less PA builds on macOS and produces an AU component and a VST3 bundle. A
Windows VST3 is built by GitHub for each release; see [Windows (VST3)](#windows-vst3). It is a single CMake build of the JUCE plugin; the echo
canceller is the plugin's own code (`plugin/Source/KalmanEchoCanceller.h`),
with no external DSP library to build first.

## Prerequisites

- **Xcode Command Line Tools** — `xcode-select --install`
- **CMake** 3.22 or newer:

  ```sh
  brew install cmake
  ```

## Get the source

JUCE is a git submodule, so clone recursively:

```sh
git clone --recurse-submodules https://github.com/okarlsen/less-pa.git
cd less-pa
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

## Build the plugin

```sh
cd plugin
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PAEchoCanceller_AU PAEchoCanceller_VST3 -j8
```

The deployment target is macOS 13 and the build is universal (arm64 and
x86_64), both set in `plugin/CMakeLists.txt`. The packaging scripts refuse
binaries built for anything else, and the installer refuses to run on a
macOS older than the deployment target. For a faster local build of one
architecture only, configure with `-DCMAKE_OSX_ARCHITECTURES=arm64`; such a
build can't be packaged.

The Intel part runs on Apple Silicon under Rosetta, which is how it is
tested: `arch -x86_64 <command>` runs a universal binary's Intel part, for
example `arch -x86_64 auval -v aufx LesP Sgtm`.

The build products land in:

```
plugin/build/PAEchoCanceller_artefacts/Release/AU/Less PA.component
plugin/build/PAEchoCanceller_artefacts/Release/VST3/Less PA.vst3
```

Both are also copied into `~/Library/Audio/Plug-Ins/Components` and
`~/Library/Audio/Plug-Ins/VST3` automatically at the end of the build. A DAW
may need a rescan to pick up a newly built version. For a scratch or test
build that must not replace the installed plugins, configure with
`-DLESSPA_INSTALL_AFTER_BUILD=OFF`.

A test build for listening before a release can carry a label after the
version, shown in the panel and its tooltip: `-DLESSPA_BUILD_LABEL=PR12`
shows "v1.1.5 PR12". The version number itself stays numeric, since hosts
read it as major.minor.patch. Leave it empty (the default) for releases.

### AAX (Pro Tools)

JUCE includes the AAX SDK, so an AAX build needs nothing extra:

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DLESSPA_BUILD_AAX=ON
cmake --build build --target PAEchoCanceller_AAX -j8
```

It is off by default. Pro Tools only loads an AAX plugin that has been
signed with PACE's wraptool. PACE's tools are confidential and not part of
this repository, so the packaging scripts never sign the AAX themselves:
they include an AAX in the installer and the zip only if the one in the
build folder is already PACE-signed, notarized and stapled. With
`LESSPA_INSTALL_AFTER_BUILD` on, the build also copies the plugin into
`/Library/Application Support/Avid/Audio/Plug-Ins`.

The plugin links nothing outside the system frameworks, which you can check
with:

```sh
otool -L "plugin/build/PAEchoCanceller_artefacts/Release/AU/Less PA.component/Contents/MacOS/Less PA" \
  | grep -v "/System/Library\|/usr/lib"
```

That should print only the file's own name.

## Verifying a build

The project has a standalone regression harness that drives the audio
processor directly at every supported sample rate with irregular block sizes:

```sh
cmake --build build --target PAEchoCancellerVerify -j8
./build/PAEchoCancellerVerify_artefacts/Release/PAEchoCancellerVerify
```

It must report 66 individual `PASS` results and end with `ALL TESTS PASS`.
(`grep -c PASS` reports 67, because it counts the closing `ALL TESTS PASS`
line as well.) `--mic-silence` runs only the tests of a mic that is digitally
silent at the start or mid-session, for quick iteration on the canceller.

To check the plugin's real delay against what it reports to the host across
every sample rate (44.1 to 192 kHz), buffer sizes from 16 to 2048 samples,
Mix settings, bypass and offline rendering, run the full latency sweep (a
few minutes):

```sh
./build/PAEchoCancellerVerify_artefacts/Release/PAEchoCancellerVerify --latency-matrix
```

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

`packaging/signing.sh` holds the shared logic and `packaging/release_checks.sh`
the release-build checks; both packaging scripts source them. Running either
one will:

1. Refuse a build folder that is not a release build: configured with a test
   label (`LESSPA_BUILD_LABEL`, read from `plugin/build/lesspa_build_label.txt`,
   which CMake writes at configure time), not configured as Release, or with
   bundles whose version is not the one in `plugin/CMakeLists.txt` (built
   before a version bump). Then verify the bundles link nothing outside
   `/System/Library` and `/usr/lib`, and are universal with the right
   minimum macOS.
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

This writes `packaging/build/Less-PA-<version>.pkg`, containing the AU, the
VST3 and (when a PACE-signed one is present) the AAX as separate components
the user can tick or untick. They install system-wide, into
`/Library/Audio/Plug-Ins` and `/Library/Application Support/Avid/Audio/Plug-Ins`,
so the installer asks for an administrator password; Pro Tools scans no
per-user folder. The AU and VST3 components remove an older per-user copy
from `~/Library/Audio/Plug-Ins`, where versions up to 1.1.2 installed. The
package is signed, notarized and stapled as described above, so it installs
with no Gatekeeper detour.

```sh
./packaging/build_zip.sh
```

writes `packaging/build/Less-PA-<version>.zip` — the same two bundles for
people who would rather copy them into place by hand. Because the bundles are
stapled individually, they load with no quarantine-clearing step either.

The two scripts can be run in either order; neither disturbs the other's
output.

Note that the build copies the plugins to
`~/Library/Audio/Plug-Ins` at *build* time, which means those installed
copies are the ad-hoc signed ones. Re-copy them from
`plugin/build/PAEchoCanceller_artefacts/Release/` after running a packaging
script if you want to test against the signed build locally.

## Windows (VST3)

Windows builds are made by GitHub, not on a local machine: the
[Windows VST3 workflow](.github/workflows/windows-vst3.yml) runs on
`windows-latest` with Visual Studio. It builds the VST3 (64-bit Intel/AMD)
and the test harness, runs the full suite and the latency sweep
(`--latency-matrix`), and packages
`packaging/build/Less-PA-<version>-Windows-VST3.zip` with
`packaging/build_windows_zip.ps1`.

- On every pull request it builds and tests, and keeps the zip as a workflow
  artifact for a test install.
- When a GitHub release is published, it builds from the release tag and
  attaches the zip to that release. The tag must be a plain version
  (`v1.1.7`) equal to the version in `plugin/CMakeLists.txt`, and the build
  carries no test label; otherwise nothing is attached. A failed run can be
  rerun from the Actions tab; the upload replaces any earlier zip.

The same build runs on a Windows machine with Visual Studio 2022 or newer
and CMake:

```powershell
cmake -B plugin/build -S plugin -A x64 -DLESSPA_INSTALL_AFTER_BUILD=OFF -DLESSPA_BUILD_LABEL=
cmake --build plugin/build --config Release --target PAEchoCanceller_VST3 PAEchoCancellerVerify
pwsh packaging/build_windows_zip.ps1
```

The Windows VST3 is not code-signed. AU and AAX are macOS-only formats and
are not built on Windows.

## Not currently supported

Linux, and AAX on Windows.
