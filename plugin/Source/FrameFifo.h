#pragma once

#include <algorithm>
#include <vector>

// Fixed-capacity single-channel circular buffer of floats. Used to bridge
// between the host's arbitrary per-block sample counts and AEC3's fixed
// ~10ms frame size, adding no more buffering than that gap requires.
// Not thread-safe -- assumes single-threaded use from the audio callback.
class FrameFifo {
public:
    void setCapacity(int numSamples) {
        buffer.assign(static_cast<size_t>(numSamples), 0.0f);
        capacity = numSamples;
        writeIndex = 0;
        readIndex = 0;
        used = 0;
    }

    void reset() {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        readIndex = 0;
        used = 0;
    }

    int availableToRead() const { return used; }
    int freeSpace() const { return capacity - used; }

    void write(const float* src, int numSamples) {
        const int n = std::min(numSamples, freeSpace());
        for (int i = 0; i < n; ++i) {
            buffer[static_cast<size_t>(writeIndex)] = src[i];
            writeIndex = (writeIndex + 1) % capacity;
        }
        used += n;
    }

    void read(float* dst, int numSamples) {
        const int n = std::min(numSamples, used);
        for (int i = 0; i < n; ++i) {
            dst[i] = buffer[static_cast<size_t>(readIndex)];
            readIndex = (readIndex + 1) % capacity;
        }
        for (int i = n; i < numSamples; ++i)
            dst[i] = 0.0f; // underrun (startup transient): pad with silence
        used -= n;
    }

private:
    std::vector<float> buffer;
    int capacity = 0;
    int writeIndex = 0;
    int readIndex = 0;
    int used = 0;
};
