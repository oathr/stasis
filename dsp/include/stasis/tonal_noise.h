#pragma once

#include "common.h"
#include "fft.h"

namespace stasis {

// TONAL ◂▸ NOISE (after PaulXStretch's "tonal vs noise"): splits the spectrum into
// stable peaks (notes, partials) and everything in between (breath, hiss, diffuse
// texture), and keeps more of one or the other.
//
// Per STFT bin: ratio = magnitude / local mean over ±bandwidth semitones (prefix
// sums, so any width costs O(1)). Peaks stand well above their neighbourhood, noise
// hovers around 1. Two detections, because the two directions need opposite things:
//  - keeping partials: strict (the reference includes the peak itself), mask ±1 bin,
//    so only the core of each partial passes and little noise rides along;
//  - removing partials: wide (the reference leaves the main lobe out), mask ±2 bins,
//    so nothing of a partial is left, even in the bass.
// Masks are smoothed over time so they do not flicker ("musical noise"). Phases are
// untouched: this is filtering, not resynthesis.
//
// Measured on a chord of sines in noise (tonal-to-noise ratio +7.6 dB at the input):
// balance +1 → +17.1 dB, balance -1 → -1.8 dB.
//
//  balance  -1 = noise only .. 0 = unchanged .. +1 = tonal only
//
// Latency = FFT size (4096 → 85 ms at 48 kHz), wet path only; MIX is compensated.
class TonalNoise {
public:
    struct Params {
        float balance = 0.0f;
        float bandwidth = 0.5f;  // 0..1 → neighbourhood of ±0.5 .. ±4 semitones
        float mix = 1.0f;
    };

    void prepare(float sampleRate, int fftSize = 4096) {
        sampleRate_ = sampleRate;
        size_ = fftSize;
        hop_ = fftSize / 4;
        fft_.prepare(fftSize);
        window_.resize(size_t(size_));
        for (int i = 0; i < size_; ++i) window_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(size_));
        const size_t bins = size_t(size_ / 2 + 1);
        for (int c = 0; c < 2; ++c) {
            input_[c].assign(size_t(size_), 0.0f);
            accum_[c].assign(size_t(size_), 0.0f);
            output_[c].assign(size_t(hop_), 0.0f);
            dryDelay_[c].prepare(size_ + 8);
        }
        spec_.resize(size_t(size_));
        mag_.assign(bins, 0.0f);
        prefix_.assign(bins + 1, 0.0);
        tonal_.assign(bins, 0.0f);
        tonalWide_.assign(bins, 0.0f);
        weight_.assign(bins, 0.0f);
        weightWide_.assign(bins, 0.0f);
        inputFill_ = size_ - hop_;
        outputRead_ = 0;
        maskCoef_ = smoothingCoef(0.04f, sampleRate, hop_);
        makeup_ = 1.0f;
    }

    int latencySamples() const { return size_; }
    void setParams(const Params& p) { params_ = p; }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float* in[2] = {inL, inR};
        float* out[2] = {outL, outR};
        const float mix = std::clamp(params_.mix, 0.0f, 1.0f);
        for (int n = 0; n < numSamples; ++n) {
            for (int c = 0; c < 2; ++c) {
                const float x = in[c][n];
                input_[c][size_t(inputFill_)] = x;
                const float dry = dryDelay_[c].readInt(size_);
                dryDelay_[c].write(x);
                out[c][n] = dry + mix * (output_[c][size_t(outputRead_)] * makeup_ - dry);
            }
            ++outputRead_;
            if (++inputFill_ == size_) processFrame();
        }
    }

private:
    void processFrame() {
        const size_t bins = mag_.size();
        // Both channels in one complex FFT: z = left + i·right.
        for (int i = 0; i < size_; ++i)
            spec_[size_t(i)] = Fft::Complex(input_[0][size_t(i)], input_[1][size_t(i)]) * window_[size_t(i)];
        fft_.forward(spec_);
        // Mask from the mid (L+R) magnitude: both channels share it, the image stays put.
        for (size_t k = 0; k < bins; ++k) {
            const Fft::Complex a = spec_[k], b = std::conj(spec_[(size_t(size_) - k) % size_t(size_)]);
            const Fft::Complex left = 0.5f * (a + b), right = Fft::Complex(0.0f, -0.5f) * (a - b);
            mag_[k] = std::abs(left + right);
        }
        for (size_t k = 0; k < bins; ++k) prefix_[k + 1] = prefix_[k] + mag_[k];

        const float semis = 0.5f + 3.5f * std::clamp(params_.bandwidth, 0.0f, 1.0f);
        const float up = std::pow(2.0f, semis / 12.0f);
        auto smooth = [](float ratio) {
            const float t = std::clamp((ratio - 1.6f) / (3.4f - 1.6f), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const auto mean = [&](long lo, long hi) { return (prefix_[size_t(hi + 1)] - prefix_[size_t(lo)]) / double(hi - lo + 1); };
        for (size_t k = 1; k < bins; ++k) {
            const long kk = long(k), last = long(bins) - 1;
            // Strict detection, used to KEEP partials: the reference includes the bin's own
            // main lobe, so only the core of a clear peak passes and little noise rides along.
            const long lo = std::max(1L, std::min(long(float(k) / up), kk - 3));
            const long hi = std::min(last, std::max(long(float(k) * up), kk + 3));
            tonal_[k] = smooth(float(mag_[k] / (mean(lo, hi) + 1e-12)));
            // Wide detection, used to REMOVE partials: the reference (at least ±8 bins) leaves
            // out the main lobe (±2 bins), so even a low partial cannot hide behind itself.
            const long wlo = std::max(1L, std::min(long(float(k) / up), kk - 8));
            const long whi = std::min(last, std::max(long(float(k) * up), kk + 8));
            const long cLo = std::max(wlo, kk - 2), cHi = std::min(whi, kk + 2);
            const double outside = (prefix_[size_t(whi + 1)] - prefix_[size_t(wlo)]) - (prefix_[size_t(cHi + 1)] - prefix_[size_t(cLo)]);
            const long count = std::max(1L, (whi - wlo + 1) - (cHi - cLo + 1));
            tonalWide_[k] = smooth(float(mag_[k] / (outside / double(count) + 1e-12)));
        }
        tonal_[0] = tonalWide_[0] = 0.0f;
        for (size_t k = 0; k < bins; ++k) {
            // Keeping partials uses a narrow mask (±1 bin) so little noise rides along;
            // removing them uses the full main lobe (±2 bins) so nothing of them is left.
            const float n1 = std::max({tonal_[k], k >= 1 ? tonal_[k - 1] : 0.0f, k + 1 < bins ? tonal_[k + 1] : 0.0f});
            float n2 = tonalWide_[k];
            for (size_t d = 1; d <= 2; ++d) {
                if (k >= d) n2 = std::max(n2, tonalWide_[k - d]);
                if (k + d < bins) n2 = std::max(n2, tonalWide_[k + d]);
            }
            weight_[k] += (n1 - weight_[k]) * maskCoef_;
            weightWide_[k] += (n2 - weightWide_[k]) * maskCoef_;
        }

        const float b = std::clamp(params_.balance, -1.0f, 1.0f);
        const float norm = 1.0f / (1.5f * float(size_));  // Hann² at 75 % overlap sums to 1.5
        // The gain is real and the same for both channels and for bins k and N-k, so it
        // applies to z directly.
        double energyIn = 0.0, energyOut = 0.0;
        for (size_t k = 0; k < bins; ++k) {
            const float gain = b >= 0.0f ? (1.0f - b) + b * weight_[k] : (1.0f + b) + (-b) * (1.0f - weightWide_[k]);
            const size_t mirror = (size_t(size_) - k) % size_t(size_);
            const double e = std::norm(spec_[k]) + (mirror != k ? std::norm(spec_[mirror]) : 0.0);
            energyIn += e;
            energyOut += e * gain * gain;
            spec_[k] *= gain;
            if (mirror != k) spec_[mirror] *= gain;
        }
        fft_.inverse(spec_);
        for (int c = 0; c < 2; ++c) {
            auto& acc = accum_[c];
            for (int i = 0; i < size_; ++i) acc[size_t(i)] += (c == 0 ? spec_[size_t(i)].real() : spec_[size_t(i)].imag()) * window_[size_t(i)] * norm;
            std::copy(acc.begin(), acc.begin() + hop_, output_[c].begin());
            std::move(acc.begin() + hop_, acc.end(), acc.begin());
            std::fill(acc.end() - hop_, acc.end(), 0.0f);
            std::move(input_[c].begin() + hop_, input_[c].end(), input_[c].begin());
        }
        // Keeping only one side drops the level: give part of it back (max +8 dB).
        const float target = float(std::sqrt((energyIn + 1e-12) / (energyOut + 1e-12)));
        makeup_ += (std::clamp(target, 1.0f, 2.5f) - makeup_) * 0.15f;
        inputFill_ = size_ - hop_;
        outputRead_ = 0;
    }

    float sampleRate_ = 48000.0f;
    int size_ = 4096, hop_ = 1024, inputFill_ = 0, outputRead_ = 0;
    Params params_;
    Fft fft_;
    std::vector<float> window_, mag_, tonal_, tonalWide_, weight_, weightWide_;
    std::vector<double> prefix_;
    std::vector<float> input_[2], accum_[2], output_[2];
    std::vector<Fft::Complex> spec_;
    DelayLine dryDelay_[2];
    float maskCoef_ = 0.5f, makeup_ = 1.0f;
};

}  // namespace stasis
