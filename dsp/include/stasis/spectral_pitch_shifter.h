#pragma once

#include "common.h"
#include "fft.h"

namespace stasis {

// Phase-vocoder pitch shifter (bin shifting with true-frequency estimation).
// Every partial is multiplied by exactly `ratio`: no grain crossfades, hence none of
// the sidebands that make a granular shifter sound slightly out of tune.
//
// Built for SHIMMER: the input is a dense, noise-like reverb tail, where a phase
// vocoder shines and its latency (fftSize samples, 43 ms at 2048) just becomes part
// of the feedback loop's length. Gain <= 1, so it is safe inside the loop.
class SpectralPitchShifter {
public:
    void prepare(float sampleRate, int fftSize = 2048, int overlap = 4) {
        (void)sampleRate;
        size_ = fftSize;
        hop_ = fftSize / overlap;
        fft_.prepare(fftSize);
        window_.resize(size_t(size_));
        for (int i = 0; i < size_; ++i) window_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(size_));
        // Hann analysis * Hann synthesis summed over `overlap` frames = 0.375 * overlap.
        norm_ = 1.0f / (0.375f * float(overlap) * float(size_));
        const size_t bins = size_t(size_ / 2 + 1);
        input_.assign(size_t(size_), 0.0f);
        accum_.assign(size_t(size_), 0.0f);
        output_.assign(size_t(hop_), 0.0f);
        spectrum_.resize(size_t(size_));
        lastPhase_.assign(bins, 0.0f);
        sumPhase_.assign(bins, 0.0f);
        synthMag_.assign(bins, 0.0f);
        synthFreq_.assign(bins, 0.0f);
        fill_ = size_ - hop_;
        read_ = 0;
    }

    int latencySamples() const { return size_; }
    void setSemitones(float semitones) { ratio_ = semitonesToRatio(semitones); }

    float process(float x) {
        input_[size_t(fill_)] = x;
        const float y = output_[size_t(read_++)];
        if (++fill_ == size_) processFrame();
        return y;
    }

private:
    void processFrame() {
        const size_t bins = lastPhase_.size();
        for (int i = 0; i < size_; ++i) spectrum_[size_t(i)] = input_[size_t(i)] * window_[size_t(i)];
        fft_.forward(spectrum_);

        std::fill(synthMag_.begin(), synthMag_.end(), 0.0f);
        std::fill(synthFreq_.begin(), synthFreq_.end(), 0.0f);
        const float expected = kTwoPi * float(hop_) / float(size_);
        // Down-shifts fold several bins onto one: scale down so the loop gain stays <= 1.
        const float gain = std::min(1.0f, ratio_);
        for (size_t k = 0; k < bins; ++k) {
            const float mag = std::abs(spectrum_[k]);
            const float phase = std::arg(spectrum_[k]);
            float delta = phase - lastPhase_[k] - float(k) * expected;
            lastPhase_[k] = phase;
            delta -= kTwoPi * std::round(delta / kTwoPi);
            const float trueBin = float(k) + delta / expected;  // instantaneous frequency, in bins

            const long j = std::lround(float(k) * ratio_);
            if (j <= 0 || j >= long(bins)) continue;
            if (mag * gain > synthMag_[size_t(j)]) synthFreq_[size_t(j)] = trueBin * ratio_;
            synthMag_[size_t(j)] += mag * gain;
        }
        for (size_t j = 0; j < bins; ++j) {
            sumPhase_[j] += synthFreq_[j] * expected;
            sumPhase_[j] -= kTwoPi * std::floor(sumPhase_[j] / kTwoPi);
            spectrum_[j] = std::polar(synthMag_[j], sumPhase_[j]);
            if (j > 0 && j < bins - 1) spectrum_[size_t(size_) - j] = std::conj(spectrum_[j]);
        }
        spectrum_[0] = 0.0f;
        fft_.inverse(spectrum_);
        for (int i = 0; i < size_; ++i) accum_[size_t(i)] += spectrum_[size_t(i)].real() * window_[size_t(i)] * norm_;

        std::copy(accum_.begin(), accum_.begin() + hop_, output_.begin());
        std::move(accum_.begin() + hop_, accum_.end(), accum_.begin());
        std::fill(accum_.end() - hop_, accum_.end(), 0.0f);
        std::move(input_.begin() + hop_, input_.end(), input_.begin());
        fill_ = size_ - hop_;
        read_ = 0;
    }

    int size_ = 2048, hop_ = 512, fill_ = 0, read_ = 0;
    float ratio_ = 2.0f, norm_ = 1.0f;
    Fft fft_;
    std::vector<float> window_, input_, accum_, output_, lastPhase_, sumPhase_, synthMag_, synthFreq_;
    std::vector<Fft::Complex> spectrum_;
};

}  // namespace stasis
