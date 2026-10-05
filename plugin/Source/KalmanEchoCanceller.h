#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// Full-band partitioned-block frequency-domain Kalman filter (PB-FDKF) echo
// canceller with a low-latency Wiener suppressor -- Less PA's canceller
// since 1.1.0, replacing the WebRTC AEC3 engine of 1.0.x.
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
        float beta = 0.48f;        // residual-echo share of the echo estimate (model mismatch, PA distortion)
        float overSub = 2.4f;      // over-subtraction of the residual-echo estimate; 0 = filter only
        float floorDb = -12.0f;    // deepest per-bin cut
        float responseMs = 30.0f;  // time constant of the suppressor's power and gain smoothing
    };

    // The panel's Amount (0..1) as suppressor settings: 0 is the bare
    // filter, 1 the strongest setting tried on the recordings.
    static SuppressorSettings settingsForAmount(float amount, float maxReductionDb, float responseMs)
    {
        const float a = std::clamp(amount, 0.0f, 1.0f);
        return { 0.6f * a, 3.0f * a, maxReductionDb, responseMs };
    }

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
        probeR.assign(static_cast<size_t>(K), 0.0f); probeI.assign(static_cast<size_t>(K), 0.0f);
        irPeak.assign(static_cast<size_t>(maxPartitions), 0.0f);
        irPeakIdx.assign(static_cast<size_t>(maxPartitions), 0);
        const double binHz = fs / M;
        probeLoBin = std::clamp(static_cast<int>(std::ceil(probeLoHz / binHz)), 1, K - 1);
        probeHiBin = std::clamp(static_cast<int>(std::floor(probeHiHz / binHz)), probeLoBin, K - 1);

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
        setSuppressor(supp); // the smoothing depends on the block size and rate
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
        std::fill(irPeak.begin(), irPeak.end(), 0.0f);
        std::fill(irPeakIdx.begin(), irPeakIdx.end(), 0);
        probeIndex = 0;
        recentDelays.fill(-1);
        recentCount = 0;
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

    // Live and allocation-free: takes effect from the next block.
    void setSuppressor(const SuppressorSettings& s)
    {
        supp = s;
        const double tau = std::max(1.0, static_cast<double>(s.responseMs)) * 1e-3;
        suppSmooth = static_cast<float>(std::exp(-static_cast<double>(N) / (tau * fs)));
        floorGain = std::pow(10.0f, std::min(0.0f, s.floorDb) / 20.0f);
    }

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

        probeImpulseResponse(P);

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
    // 56-66 min: ~2 min behind the old AEC3 engine), too large and it can lose its lock
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
    static constexpr int constraintStride = 64; // gradient constraint on 1 partition in 64 per block
    static constexpr double startSeconds = 3.0;    // PA needed before the level-relative start
    static constexpr double startRefPower = 1e-5;  // a "PA-active" ref block: mean square above -50 dBFS
    static constexpr double startScale = 1.0;
    static constexpr double probeLoHz = 400.0;     // delay readout band
    static constexpr double probeHiHz = 6000.0;
    static constexpr double peakDominance = 4.0;   // peak^2 vs mean partition peak^2 to count as found
    static constexpr float firstArrivalRatio = 0.5f;

    int partitionsFor(double seconds) const
    {
        return std::max(1, static_cast<int>(std::ceil(seconds * fs / N)));
    }

    // PA delay readout. The filter needs no delay estimate itself (it covers
    // the whole tail), so this is display only. Each block one partition of
    // the (first) channel's filter is turned back into its N impulse-response
    // taps, band-limited to probeLoHz-probeHiHz so the readout follows the
    // direct sound rather than low-frequency room build-up, and its peak is
    // kept. The readout is the overall peak's lag, at sample resolution,
    // shown only when it clearly stands out, and as the median of the last
    // few estimates so a single odd one never reaches the display.
    void probeImpulseResponse(int P)
    {
        const int q = static_cast<int>(probeIndex++ % static_cast<uint64_t>(P));
        const auto& ch = channels.front();
        std::fill(probeR.begin(), probeR.end(), 0.0f);
        std::fill(probeI.begin(), probeI.end(), 0.0f);
        for (int k = probeLoBin; k <= probeHiBin; ++k) {
            probeR[static_cast<size_t>(k)] = ch.wr[static_cast<size_t>(q * K + k)];
            probeI[static_cast<size_t>(k)] = ch.wi[static_cast<size_t>(q * K + k)];
        }
        inverse(probeR.data(), probeI.data(), timeScratch2.data());
        float peak = 0.0f;
        int at = 0;
        for (int n = 0; n < N; ++n) {
            const float a = std::abs(timeScratch2[static_cast<size_t>(n)]);
            if (a > peak) { peak = a; at = n; }
        }
        irPeak[static_cast<size_t>(q)] = peak;
        irPeakIdx[static_cast<size_t>(q)] = at;
    }

    void updateDelayEstimate(int P)
    {
        int best = -1;
        float bestPeak = 0.0f;
        double meanSq = 0.0;
        for (int q = 0; q < P; ++q) {
            const float v = irPeak[static_cast<size_t>(q)];
            meanSq += static_cast<double>(v) * v;
            if (v > bestPeak) { bestPeak = v; best = q; }
        }
        meanSq /= P;
        // "Found": the strongest tap clearly above the typical partition peak
        const bool found = best >= 0 && bestPeak > 0.0f
                        && static_cast<double>(bestPeak) * bestPeak > peakDominance * meanSq;
        // Report the first arrival: the earliest peak within firstArrivalRatio
        // of the strongest. Music that repeats on the beat lets the filter
        // build echo-like taps a beat or two later (232-256 ms on LS26), and
        // those must not win over the direct sound.
        for (int q = 0; found && q < best; ++q)
            if (irPeak[static_cast<size_t>(q)] >= firstArrivalRatio * bestPeak) {
                best = q;
                break;
            }
        const int estimate = found ? static_cast<int>(std::lround(
                                         1000.0 * (best * N + irPeakIdx[static_cast<size_t>(best)]) / fs))
                                   : -1;
        recentDelays[static_cast<size_t>(recentCount++ % recentLen)] = estimate;

        std::array<int, recentLen> sorted{};
        int n = 0;
        for (int d : recentDelays)
            if (d >= 0)
                sorted[static_cast<size_t>(n++)] = d;
        int shown = -1;
        if (n > recentLen / 2) { // a majority of recent estimates found one
            std::sort(sorted.begin(), sorted.begin() + n);
            shown = sorted[static_cast<size_t>(n / 2)];
        }
        delayEstimateMs.store(shown, std::memory_order_relaxed);
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
        const float gmin = floorGain;
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
    uint64_t probeIndex = 0;
    int probeLoBin = 1, probeHiBin = 1;
    std::vector<float> irPeak, probeR, probeI;
    std::vector<int> irPeakIdx;
    static constexpr int recentLen = 9; // ~2.25 s of readouts
    std::array<int, recentLen> recentDelays{};
    uint32_t recentCount = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    SuppressorSettings supp;
    float suppSmooth = 0.875f; // exp(-N / (responseMs * fs)), set by setSuppressor
    float floorGain = 0.25f;
    std::atomic<int> delayEstimateMs{ -1 };

    std::vector<float> xr, xi, x2, xbuf;
    std::vector<Channel> channels;
    std::vector<float> fftScratch, fftScratch2, yhr, yhi, unc, er, ei, yr, yi, ypr, ypi, gl, cosTable;
    std::vector<float> timeScratch, timeScratch2, dScratch, mScratch, egScratchR, egScratchI;
};
