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

project(PAEchoCanceller VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

set(WEBRTC_APM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../webrtc-audio-processing")

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${WEBRTC_APM_ROOT}/build/meson-uninstalled:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-2)

juce_set_aax_sdk_path("${CMAKE_CURRENT_SOURCE_DIR}/../../aax-sdk-2-9-0")

juce_add_plugin(PAEchoCanceller
    COMPANY_NAME "SGTM"
    BUNDLE_ID com.sgtm.LessPA
    PLUGIN_MANUFACTURER_CODE Sgtm
    PLUGIN_CODE LesP
    FORMATS AU VST3 AAX
    PRODUCT_NAME "Less PA"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(PAEchoCanceller
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
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
        juce::juce_audio_utils
        juce::juce_dsp
        PkgConfig::WEBRTC_APM
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
        juce::juce_audio_utils
        juce::juce_dsp
        PkgConfig::WEBRTC_APM
        "-framework CoreFoundation"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
