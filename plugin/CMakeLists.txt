cmake_minimum_required(VERSION 3.22)

# Default to Release unless the build directory was explicitly configured
# otherwise: this is a live-use plugin (also rendered in DAW timelines), not
# something run under a debugger day to day, so there's no reason to default
# to -O0 + JUCE assertions. Only takes effect on a *fresh* configure --
# CMAKE_BUILD_TYPE is cached per build directory, so an existing Debug build
# dir needs `cmake -DCMAKE_BUILD_TYPE=Release .` (or a fresh directory) to
# pick this up.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Without this, CMake defaults the deployment target to whatever macOS the
# build machine happens to run, which quietly produces a plugin that refuses
# to load on anything older. Released builds target macOS 13.
#
# webrtc-audio-processing has to be built to match -- see BUILDING.md, which
# sets MACOSX_DEPLOYMENT_TARGET for the Meson stage.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(PAEchoCanceller VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

set(WEBRTC_APM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../webrtc-audio-processing")

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${WEBRTC_APM_ROOT}/build/meson-uninstalled:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-2)

# Abseil. webrtc-audio-processing resolves absl_base/absl_flags/... through
# pkg-config, and on a machine with Homebrew's `abseil` installed it will
# happily pick that up -- which links the plugin against ~50 dylibs under
# /opt/homebrew/opt/abseil/lib that do not exist on anyone else's Mac, so the
# resulting .component/.vst3 simply fails to load once distributed. Release
# builds therefore configure Meson with --force-fallback-for=abseil-cpp
# (see BUILDING.md) so Abseil is built as static archives inside the Meson
# build tree.
#
# In that fallback case upstream's meson.build deliberately emits an empty
# `Requires:` in the generated .pc, and the webrtc static library leaves
# Abseil's out-of-line symbols undefined -- so the archives have to be added
# to the link line here.
file(GLOB WEBRTC_APM_ABSL_LIBS
    "${WEBRTC_APM_ROOT}/build/subprojects/abseil-cpp-*/libabsl_*.a")

if(WEBRTC_APM_ABSL_LIBS)
    message(STATUS "Less PA: linking statically against the vendored Abseil subproject")
else()
    message(WARNING
        "Less PA: no vendored Abseil found under ${WEBRTC_APM_ROOT}/build/subprojects. "
        "Falling back to whatever Abseil pkg-config resolves (typically Homebrew's "
        "shared libraries). The build will work on this machine but the resulting "
        "plugins are NOT redistributable -- see BUILDING.md.")
endif()

juce_add_plugin(PAEchoCanceller
    COMPANY_NAME "SGTM"
    BUNDLE_ID com.sgtm.LessPA
    PLUGIN_MANUFACTURER_CODE Sgtm
    PLUGIN_CODE LesP
    FORMATS AU VST3
    PRODUCT_NAME "Less PA"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

# The SGTM wordmark, compiled in as Assets::sgtm_logo_png. Underscores in the
# filename are load-bearing: juce_add_binary_data's identifier generator
# deletes hyphens rather than converting them.
#
# Linked into PAEchoCancellerVerify as well as the plugin -- that target
# compiles Source/PluginEditor.cpp too (to keep the editor linkable), so
# without it the build fails on unresolved Assets:: symbols.
juce_add_binary_data(LessPAAssets NAMESPACE Assets SOURCES Resources/sgtm_logo.png)

target_sources(PAEchoCanceller
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/LessPALookAndFeel.cpp
)

target_compile_definitions(PAEchoCanceller
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        WEBRTC_APM_DEBUG_DUMP=0
)

target_include_directories(PAEchoCanceller
    PRIVATE
        ${WEBRTC_APM_ROOT}
)

target_link_libraries(PAEchoCanceller
    PRIVATE
        LessPAAssets
        juce::juce_audio_utils
        juce::juce_dsp
        PkgConfig::WEBRTC_APM
        ${WEBRTC_APM_ABSL_LIBS}
        "-framework CoreFoundation"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Standalone harness that drives PAEchoCancellerAudioProcessor directly
# (no AU/VST3 host wrapper) to verify the frame accumulator and AEC3 wiring
# at all required sample rates with irregular block sizes.
juce_add_console_app(PAEchoCancellerVerify
    PRODUCT_NAME "Less PA Verify"
)

target_sources(PAEchoCancellerVerify
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/LessPALookAndFeel.cpp
        Source/verify_main.cpp
)

target_compile_definitions(PAEchoCancellerVerify
    PRIVATE
        JucePlugin_Name="Less PA"
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        WEBRTC_APM_DEBUG_DUMP=0
)

target_include_directories(PAEchoCancellerVerify
    PRIVATE
        ${WEBRTC_APM_ROOT}
)

target_link_libraries(PAEchoCancellerVerify
    PRIVATE
        LessPAAssets
        juce::juce_audio_utils
        juce::juce_dsp
        PkgConfig::WEBRTC_APM
        ${WEBRTC_APM_ABSL_LIBS}
        "-framework CoreFoundation"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
