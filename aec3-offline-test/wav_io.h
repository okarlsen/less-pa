#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Minimal 16-bit PCM mono/stereo WAV reader/writer. No external deps.
struct WavFile {
    uint32_t sampleRate = 0;
    uint16_t numChannels = 0;
    std::vector<int16_t> samples; // interleaved
};

inline WavFile readWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open " + path);

    char riff[4];
    f.read(riff, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0) throw std::runtime_error(path + ": not a RIFF file");

    uint32_t chunkSize;
    f.read(reinterpret_cast<char*>(&chunkSize), 4);

    char wave[4];
    f.read(wave, 4);
    if (std::strncmp(wave, "WAVE", 4) != 0) throw std::runtime_error(path + ": not a WAVE file");

    WavFile result;
    uint16_t bitsPerSample = 0;
    bool haveFmt = false;

    while (f.good() && !f.eof()) {
        char id[4];
        f.read(id, 4);
        if (f.eof()) break;
        uint32_t size;
        f.read(reinterpret_cast<char*>(&size), 4);

        if (std::strncmp(id, "fmt ", 4) == 0) {
            uint16_t audioFormat;
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&result.numChannels), 2);
            f.read(reinterpret_cast<char*>(&result.sampleRate), 4);
            uint32_t byteRate;
            f.read(reinterpret_cast<char*>(&byteRate), 4);
            uint16_t blockAlign;
            f.read(reinterpret_cast<char*>(&blockAlign), 2);
            f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (size > 16) f.seekg(size - 16, std::ios::cur);
            haveFmt = true;
        } else if (std::strncmp(id, "data", 4) == 0) {
            if (!haveFmt) throw std::runtime_error(path + ": data chunk before fmt chunk");
            if (bitsPerSample != 16) throw std::runtime_error(path + ": only 16-bit PCM supported");
            result.samples.resize(size / 2);
            f.read(reinterpret_cast<char*>(result.samples.data()), size);
        } else {
            f.seekg(size, std::ios::cur);
        }
        if (size % 2 == 1) f.seekg(1, std::ios::cur); // chunks are word-aligned
    }

    if (!haveFmt) throw std::runtime_error(path + ": missing fmt chunk");
    return result;
}

inline void writeWav(const std::string& path, const WavFile& wav) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot create " + path);

    uint32_t dataSize = static_cast<uint32_t>(wav.samples.size() * sizeof(int16_t));
    uint32_t byteRate = wav.sampleRate * wav.numChannels * 2;
    uint16_t blockAlign = static_cast<uint16_t>(wav.numChannels * 2);
    uint16_t bitsPerSample = 16;
    uint32_t fmtChunkSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint32_t riffChunkSize = 4 + (8 + fmtChunkSize) + (8 + dataSize);

    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&riffChunkSize), 4);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
    f.write(reinterpret_cast<const char*>(&audioFormat), 2);
    f.write(reinterpret_cast<const char*>(&wav.numChannels), 2);
    f.write(reinterpret_cast<const char*>(&wav.sampleRate), 4);
    f.write(reinterpret_cast<const char*>(&byteRate), 4);
    f.write(reinterpret_cast<const char*>(&blockAlign), 2);
    f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&dataSize), 4);
    f.write(reinterpret_cast<const char*>(wav.samples.data()), dataSize);
}
