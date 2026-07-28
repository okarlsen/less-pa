// Standalone offline test: runs AEC3 (via WebRTC's AudioProcessing) over a
// synthetic "PA reference" + "mic with leakage" WAV pair and writes the
// cleaned mic signal to a third WAV, so we can confirm AEC3 actually
// cancels the leakage before it's anywhere near the plugin.

#include <webrtc/modules/audio_processing/include/audio_processing.h>

#include <cstdlib>
#include <iostream>

#include "wav_io.h"

namespace {
constexpr int kBlockMs = 10;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <reference.wav> <mic.wav> <cleaned_out.wav>" << std::endl;
        return EXIT_FAILURE;
    }

    WavFile reference = readWav(argv[1]);
    WavFile mic = readWav(argv[2]);

    if (reference.numChannels != 1 || mic.numChannels != 1)
        throw std::runtime_error("Only mono WAV files are supported");
    if (reference.sampleRate != mic.sampleRate)
        throw std::runtime_error("Reference and mic sample rates must match");

    const uint32_t sampleRate = reference.sampleRate;
    const int samplesPerBlock = sampleRate * kBlockMs / 1000;

    rtc::scoped_refptr<webrtc::AudioProcessing> apm = webrtc::AudioProcessingBuilder().Create();

    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false; // false selects AEC3
    // Everything else off, so this test isolates AEC3's own behaviour.
    config.gain_controller1.enabled = false;
    config.gain_controller2.enabled = false;
    config.high_pass_filter.enabled = false;
    config.noise_suppression.enabled = false;
    apm->ApplyConfig(config);

    webrtc::StreamConfig streamConfig(sampleRate, 1);

    WavFile cleaned;
    cleaned.sampleRate = sampleRate;
    cleaned.numChannels = 1;

    size_t numBlocks = std::min(reference.samples.size(), mic.samples.size()) / samplesPerBlock;
    cleaned.samples.reserve(numBlocks * samplesPerBlock);

    std::vector<int16_t> refBlock(samplesPerBlock);
    std::vector<int16_t> micBlock(samplesPerBlock);

    for (size_t b = 0; b < numBlocks; ++b) {
        std::copy_n(reference.samples.begin() + b * samplesPerBlock, samplesPerBlock, refBlock.begin());
        std::copy_n(mic.samples.begin() + b * samplesPerBlock, samplesPerBlock, micBlock.begin());

        apm->ProcessReverseStream(refBlock.data(), streamConfig, streamConfig, refBlock.data());
        apm->ProcessStream(micBlock.data(), streamConfig, streamConfig, micBlock.data());

        cleaned.samples.insert(cleaned.samples.end(), micBlock.begin(), micBlock.end());
    }

    writeWav(argv[3], cleaned);

    std::cout << "Processed " << numBlocks << " blocks (" << (numBlocks * kBlockMs) << " ms) at "
              << sampleRate << " Hz -> " << argv[3] << std::endl;

    return EXIT_SUCCESS;
}
