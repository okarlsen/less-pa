#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// Full-band partitioned-block frequency-domain Kalman filter (PB-FDKF) echo
// canceller with a low-latency Wiener suppressor -- the "Kalman" engine.
// Port of research/kalman/fdkf_x.py (the configuration tuned on the LS26 and
// Oslo Spektrum recordings), after Enzner & Vary 2006 / Kuech et al. 2014.
//
// Unlike AEC3, the adaptive filter covers the whole band (0-24 kHz at
// 48 kHz), and every bin's step size comes from the Kalman gain: how unsure
// that bin's filter is versus how loud the crowd is in it. So it keeps
// adapting under a permanently present crowd instead of freezing, and there
// is no near-end/double-talk detector to switch.
//
// Latency is fixed and small: one block (blockSize samples, the caller's
// frame) plus the suppressor's linear-phase FIR (suppressorDelaySamples).
// 128 + 64 = 192 samples (4.0 ms) at 48 kHz.
//
// Usage: prepare() off the audio thread (allocates), then processFrame()
// with exactly blockSize samples of every mic channel (in place) plus the
// matching reference block. Nothing on the processFrame() path allocates.
class KalmanEchoCanceller {
public:
    struct SuppressorSettings {
        float beta = 0.3f;       // residual-echo share of the echo estimate (model mismatch, PA distortion)
        float overSub = 2.0f;    // over-subtraction of the residual-echo estimate
        float floorDb = -18.0f;  // deepest per-bin cut
    };

    static int blockSizeForRate(double sampleRate) { return sampleRate > 64000.0 ? 256 : 128; }
    static constexpr int firLength = 128;
    static constexpr int suppressorDelaySamples = firLength / 2;

    void prepare(double sampleRate, int numMicChannels, double maxTailSeconds)
    {
        N = blockSizeForRate(sampleRate);
        M = 2 * N;
        K = N + 1;
        fftOrder = static_cast<int>(std::round(std::log2(static_cast<double>(M))));
        fft = std::make_unique<juce::dsp::FFT>(fftOrder);
        fs = sampleRate;
        maxPartitions = partitionsFor(maxTailSeconds);
        activePartitions = maxPartitions;

        const size_t pk = static_cast<size_t>(maxPartitions * K);
        xr.assign(pk, 0.0f); xi.assign(pk, 0.0f); x2.assign(pk, 0.0f);
        xbuf.assign(static_cast<size_t>(M), 0.0f);
        fftScratch.assign(static_cast<size_t>(2 * M), 0.0f);
        fftScratch2.assign(static_cast<size_t>(2 * M), 0.0f);
        yhr.assign(static_cast<size_t>(K), 0.0f); yhi.assign(static_cast<size_t>(K), 0.0f);

        channels.clear();
        channels.resize(static_cast<size_t>(std::max(1, numMicChannels)));
        for (auto& c : channels) {
            c.wr.assign(pk, 0.0f); c.wi.assign(pk, 0.0f); c.psi.assign(pk, psi0);
            c.psiS.assign(static_cast<size_t>(K), 1e-6f);
            c.py.assign(static_cast<size_t>(K), 1e-12f); c.pe.assign(static_cast<size_t>(K), 1e-12f);
            c.pout.assign(static_cast<size_t>(K), 1e-12f); c.gain.assign(static_cast<size_t>(K), 1.0f);
            c.h.assign(static_cast<size_t>(firLength), 0.0f); c.hPrev.assign(static_cast<size_t>(firLength), 0.0f);
            c.hist.assign(static_cast<size_t>(firLength - 1 + N), 0.0f);
            c.hasPrev = false;
        }
        unc.assign(static_cast<size_t>(K), 0.0f);
        er.assign(static_cast<size_t>(K), 0.0f); ei.assign(static_cast<size_t>(K), 0.0f);
        yr.assign(static_cast<size_t>(K), 0.0f); yi.assign(static_cast<size_t>(K), 0.0f);
        ypr.assign(static_cast<size_t>(K), 0.0f); ypi.assign(static_cast<size_t>(K), 0.0f);
        gl.assign(static_cast<size_t>(firLength / 2 + 1), 1.0f);
        timeScratch.assign(static_cast<size_t>(M), 0.0f);
        timeScratch2.assign(static_cast<size_t>(M), 0.0f);
        dScratch.assign(static_cast<size_t>(K), 0.0f);
        mScratch.assign(static_cast<size_t>(K), 0.0f);
        egScratchR.assign(static_cast<size_t>(K), 0.0f);
        egScratchI.assign(static_cast<size_t>(K), 0.0f);

        // Zero-phase FIR from firLength/2+1 gains: h[n] = irfft(G)[n - L/2] * hann.
        const int L = firLength, H = L / 2 + 1;
        cosTable.assign(static_cast<size_t>(L * H), 0.0f);
        for (int n = 0; n < L; ++n) {
            const double win = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * n / L);
            const int m = n - L / 2; // np.roll(irfft, L/2)
            for (int j = 0; j < H; ++j) {
                const double w = (j == 0 || j == H - 1) ? 1.0 : 2.0;
                cosTable[static_cast<size_t>(n * H + j)] =
                    static_cast<float>(win * w * std::cos(2.0 * juce::MathConstants<double>::pi * j * m / L) / L);
            }
        }
        reset();
    }

    // Clean restart (fresh convergence), keeping the allocation.
    void reset()
    {
        std::fill(xr.begin(), xr.end(), 0.0f); std::fill(xi.begin(), xi.end(), 0.0f);
        std::fill(x2.begin(), x2.end(), 0.0f); std::fill(xbuf.begin(), xbuf.end(), 0.0f);
        for (auto& c : channels) {
            std::fill(c.wr.begin(), c.wr.end(), 0.0f); std::fill(c.wi.begin(), c.wi.end(), 0.0f);
            std::fill(c.psi.begin(), c.psi.end(), psi0);
            std::fill(c.psiS.begin(), c.psiS.end(), 1e-6f);
            std::fill(c.py.begin(), c.py.end(), 1e-12f); std::fill(c.pe.begin(), c.pe.end(), 1e-12f);
            std::fill(c.pout.begin(), c.pout.end(), 1e-12f); std::fill(c.gain.begin(), c.gain.end(), 1.0f);
            std::fill(c.hist.begin(), c.hist.end(), 0.0f);
            c.hasPrev = false;
            c.accMic = 0.0;
            c.psiStart = psi0;
        }
        accRef = 0.0;
        accBlocks = 0;
        startDone = false;
        head = 0;
        blockCount = 0;
        delayEstimateMs.store(-1, std::memory_order_relaxed);
    }

    // Live: shorter tails drop the far partitions, longer ones start them from
    // zero. The near part of the filter (where most of the echo is) is kept,
    // so a Tail Length change needs no silence and no re-convergence.
    void setTailSeconds(double seconds)
    {
        const int p = std::clamp(partitionsFor(seconds), 1, maxPartitions);
        if (p > activePartitions)
            for (auto& c : channels)
                for (int q = activePartitions; q < p; ++q) {
                    std::fill_n(c.wr.begin() + q * K, K, 0.0f);
                    std::fill_n(c.wi.begin() + q * K, K, 0.0f);
                    std::fill_n(c.psi.begin() + q * K, K, c.psiStart);
                }
        activePartitions = p;
    }

    void setSuppressor(const SuppressorSettings& s) { supp = s; }

    int getBlockSize() const noexcept { return N; }

    // Echo path delay (PA to mic) read off the filter: the lag of its
    // strongest partition, refreshed ~4x/s. -1 until the filter has found
    // the PA in the mic. Safe to call from any thread.
    int getDelayEstimateMs() const noexcept { return delayEstimateMs.load(std::memory_order_relaxed); }
    int getLatencySamples() const noexcept { return N + suppressorDelaySamples; }

    // mic: numChannels pointers to N samples each, replaced by the output
    // (delayed by suppressorDelaySamples). ref: N samples.
    void processFrame(float* const* mic, const float* ref, int numChannels)
    {
        const int P = activePartitions;

        // Reference: newest 2N-sample window -> spectrum into the ring slot
        // for lag 0 (the ring rotates instead of shifting P*K values).
        std::copy(xbuf.begin() + N, xbuf.end(), xbuf.begin());
        std::copy_n(ref, N, xbuf.begin() + N);
        head = (head + maxPartitions - 1) % maxPartitions;
        forward(xbuf.data(), xr.data() + head * K, xi.data() + head * K);
        for (int k = 0; k < K; ++k) {
            const size_t i = static_cast<size_t>(head * K + k);
            x2[i] = xr[i] * xr[i] + xi[i] * xi[i];
        }

        const int numCh = std::min(numChannels, static_cast<int>(channels.size()));
        if (!startDone)
            updateStartUncertainty(mic, ref, numCh);

        for (int ch = 0; ch < numCh; ++ch)
            processChannel(channels[static_cast<size_t>(ch)], mic[ch], P);

        ++blockCount;
        if (blockCount % static_cast<uint64_t>(std::max(1.0, fs / N / 4.0)) == 0)
            updateDelayEstimate(P);
    }

private:
    struct Channel {
        std::vector<float> wr, wi, psi, psiS, py, pe, pout, gain, h, hPrev, hist;
        bool hasPrev = false;
        double accMic = 0.0;   // mic power summed over the PA-active start blocks
        float psiStart = psi0; // the uncertainty newly started partitions get
    };

    // Level-relative start. A fixed starting uncertainty only suits one
    // mic/PA level ratio: too small and the filter learns for minutes (LS26
    // 56-66 min: ~2 min behind Classic), too large and it can lose its lock
    // later (LS26 Pub 2). So once startSeconds of PA (ref blocks above
    // startRefPower) have been seen, the uncertainty restarts at
    // startScale * mic power / ref power -- the echo-path gain it may have to
    // learn. Tested on five recordings with the ref trimmed -10/0/+10 dB.
    void updateStartUncertainty(float* const* mic, const float* ref, int numCh)
    {
        double px = 0.0;
        for (int n = 0; n < N; ++n)
            px += static_cast<double>(ref[n]) * ref[n];
        px /= N;
        if (px <= startRefPower)
            return;
        accRef += px;
        for (int c = 0; c < numCh; ++c) {
            double py = 0.0;
            for (int n = 0; n < N; ++n)
                py += static_cast<double>(mic[c][n]) * mic[c][n];
            channels[static_cast<size_t>(c)].accMic += py / N;
        }
        if (++accBlocks * N < static_cast<int>(startSeconds * fs))
            return;
        for (auto& c : channels) {
            c.psiStart = std::max(psiFloor, static_cast<float>(startScale * c.accMic / accRef));
            std::fill(c.psi.begin(), c.psi.end(), c.psiStart);
        }
        startDone = true;
    }

    // fdkf_x.py defaults, tuned on the real recordings
    static constexpr float A2 = 0.99999f * 0.99999f;
    static constexpr float psi0 = 1e-3f;
    static constexpr float lam = 0.9f;          // crowd/noise PSD smoothing
    static constexpr float psiFloor = 1e-10f;
    static constexpr float cPad = 0.5f;         // |X|^2 of a 2N frame vs |E|^2 of N samples zero-padded
    static constexpr float guardSmooth = 0.9f;
    static constexpr float suppSmooth = 0.7f;
    static constexpr int constraintStride = 64; // gradient constraint on 1 partition in 64 per block
    static constexpr double startSeconds = 3.0;    // PA needed before the level-relative start
    static constexpr double startRefPower = 1e-5;  // a "PA-active" ref block: mean square above -50 dBFS
    static constexpr double startScale = 1.0;

    int partitionsFor(double seconds) const
    {
        return std::max(1, static_cast<int>(std::ceil(seconds * fs / N)));
    }

    void updateDelayEstimate(int P)
    {
        const auto& ch = channels.front();
        int best = -1;
        float bestEnergy = 0.0f, total = 0.0f;
        for (int p = 0; p < P; ++p) {
            float e = 0.0f;
            for (int k = 0; k < K; ++k) {
                const size_t i = static_cast<size_t>(p * K + k);
                e += ch.wr[i] * ch.wr[i] + ch.wi[i] * ch.wi[i];
            }
            total += e;
            if (e > bestEnergy) { bestEnergy = e; best = p; }
        }
        // "Found": one partition clearly stands out from an even spread
        const bool found = best >= 0 && total > 1e-12f && bestEnergy > 3.0f * total / static_cast<float>(P);
        delayEstimateMs.store(found ? static_cast<int>(std::lround(1000.0 * best * N / fs)) : -1,
                              std::memory_order_relaxed);
    }

    // The two per-partition inner loops, as free-standing kernels so the
    // compiler can vectorise them (inline in the nest it gives up on the
    // ring-buffer indexing).
    static void accumulatePartition(float* __restrict Yr, float* __restrict Yi, float* __restrict U,
                                    const float* __restrict Xr, const float* __restrict Xi,
                                    const float* __restrict X2, const float* __restrict Wr,
                                    const float* __restrict Wi, const float* __restrict Ps, int K) noexcept
    {
        for (int k = 0; k < K; ++k) {
            Yr[k] += Xr[k] * Wr[k] - Xi[k] * Wi[k];
            Yi[k] += Xr[k] * Wi[k] + Xi[k] * Wr[k];
            U[k] += Ps[k] * X2[k];
        }
    }

    static void updatePartition(float* __restrict Wr, float* __restrict Wi, float* __restrict Ps,
                                const float* __restrict Xr, const float* __restrict Xi,
                                const float* __restrict X2, const float* __restrict Er,
                                const float* __restrict Ei, const float* __restrict D, int K) noexcept
    {
        for (int k = 0; k < K; ++k) {
            const float g = Ps[k] * D[k];
            const float kr = g * Xr[k], ki = -g * Xi[k];
            const float wr = Wr[k] + kr * Er[k] - ki * Ei[k];
            const float wi = Wi[k] + kr * Ei[k] + ki * Er[k];
            Wr[k] = wr;
            Wi[k] = wi;
            Ps[k] = A2 * (1.0f - cPad * g * X2[k]) * Ps[k] + (1.0f - A2) * (wr * wr + wi * wi) + psiFloor;
        }
    }

    int lagIndex(int p) const { return (head + p) % maxPartitions; } // ring slot holding lag p

    // real[M] -> K complex bins
    void forward(const float* in, float* re, float* im)
    {
        std::copy_n(in, M, fftScratch.begin());
        std::fill(fftScratch.begin() + M, fftScratch.end(), 0.0f);
        fft->performRealOnlyForwardTransform(fftScratch.data(), true);
        for (int k = 0; k < K; ++k) {
            re[k] = fftScratch[static_cast<size_t>(2 * k)];
            im[k] = fftScratch[static_cast<size_t>(2 * k + 1)];
        }
    }

    // K complex bins -> real[M] (normalised like numpy's irfft)
    void inverse(const float* re, const float* im, float* out)
    {
        std::fill(fftScratch2.begin(), fftScratch2.end(), 0.0f);
        for (int k = 0; k < K; ++k) {
            fftScratch2[static_cast<size_t>(2 * k)] = re[k];
            fftScratch2[static_cast<size_t>(2 * k + 1)] = im[k];
        }
        fft->performRealOnlyInverseTransform(fftScratch2.data());
        std::copy_n(fftScratch2.begin(), M, out);
    }

    // spectrum of [N zeros, block]
    void forwardPadded(const float* block, float* re, float* im)
    {
        std::fill(fftScratch.begin(), fftScratch.end(), 0.0f);
        std::copy_n(block, N, fftScratch.begin() + N);
        fft->performRealOnlyForwardTransform(fftScratch.data(), true);
        for (int k = 0; k < K; ++k) {
            re[k] = fftScratch[static_cast<size_t>(2 * k)];
            im[k] = fftScratch[static_cast<size_t>(2 * k + 1)];
        }
    }

    void processChannel(Channel& ch, float* y, int P)
    {
        // --- one pass over the partitions: echo estimate Yhat = sum_p X_p W_p,
        // and the filter's own uncertainty sum_p Psi_p |X_p|^2 ---
        float* __restrict Yr = yhr.data();
        float* __restrict Yi = yhi.data();
        float* __restrict U = unc.data();
        std::fill_n(Yr, K, 0.0f);
        std::fill_n(Yi, K, 0.0f);
        std::fill_n(U, K, 0.0f);
        for (int p = 0; p < P; ++p) {
            const int x = lagIndex(p) * K;
            const float* __restrict Xr = xr.data() + x;
            const float* __restrict Xi = xi.data() + x;
            const float* __restrict X2 = x2.data() + x;
            const float* __restrict Wr = ch.wr.data() + p * K;
            const float* __restrict Wi = ch.wi.data() + p * K;
            const float* __restrict Ps = ch.psi.data() + p * K;
            accumulatePartition(Yr, Yi, U, Xr, Xi, X2, Wr, Wi, Ps, K);
        }
        inverse(yhr.data(), yhi.data(), timeScratch.data());
        const float* yhat = timeScratch.data() + N;

        // Spectra of the zero-padded mic, echo estimate and error blocks
        forwardPadded(y, yr.data(), yi.data());
        forwardPadded(yhat, ypr.data(), ypi.data());
        float* __restrict Er = er.data();
        float* __restrict Ei = ei.data();
        float* __restrict D = dScratch.data();
        for (int k = 0; k < K; ++k) {
            Er[k] = yr[static_cast<size_t>(k)] - ypr[static_cast<size_t>(k)];
            Ei[k] = yi[static_cast<size_t>(k)] - ypi[static_cast<size_t>(k)];
            U[k] *= cPad;
            const float e2 = Er[k] * Er[k] + Ei[k] * Ei[k];
            // crowd/noise PSD: error power the filter's uncertainty can't explain
            ch.psiS[static_cast<size_t>(k)] = lam * ch.psiS[static_cast<size_t>(k)]
                                            + (1.0f - lam) * std::max(e2 - U[k], 1e-3f * e2 + 1e-12f);
            D[k] = 1.0f / (U[k] + ch.psiS[static_cast<size_t>(k)]);
        }

        // --- Kalman update, one pass: W += Kg E with Kg = Psi conj(X) / D,
        // then Psi <- A^2 (1 - c Re(Kg X)) Psi + (1 - A^2) |W|^2 ---
        for (int p = 0; p < P; ++p) {
            const int x = lagIndex(p) * K;
            const float* __restrict Xr = xr.data() + x;
            const float* __restrict Xi = xi.data() + x;
            const float* __restrict X2 = x2.data() + x;
            float* __restrict Wr = ch.wr.data() + p * K;
            float* __restrict Wi = ch.wi.data() + p * K;
            float* __restrict Ps = ch.psi.data() + p * K;
            updatePartition(Wr, Wi, Ps, Xr, Xi, X2, Er, Ei, D, K);
        }
        // Gradient constraint (keep each partition causal and N long), on one
        // partition in constraintStride per block in turn: 2 FFTs per
        // partition is most of the cost, and the round-robin measured no
        // loss on the real recordings.
        for (int p = static_cast<int>(blockCount % constraintStride); p < P; p += constraintStride) {
            float* Wr = ch.wr.data() + p * K;
            float* Wi = ch.wi.data() + p * K;
            auto& w = timeScratch2;
            inverse(Wr, Wi, w.data());
            std::fill(w.begin() + N, w.end(), 0.0f);
            forward(w.data(), Wr, Wi);
        }

        // --- divergence guard: never let the subtraction make a bin louder ---
        auto& m = mScratch;
        auto& egr = egScratchR; auto& egi = egScratchI;
        for (int k = 0; k < K; ++k) {
            const size_t kk = static_cast<size_t>(k);
            const float y2 = yr[kk] * yr[kk] + yi[kk] * yi[kk];
            const float e2 = er[kk] * er[kk] + ei[kk] * ei[kk];
            ch.py[kk] = guardSmooth * ch.py[kk] + (1.0f - guardSmooth) * y2;
            ch.pe[kk] = guardSmooth * ch.pe[kk] + (1.0f - guardSmooth) * e2;
            m[kk] = std::min(1.0f, ch.py[kk] / (ch.pe[kk] + 1e-20f));
            egr[kk] = yr[kk] - m[kk] * ypr[kk];
            egi[kk] = yi[kk] - m[kk] * ypi[kk];
        }
        inverse(egr.data(), egi.data(), timeScratch.data()); // yhat is no longer needed
        const float* e = timeScratch.data() + N;

        // --- suppressor: per-bin Wiener gain from the Kalman residual-echo estimate ---
        const float gmin = std::pow(10.0f, supp.floorDb / 20.0f);
        for (int k = 0; k < K; ++k) {
            const size_t kk = static_cast<size_t>(k);
            const float mm = m[kk];
            const float r = unc[kk] * mm * mm + supp.beta * mm * mm * (ypr[kk] * ypr[kk] + ypi[kk] * ypi[kk]);
            ch.pout[kk] = suppSmooth * ch.pout[kk] + (1.0f - suppSmooth) * (egr[kk] * egr[kk] + egi[kk] * egi[kk]);
            const float gn = std::clamp(1.0f - supp.overSub * r / (ch.pout[kk] + 1e-20f), gmin, 1.0f);
            ch.gain[kk] = suppSmooth * ch.gain[kk] + (1.0f - suppSmooth) * gn;
        }
        // K bins -> firLength/2+1 points (linear interpolation, like np.interp)
        const int H = firLength / 2 + 1;
        for (int j = 0; j < H; ++j) {
            const double pos = static_cast<double>(j) * (K - 1) / (H - 1);
            const int k0 = std::min(static_cast<int>(pos), K - 2);
            const float t = static_cast<float>(pos - k0);
            gl[static_cast<size_t>(j)] = (1.0f - t) * ch.gain[static_cast<size_t>(k0)] + t * ch.gain[static_cast<size_t>(k0 + 1)];
        }
        std::swap(ch.h, ch.hPrev);
        for (int n = 0; n < firLength; ++n) {
            const float* row = cosTable.data() + n * H;
            float acc = 0.0f;
            for (int j = 0; j < H; ++j)
                acc += row[j] * gl[static_cast<size_t>(j)];
            ch.h[static_cast<size_t>(n)] = acc;
        }

        // FIR over [last L-1 inputs, this block], crossfading from the previous taps
        std::copy(ch.hist.begin() + N, ch.hist.end(), ch.hist.begin());
        std::copy_n(e, N, ch.hist.end() - N);
        const float* x = ch.hist.data() + (firLength - 1);
        for (int n = 0; n < N; ++n) {
            float a = 0.0f, b = 0.0f;
            for (int t = 0; t < firLength; ++t) {
                a += ch.h[static_cast<size_t>(t)] * x[n - t];
                b += ch.hPrev[static_cast<size_t>(t)] * x[n - t];
            }
            const float fade = ch.hasPrev ? static_cast<float>(n) / static_cast<float>(N - 1) : 1.0f;
            y[n] = fade * a + (1.0f - fade) * b;
        }
        ch.hasPrev = true;
    }

    double fs = 48000.0;
    int N = 128, M = 256, K = 129, fftOrder = 8;
    int maxPartitions = 1, activePartitions = 1;
    int head = 0;
    uint64_t blockCount = 0;
    double accRef = 0.0;
    int accBlocks = 0;
    bool startDone = false;
    std::unique_ptr<juce::dsp::FFT> fft;
    SuppressorSettings supp;
    std::atomic<int> delayEstimateMs{ -1 };

    std::vector<float> xr, xi, x2, xbuf;
    std::vector<Channel> channels;
    std::vector<float> fftScratch, fftScratch2, yhr, yhi, unc, er, ei, yr, yi, ypr, ypi, gl, cosTable;
    std::vector<float> timeScratch, timeScratch2, dScratch, mScratch, egScratchR, egScratchI;
};
