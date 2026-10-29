#pragma once

#include "common.h"
#include "fft.h"
#include "note_set.h"

namespace stasis {

// FOCUS: listens to incoming MIDI and keeps only the frequencies that belong to
// the music. Built as an STFT mask (Hann / Hann, 75 % overlap); each bin gets a
// gain from its distance in semitones to the nearest allowed pitch.
//
//  KEY       allowed = the pitch classes of the held notes, in every octave
//            (or a fixed scale when no notes are held)
//  HARMONIC  allowed = the harmonic series of each held note (n * f0):
//            keeps each note's natural timbre, removes everything in between
//
// Sits on the wet path, after SPACE: a tail left over from the previous chord gets
// faded out as soon as the new chord no longer contains its notes.
//
// Latency = FFT size (8192 → 170 ms, 4096 → 85 ms at 48 kHz). MIX compensates
// internally, so the blend is phase-coherent. ResonantFocus is the zero-latency engine.
class HarmonicFocus {
public:
    enum class Mode { Key, Harmonic };

    struct Params {
        Mode mode = Mode::Key;
        float focus = 0.5f;  // 0 = gentle (±0.6 semitone) .. 1 = laser (±0.1 semitone)
        float depth = 0.85f; // how much out-of-key content is removed
        float mix = 1.0f;
        bool latch = true;   // keep the last chord after the keys are released
    };

    void prepare(float sampleRate, int fftSize = 8192) {
        sampleRate_ = sampleRate;
        size_ = fftSize;
        hop_ = fftSize / 4;
        fft_.prepare(fftSize);
        window_.resize(size_t(size_));
        for (int i = 0; i < size_; ++i) window_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(size_));
        for (int c = 0; c < 2; ++c) {
            input_[c].assign(size_t(size_), 0.0f);
            accum_[c].assign(size_t(size_), 0.0f);
            output_[c].assign(size_t(hop_), 0.0f);
            spectrum_[c].resize(size_t(size_));
            dryDelay_[c].prepare(size_ + 8);
        }
        const size_t bins = size_t(size_ / 2 + 1);
        mask_.assign(bins, 1.0f);
        targetMask_.assign(bins, 1.0f);
        inputFill_ = size_ - hop_;
        outputRead_ = 0;
        maskCoef_ = smoothingCoef(0.15f, sampleRate, hop_);
        dirty_ = true;
    }

    int latencySamples() const { return size_; }

    void setParams(const Params& p) {
        dirty_ = dirty_ || p.mode != params_.mode || p.focus != params_.focus || p.depth != params_.depth ||
                 p.latch != params_.latch;
        params_ = p;
        notes_.setLatch(p.latch);
    }

    // Fallback when no note is held: bit n = pitch class n (0 = C).
    void setScale(uint16_t pitchClassMask) {
        dirty_ = dirty_ || pitchClassMask != scale_;
        scale_ = pitchClassMask;
    }

    void noteOn(int note) {
        notes_.noteOn(note);
        dirty_ = true;
    }
    void noteOff(int note) {
        notes_.noteOff(note);
        dirty_ = true;
    }

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
        if (dirty_) {
            computeMask();
            dirty_ = false;
        }
        const size_t bins = mask_.size();
        for (size_t k = 0; k < bins; ++k) mask_[k] += (targetMask_[k] - mask_[k]) * maskCoef_;

        double energyIn = 0.0, energyOut = 0.0;
        const float norm = 1.0f / (1.5f * float(size_));  // Hann² at 75 % overlap sums to 1.5
        for (int c = 0; c < 2; ++c) {
            auto& spec = spectrum_[c];
            for (int i = 0; i < size_; ++i) spec[size_t(i)] = input_[c][size_t(i)] * window_[size_t(i)];
            fft_.forward(spec);
            for (size_t k = 0; k < bins; ++k) {
                energyIn += std::norm(spec[k]);
                spec[k] *= mask_[k];
                energyOut += std::norm(spec[k]);
                if (k > 0 && k < bins - 1) spec[size_t(size_) - k] = std::conj(spec[k]);
            }
            fft_.inverse(spec);
            auto& acc = accum_[c];
            for (int i = 0; i < size_; ++i) acc[size_t(i)] += spec[size_t(i)].real() * window_[size_t(i)] * norm;

            std::copy(acc.begin(), acc.begin() + hop_, output_[c].begin());
            std::move(acc.begin() + hop_, acc.end(), acc.begin());
            std::fill(acc.end() - hop_, acc.end(), 0.0f);
            std::move(input_[c].begin() + hop_, input_[c].end(), input_[c].begin());
        }
        // Loudness makeup: removing most bins drops the level; give some of it back.
        const float target = float(std::sqrt((energyIn + 1.0e-12) / (energyOut + 1.0e-12)));
        makeup_ += (std::clamp(target, 1.0f, 2.5f) - makeup_) * 0.2f;

        inputFill_ = size_ - hop_;
        outputRead_ = 0;
    }

    void computeMask() {
        uint16_t pitchClasses = notes_.pitchClasses();
        std::vector<float> fundamentals;
        for (int note = 0; note < 128; ++note)
            if (notes_.active().test(size_t(note))) fundamentals.push_back(440.0f * std::pow(2.0f, float(note - 69) / 12.0f));
        if (fundamentals.empty()) pitchClasses = scale_;
        const bool harmonic = params_.mode == Mode::Harmonic && !fundamentals.empty();

        if (!harmonic && pitchClasses == 0) {  // nothing to focus on: pass through
            std::fill(targetMask_.begin(), targetMask_.end(), 1.0f);
            return;
        }
        const float sigmaFocus = 0.6f * std::pow(0.1f / 0.6f, std::clamp(params_.focus, 0.0f, 1.0f));
        const float depth = std::clamp(params_.depth, 0.0f, 1.0f);
        const float binHz = sampleRate_ / float(size_);
        targetMask_[0] = 1.0f - depth;
        for (size_t k = 1; k < targetMask_.size(); ++k) {
            const float f = float(k) * binHz;
            const float midi = 69.0f + 12.0f * std::log2(f / 440.0f);
            // Low bins are wider than a semitone: never make the window narrower than the bin.
            const float semisPerBin = 12.0f * std::log2((f + binHz) / f);
            const float sigma = std::max(sigmaFocus, 0.6f * semisPerBin);

            float dist = 12.0f;
            if (harmonic) {
                for (const float f0 : fundamentals) {
                    const float h = std::max(1.0f, std::round(f / f0));
                    dist = std::min(dist, std::abs(12.0f * std::log2(f / (h * f0))));
                }
            } else {
                for (int pc = 0; pc < 12; ++pc) {
                    if (!(pitchClasses & (1u << pc))) continue;
                    float d = std::fmod(midi - float(pc), 12.0f);
                    if (d < 0.0f) d += 12.0f;
                    dist = std::min(dist, std::min(d, 12.0f - d));
                }
            }
            const float weight = std::exp(-0.5f * (dist / sigma) * (dist / sigma));
            targetMask_[k] = 1.0f - depth * (1.0f - weight);
        }
    }

    float sampleRate_ = 48000.0f;
    int size_ = 8192, hop_ = 2048;
    Params params_;
    Fft fft_;
    std::vector<float> window_, mask_, targetMask_;
    std::vector<float> input_[2], accum_[2], output_[2];
    std::vector<Fft::Complex> spectrum_[2];
    DelayLine dryDelay_[2];
    int inputFill_ = 0, outputRead_ = 0;
    float maskCoef_ = 0.1f, makeup_ = 1.0f;
    NoteSet notes_;
    uint16_t scale_ = 0;
    bool dirty_ = true;
};

}  // namespace stasis
