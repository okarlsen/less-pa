# Setup Guide: PA-Leakage Cancelling Plugin (AU / VST3 / AAX, macOS)

This covers everything to install and set up before we start writing code. It's organized as: what the plugin actually needs to do (this shapes the setup), accounts to create, software to install, and the order in which to build things.

## 1. How this maps onto AEC3

AEC3 (WebRTC's Acoustic Echo Canceller v3) is built for telephony: it takes a **far-end reference** signal (what's about to come out of a speaker) and a **near-end microphone** signal (what that speaker's sound plus the person's voice sounds like after room reflection), and subtracts the predictable part out. Your case — PA leakage into audience mics — is the same problem with a different name: the "far-end reference" is your PA feed, and the "near-end mic" is each audience microphone.

**Why AEC3 over the alternatives:** This is a *known-reference* cancellation problem — you have the exact PA feed in hand and want to subtract it from a mic that also picked it up, while leaving everything else in that mic (audience noise, clapping, screaming) untouched. That's a different and easier problem than *blind* mic-bleed removal (e.g. Auphonic's Mic Bleed Remover or iZotope RX's de-bleed tool), which infers leakage across multiple mics with no clean reference for any one source — not applicable here, and not a fair comparison to AEC3.

Apple's built-in Voice Processing I/O audio unit was considered and ruled out — it isn't a true adaptive canceller (no delay/tuning parameters), and it's wired into its own hardware render/capture loop rather than accepting two independent arbitrary streams, so it can't take a separate PA feed + mic input the way this plugin needs to. Speex's older echo canceller still exists but is generally weaker and no longer actively developed. Deep-learning cancellers (Microsoft's DeepVQE, Meta-AF, DTLN-aec) can beat classic adaptive filters on hard cases, but they're research code needing an embedded ONNX/PyTorch runtime — a much bigger lift, worth revisiting later as an upgrade, not a starting point.

AEC3 is the same algorithm family as the AEC block in professional AV DSPs (Biamp Tesira, QSC Q-SYS, BSS, ClearOne): an adaptive filter that models the transfer function between reference and mic, predicts the echo, subtracts it, and cleans up the linear-model residue with a nonlinear suppressor. If you've already gotten this working via Tesira's AEC routed over Dante, that's direct proof the architecture fits this exact PA-into-audience-mic scenario — AEC3 is that same approach, open-source and embeddable in your own plugin.

One thing this means for the plugin design from the start: AEC3 needs the *actual PA feed signal* as a second input, not just the mic. So the plugin will need a **sidechain/reference input**, not just a single in/out. Most DAWs and NLEs (Pro Tools, Reaper, Logic) support sidechain inputs on plugins, so this is normal, but it's worth knowing now because it affects the plugin's bus layout from the first line of code. Sample-accurate alignment between the reference and mic (latency) also matters a lot for AEC3's delay estimator — worth keeping in mind when you test.

## 2. Accounts you'll need

**Apple Developer Program** — $99/year, at developer.apple.com. Needed to get a Developer ID certificate for code-signing and notarizing the AU (and VST3/AAX) so macOS and Logic will actually load it without Gatekeeper complaints. Set this up early since approval can take a day or two.

**Avid Developer account** — free, at developer.avid.com/aax. Click through the license agreement to get SDK download access.

Signing and copy-protection/licensing are two separate things that both happen to go through PACE, and it's worth keeping them apart: **signing** is mandatory — every AAX binary, free or commercial, has to be PACE-signed or Pro Tools won't load it. **Licensing/DRM** (iLok-based authorization, serials, etc.) is optional and separate — plenty of developers ship signed-but-unlocked plugins with no license check at all, which is what you want here.

Recommended path for a free, friends-only plugin: once you're registered in the Avid Developer Program, PACE issues you the Eden signing tools for free (normally a $500/year product). Historically this needed a physical iLok USB on the signing machine, but PACE also now offers a **Cloud Signing Service** that skips the physical dongle. You sign the build yourself, and your friends load it in their completely normal, everyday Pro Tools — no iLok, no special build, on their end at all. The only cost is that you re-sign each time you build a new version.

For your own early testing before you've set up signing, Avid also provides a free **Pro Tools Developer** build that loads unsigned AAX plugins directly — useful to validate the plugin works before bothering with the signing setup, but not something to hand to friends since it's a different build from regular Pro Tools.

**Steinberg VST3** — no account/NDA needed anymore. The VST3 SDK is free and open (dual GPLv3/proprietary license), and JUCE bundles/fetches it for you automatically, so there's nothing to separately sign up for here.

**JUCE account** — free, at juce.com. JUCE's personal/free tier is fine for development and even shipping while under their revenue threshold; you can switch to a paid tier later if this becomes commercial.

## 3. Software to install (macOS)

Install in this order:

1. **Xcode**, from the App Store, then open it once to let it finish installing components. Afterward, install the command-line tools: `xcode-select --install`.
2. **Homebrew** (if you don't have it): `/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"`.
3. **Core build tools**, via Homebrew: `brew install cmake ninja meson pkg-config git`.
4. **abseil** (a Google C++ library that webrtc-audio-processing depends on): `brew install abseil`.
5. **JUCE**: clone it rather than using the installer, so you can pin a version: `git clone https://github.com/juce-framework/JUCE.git`. This gives you both the Projucer app (a GUI project generator) and full CMake support. Given your coding level, I'd recommend using JUCE's CMake integration rather than Projucer/Xcode-project-generation — one config file, easier to keep in sync as we add the WebRTC library, and easier for me to help you edit later.

You do **not** need to separately download the VST3 or AAX SDKs by hand — JUCE's CMake setup pulls in VST3 automatically, and for AAX you just point JUCE at the AAX SDK folder you downloaded from developer.avid.com once you have it.

## 4. Building the AEC3 library (webrtc-audio-processing)

The library you want is the PulseAudio-maintained extraction of WebRTC's audio processing module (this is the one that ships AEC3 as the default canceller, with the telephony-specific cruft trimmed out). It lives on freedesktop.org's GitLab, mirrored on GitHub:

```
git clone https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing.git
cd webrtc-audio-processing
meson setup build --default-library=static
ninja -C build
```

That produces a static `libwebrtc-audio-processing` you can link straight into the JUCE plugin's CMake target. We'll disable the parts of the module you don't need (noise suppression, AGC, VAD) and just drive the AEC3 submodule directly once we're inside the code.

## 5. Test hosts (you already have these)

Good — Pro Tools, Logic, and Reaper cover all three formats. Practical order for testing as we build: **Reaper first** (fastest iteration, loads unsigned VST3/AU builds with no fuss, and has flexible routing for feeding a sidechain reference), then **Logic** for AU validation, then **Pro Tools Developer** (not retail Pro Tools) last for AAX — this free Avid build loads unsigned AAX plugins directly, which fits a free/friends-only project with no signing detour needed.

## 6. Suggested build order

1. Get a trivial JUCE "pass-through" plugin building and loading as both AU and VST3 in Reaper. This proves the whole toolchain (JUCE + CMake + Xcode + code signing) before any DSP is involved.
2. Build `webrtc-audio-processing` standalone and write a tiny offline test program that feeds it two WAV files (a fake "PA reference" and a "mic with leakage") to confirm AEC3 actually cancels the leakage before it's anywhere near a plugin UI.
3. Add a sidechain input bus to the JUCE plugin and wire the main mic input + sidechain reference into AEC3's `ProcessStream`, outputting the cleaned mic signal.
4. Test in Reaper with real or recorded PA/audience-mic material, tune AEC3's delay/settings.
5. Apply for the AAX SDK, wrap the plugin for AAX via JUCE, and test in **Pro Tools Developer** first (free from Avid, loads unsigned builds — good for quick validation before setting up signing).
6. Register in the Avid Developer Program, get PACE's free Eden signing tools (or use their Cloud Signing Service), and sign the AAX build unlocked/DRM-free — then your friends can run it in their normal retail Pro Tools, no iLok needed on their end.
7. Set up Developer ID signing and notarization for the AU/VST3 builds so macOS Gatekeeper doesn't block them on your friends' machines.

Once you've got Xcode, Homebrew, JUCE, and the webrtc-audio-processing build done, we can start on step 1 together.

---

### Sources
- [AAX SDK — developer.avid.com](https://developer.avid.com/aax/)
- [AAX developer forum thread — JUCE forum](https://forum.juce.com/t/aax-developer-have-you-got-any-advices-before-applying-to-avid-partnership/44447)
- [Getting AAX signed, Avid test — HISE forum](https://forum.hise.audio/topic/6195/getting-aax-signed-avid-test-etc)
- [JUCE framework — GitHub](https://github.com/juce-framework/JUCE)
- [Projucer getting-started tutorial — juce.com](https://juce.com/tutorials/tutorial_new_projucer_project/)
- [JUCE + CMake + VS Code on macOS — JUCE forum](https://forum.juce.com/t/how-to-set-up-a-juce-cmake-vs-code-workflow-on-macos/66884)
- [WebRTC AudioProcessing (PulseAudio project page)](https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/)
- [webrtc-audio-processing meson build reference — GitHub mirror](https://github.com/voysys/webrtc-audio-processing/blob/master/meson.build)
