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
//
// Identity phase locking (Laroche & Dolson, 1999): each spectral peak runs its own phase
// at its true frequency, and the bins around it (its region, up to half-way to the next
// peak) keep their original phase offset from it. Without it, the 4 bins of a partial
// drift apart and the shifted sound turns watery / phasey, more so pass after pass.
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
        best_.assign(bins, 0.0f);
        mag_.assign(bins, 0.0f);
        phase_.assign(bins, 0.0f);
        trueBin_.assign(bins, 0.0f);
        peakPhase_.assign(bins, 0.0f);
        synthPhase_.assign(bins, 0.0f);
        owner_.assign(bins, 0);
        peaks_.clear();
        peaks_.reserve(bins);
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
        std::fill(best_.begin(), best_.end(), 0.0f);
        const float expected = kTwoPi * float(hop_) / float(size_);
        // Down-shifts fold several bins onto one: scale down so the loop gain stays <= 1.
        const float gain = std::min(1.0f, ratio_);
        const auto target = [&](size_t k) { return std::lround(float(k) * ratio_); };
        // Analysis: magnitude, phase and instantaneous frequency (in bins) of every bin.
        for (size_t k = 0; k < bins; ++k) {
            mag_[k] = std::abs(spectrum_[k]);
            phase_[k] = std::arg(spectrum_[k]);
            float delta = phase_[k] - lastPhase_[k] - float(k) * expected;
            lastPhase_[k] = phase_[k];
            delta -= kTwoPi * std::round(delta / kTwoPi);
            trueBin_[k] = float(k) + delta / expected;
            // Where this bin's partial would be now if it led its region: the running phase
            // of its target bin, advanced at its own shifted frequency.
            const long j = target(k);
            peakPhase_[k] = j > 0 && j < long(bins) ? sumPhase_[size_t(j)] + trueBin_[k] * ratio_ * expected : 0.0f;
        }
        // Peaks: local maxima over Â±2 bins. Every bin belongs to the nearest one.
        peaks_.clear();
        for (size_t k = 2; k + 2 < bins; ++k)
            if (mag_[k] > 1.0e-9f && mag_[k] >= mag_[k - 1] && mag_[k] > mag_[k + 1] && mag_[k] >= mag_[k - 2] && mag_[k] > mag_[k + 2])
                peaks_.push_back(int(k));
        for (size_t k = 0, p = 0; k < bins; ++k) {
            if (peaks_.empty()) {
                owner_[k] = int(k);
                continue;
            }
            while (p + 1 < peaks_.size() && 2 * int(k) > peaks_[p] + peaks_[p + 1]) ++p;
            owner_[k] = peaks_[p];
        }
        // Synthesis: the peak's phase plus the bin's original offset from its peak. Where
        // several bins land on one (down-shifts), the loudest sets the phase.
        for (size_t k = 0; k < bins; ++k) {
            const long j = target(k);
            if (j <= 0 || j >= long(bins)) continue;
            const float m = mag_[k] * gain;
            synthMag_[size_t(j)] += m;
            if (m > best_[size_t(j)]) {
                best_[size_t(j)] = m;
                const size_t p = size_t(owner_[k]);
                synthPhase_[size_t(j)] = peakPhase_[p] + (phase_[k] - phase_[p]);
            }
        }
        for (size_t j = 0; j < bins; ++j) {
            if (best_[j] > 0.0f) sumPhase_[j] = synthPhase_[j] - kTwoPi * std::floor(synthPhase_[j] / kTwoPi);
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
    std::vector<float> window_, input_, accum_, output_, lastPhase_, sumPhase_, synthMag_, best_;
    std::vector<float> mag_, phase_, trueBin_, peakPhase_, synthPhase_;
    std::vector<int> owner_, peaks_;
    std::vector<Fft::Complex> spectrum_;
};

}  // namespace stasis
