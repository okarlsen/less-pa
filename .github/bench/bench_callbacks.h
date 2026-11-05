// Temporary CPU benchmark (not part of the plugin): the processor at a
// 64-sample host buffer, 48 kHz, mono, Tail Length 800 ms. Reports the
// average load and the busiest callbacks as a share of the buffer period,
// which is what a DAW's CPU meter shows.
#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

static int runCallbackBench() {
    const int fs = 48000, bs = 64;
    struct Case { const char* name; bool pa; double from, to; };
    const Case cases[] = { { "first 30 s of PA (start-up)", true, 0.0, 30.0 },
                           { "PA, 40-60 s (steady)", true, 40.0, 60.0 },
                           { "no PA, mic only, 40-60 s", false, 40.0, 60.0 } };
    const double periodS = static_cast<double>(bs) / fs;
    for (const auto& c : cases) {
        PAEchoCancellerAudioProcessor proc;
        setMonoLayout(proc);
        proc.setNonRealtime(false);
        proc.prepareToPlay(fs, bs);
        const int totalChannels = std::max(proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer(totalChannels, bs);
        juce::MidiBuffer midi;
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.0f, 0.1f);
        std::vector<float> refHist(static_cast<size_t>(fs), 0.0f);
        size_t w = 0;
        const int d = fs / 100;
        std::vector<double> times;
        const int64_t start = static_cast<int64_t>(c.from * fs), end = static_cast<int64_t>(c.to * fs);
        for (int64_t pos = 0; pos < end; pos += bs) {
            buffer.clear();
            auto mainIn = proc.getBusBuffer(buffer, true, 0);
            auto refIn = proc.getBusBuffer(buffer, true, 1);
            for (int i = 0; i < bs; ++i) {
                const float r = c.pa ? 2.0f * nd(rng) : 0.0f;
                refHist[w % refHist.size()] = r;
                const float echo = 0.5f * refHist[(w + refHist.size() - static_cast<size_t>(d)) % refHist.size()];
                ++w;
                mainIn.setSample(0, i, echo + nd(rng));
                if (refIn.getNumChannels() > 0) refIn.setSample(0, i, r);
            }
            const auto t0 = std::chrono::steady_clock::now();
            proc.processBlock(buffer, midi);
            const auto t1 = std::chrono::steady_clock::now();
            if (pos >= start) times.push_back(std::chrono::duration<double>(t1 - t0).count());
        }
        double sum = 0.0;
        for (double t : times) sum += t;
        std::sort(times.begin(), times.end());
        const double p99 = times[static_cast<size_t>(0.99 * (times.size() - 1))];
        printf("  %-30s average %5.1f%%   busiest 1%% of buffers %5.1f%% of the 64-sample buffer time\n",
               c.name, 100.0 * sum / (times.size() * periodS), 100.0 * p99 / periodS);
    }
    return 0;
}
