#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <vector>

// Fixed-capacity single-channel circular buffer of floats. Used to bridge
// between the host's arbitrary per-block sample counts and the canceller's
// fixed frame size, adding no more buffering than that gap requires.
// Not thread-safe -- assumes single-threaded use from the audio callback.
//
// The plugin sizes and primes every FIFO so it can never overflow or run
// dry. If a change ever gets that arithmetic wrong, the symptom is a
// permanent latency shift, so both cases assert in debug builds and are
// counted for the verify harness (getXrunCount).
class FrameFifo {
public:
    void setCapacity(int numSamples) {
        buffer.assign(static_cast<size_t>(numSamples), 0.0f);
        capacity = numSamples;
        writeIndex = 0;
        readIndex = 0;
        used = 0;
        xruns = 0;
    }

    void reset() {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        readIndex = 0;
        used = 0;
        xruns = 0;
    }

    // Samples dropped on a full write plus samples padded on an empty read
    // since setCapacity/reset. Zero in correct use.
    uint64_t getXrunCount() const { return xruns; }

    int availableToRead() const { return used; }
    int freeSpace() const { return capacity - used; }

    void write(const float* src, int numSamples) {
        const int n = std::min(numSamples, freeSpace());
        jassert(n == numSamples); // overflow: samples dropped
        xruns += static_cast<uint64_t>(numSamples - n);
        for (int i = 0; i < n; ++i) {
            buffer[static_cast<size_t>(writeIndex)] = src[i];
            writeIndex = (writeIndex + 1) % capacity;
        }
        used += n;
    }

    void read(float* dst, int numSamples) {
        const int n = std::min(numSamples, used);
        jassert(n == numSamples); // underrun: padded with silence
        xruns += static_cast<uint64_t>(numSamples - n);
        for (int i = 0; i < n; ++i) {
            dst[i] = buffer[static_cast<size_t>(readIndex)];
            readIndex = (readIndex + 1) % capacity;
        }
        for (int i = n; i < numSamples; ++i)
            dst[i] = 0.0f; // underrun: pad with silence
        used -= n;
    }

private:
    std::vector<float> buffer;
    int capacity = 0;
    int writeIndex = 0;
    int readIndex = 0;
    int used = 0;
    uint64_t xruns = 0;
};
