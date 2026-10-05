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
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(PAEchoCanceller VERSION 1.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

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
)

target_link_libraries(PAEchoCanceller
    PRIVATE
        LessPAAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Standalone harness that drives PAEchoCancellerAudioProcessor directly
# (no AU/VST3 host wrapper) to verify the frame accumulator and canceller
# wiring at all required sample rates with irregular block sizes.
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
        JucePlugin_VersionString="${PROJECT_VERSION}"
    PUBLIC
        # --screenshot pumps the message loop so the editor's timer can run
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(PAEchoCancellerVerify
    PRIVATE
        LessPAAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
