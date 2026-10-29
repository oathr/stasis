#pragma once

#include "common.h"
#include "fft.h"

namespace stasis {

// Extreme time stretch after Paul Nasca's PaulStretch: very long FFT frames whose
// phases are replaced by random ones, overlap-added at 50 %. Analysis window is
// Hann; synthesis window is a sine window, which keeps constant *power* when the
// overlapping frames are uncorrelated (they are, phases are random).
//
// Stereo: one random phase per bin for both channels, plus the original L/R phase
// difference, so the stereo image survives.
//
// Phase 0 version: renders from a captured buffer. The plugin wraps the same core
// around a circular capture buffer (CAPTURE / SMEAR modes, see PLAN.md).
class PaulStretch {
public:
    void prepare(int windowSize, uint32_t seed = 1) {
        size_ = windowSize;
        fft_.prepare(windowSize);
        rng_ = Rng(seed);
        analysis_.resize(size_t(windowSize));
        synthesis_.resize(size_t(windowSize));
        double sumSq = 0.0;
        for (int i = 0; i < windowSize; ++i) {
            const float x = float(i) / float(windowSize);
            analysis_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * x);
            synthesis_[size_t(i)] = std::sin(kPi * x);
            sumSq += double(analysis_[size_t(i)]) * analysis_[size_t(i)];
        }
        gain_ = float(1.0 / std::sqrt(sumSq / windowSize));
        specL_.resize(size_t(windowSize));
        specR_.resize(size_t(windowSize));
    }

    // stretch <= 0 freezes at startSample (spectral freeze).
    void render(const float* srcL, const float* srcR, size_t srcLength, double startSample, double stretch,
                float* outL, float* outR, size_t outLength) {
        std::fill(outL, outL + outLength, 0.0f);
        std::fill(outR, outR + outLength, 0.0f);
        const int hop = size_ / 2;
        const double inHop = stretch > 0.0 ? hop / stretch : 0.0;
        double inPos = startSample;

        for (size_t outPos = 0; outPos < outLength; outPos += size_t(hop), inPos += inHop) {
            const long begin = long(inPos) - size_ / 2;
            for (int i = 0; i < size_; ++i) {
                const long s = begin + i;
                const bool inside = s >= 0 && size_t(s) < srcLength;
                specL_[size_t(i)] = inside ? srcL[s] * analysis_[size_t(i)] : 0.0f;
                specR_[size_t(i)] = inside ? srcR[s] * analysis_[size_t(i)] : 0.0f;
            }
            fft_.forward(specL_);
            fft_.forward(specR_);

            specL_[0] = specR_[0] = 0.0f;  // drop DC
            specL_[size_t(size_ / 2)] = specR_[size_t(size_ / 2)] = 0.0f;
            for (int k = 1; k < size_ / 2; ++k) {
                const size_t ks = size_t(k);
                const float phase = kTwoPi * rng_.uniform();
                const float stereoPhase = std::arg(specR_[ks]) - std::arg(specL_[ks]);
                specL_[ks] = std::polar(std::abs(specL_[ks]), phase);
                specR_[ks] = std::polar(std::abs(specR_[ks]), phase + stereoPhase);
                specL_[size_t(size_ - k)] = std::conj(specL_[ks]);
                specR_[size_t(size_ - k)] = std::conj(specR_[ks]);
            }
            fft_.inverse(specL_);
            fft_.inverse(specR_);

            const float norm = gain_ / float(size_);
            for (int i = 0; i < size_ && outPos + size_t(i) < outLength; ++i) {
                const float w = synthesis_[size_t(i)] * norm;
                outL[outPos + size_t(i)] += specL_[size_t(i)].real() * w;
                outR[outPos + size_t(i)] += specR_[size_t(i)].real() * w;
            }
        }
    }

private:
    int size_ = 0;
    Fft fft_;
    Rng rng_;
    float gain_ = 1.0f;
    std::vector<float> analysis_, synthesis_;
    std::vector<Fft::Complex> specL_, specR_;
};

}  // namespace stasis
