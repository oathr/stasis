#pragma once

#include <array>

#include "common.h"
#include "fft.h"

namespace stasis {

// AUTO mode for FOCUS: guesses which pitch classes are playing from the audio
// itself, no MIDI needed. Runs on the dry input, on the side: it never delays the
// audio, it only decides which notes FOCUS lets through (the decision lags by
// ~0.2-1 s, which suits slow ambient material).
//
// Per frame: spectral peaks between 60 Hz and 1.5 kHz, refined by parabolic
// interpolation, folded into a 12-bin chroma (weighted by how close each peak is
// to an equal-tempered pitch, A = 440 Hz). The chroma is averaged over MEMORY
// seconds; in silence it holds, so reverb tails keep the last key.
class KeyDetector {
public:
    struct Params {
        float memorySeconds = 1.5f;
        float threshold = 0.45f;  // a class is "in" above this share of the strongest one
        int maxClasses = 7;
    };

    void prepare(float sampleRate, int fftSize = 8192) {
        sampleRate_ = sampleRate;
        size_ = fftSize;
        hop_ = fftSize / 4;
        fft_.prepare(fftSize);
        window_.resize(size_t(size_));
        for (int i = 0; i < size_; ++i) window_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(size_));
        buffer_.assign(size_t(size_), 0.0f);
        spectrum_.resize(size_t(size_));
        magnitude_.resize(size_t(size_ / 2 + 1));
        chroma_.fill(0.0f);
        fill_ = 0;
        pitchClasses_ = 0;
    }

    void setParams(const Params& p) { params_ = p; }

    // Bit n = pitch class n (0 = C). 0 until something has been heard.
    uint16_t pitchClasses() const { return pitchClasses_; }

    void analyse(const float* inL, const float* inR, int numSamples) {
        for (int n = 0; n < numSamples; ++n) {
            buffer_[size_t(size_ - hop_ + fill_)] = 0.5f * (inL[n] + inR[n]);
            if (++fill_ == hop_) {
                analyseFrame();
                std::move(buffer_.begin() + hop_, buffer_.end(), buffer_.begin());
                fill_ = 0;
            }
        }
    }

private:
    void analyseFrame() {
        double energy = 0.0;
        for (int i = 0; i < size_; ++i) {
            const float v = buffer_[size_t(i)];
            energy += double(v) * v;
            spectrum_[size_t(i)] = v * window_[size_t(i)];
        }
        if (energy / size_ < 1.0e-8) return;  // silence: keep the last key

        fft_.forward(spectrum_);
        for (size_t k = 0; k < magnitude_.size(); ++k) magnitude_[k] = std::abs(spectrum_[k]);

        const float binHz = sampleRate_ / float(size_);
        const int lo = std::max(2, int(60.0f / binHz)), hi = std::min(int(magnitude_.size()) - 2, int(1500.0f / binHz));
        float peak = 0.0f;
        for (int k = lo; k <= hi; ++k) peak = std::max(peak, magnitude_[size_t(k)]);
        if (peak <= 0.0f) return;

        std::array<float, 12> frame{};
        for (int k = lo; k <= hi; ++k) {
            const float m = magnitude_[size_t(k)];
            if (m < 0.02f * peak || m <= magnitude_[size_t(k - 1)] || m < magnitude_[size_t(k + 1)]) continue;
            // Parabolic interpolation on log magnitude for the true peak frequency.
            const float a = std::log(magnitude_[size_t(k - 1)] + 1e-12f), b = std::log(m), c = std::log(magnitude_[size_t(k + 1)] + 1e-12f);
            const float denom = a - 2.0f * b + c;
            const float offset = denom != 0.0f ? 0.5f * (a - c) / denom : 0.0f;
            const float hz = (float(k) + offset) * binHz;
            const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
            const float nearest = std::round(midi);
            const float deviation = midi - nearest;
            const int pc = ((int(nearest) % 12) + 12) % 12;
            // Lower peaks weigh more: they are more likely fundamentals than upper harmonics.
            frame[size_t(pc)] += std::sqrt(m / peak) * std::sqrt(200.0f / std::max(hz, 200.0f)) * std::exp(-0.5f * (deviation / 0.25f) * (deviation / 0.25f));
        }
        float sum = 0.0f;
        for (const float v : frame) sum += v;
        if (sum <= 0.0f) return;

        const float keep = std::exp(-float(hop_) / (params_.memorySeconds * sampleRate_));
        for (size_t i = 0; i < 12; ++i) chroma_[i] = keep * chroma_[i] + (1.0f - keep) * frame[i] / sum;
        updateClasses();
    }

    void updateClasses() {
        std::array<int, 12> order{};
        for (int i = 0; i < 12; ++i) order[size_t(i)] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return chroma_[size_t(a)] > chroma_[size_t(b)]; });
        const float strongest = chroma_[size_t(order[0])];
        uint16_t mask = 0;
        for (int rank = 0; rank < params_.maxClasses; ++rank) {
            const int pc = order[size_t(rank)];
            if (chroma_[size_t(pc)] >= params_.threshold * strongest) mask |= uint16_t(1u << pc);
        }
        pitchClasses_ = mask;
    }

    float sampleRate_ = 48000.0f;
    int size_ = 8192, hop_ = 2048, fill_ = 0;
    Params params_;
    Fft fft_;
    std::vector<float> window_, buffer_, magnitude_;
    std::vector<Fft::Complex> spectrum_;
    std::array<float, 12> chroma_{};
    uint16_t pitchClasses_ = 0;
};

}  // namespace stasis
