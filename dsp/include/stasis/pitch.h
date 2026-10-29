#pragma once

#include "common.h"
#include "spectral_pitch_shifter.h"

namespace stasis {

// Frequency shifter: every partial moves by the same number of Hz (not a ratio), so
// harmonics stop being harmonic: from a slow beating shimmer (±1 Hz) to metallic.
// Analytic signal from two allpass chains 90° apart (Niemitalo's coefficients, flat
// within 0.7° from 20 Hz to 20 kHz at 44.1 kHz), multiplied by a complex oscillator.
class FrequencyShifter {
public:
    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& s : states_) s = {};
        phase_ = 0.0;
    }
    void setShiftHz(float hz) { shiftHz_ = hz; }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const double inc = double(shiftHz_) / sampleRate_;
        for (int n = 0; n < numSamples; ++n) {
            const double c = std::cos(kTwoPi * phase_), s = std::sin(kTwoPi * phase_);
            phase_ += inc;
            phase_ -= std::floor(phase_);
            const float x[2] = {inL[n], inR[n]};
            float y[2];
            for (int ch = 0; ch < 2; ++ch) {
                auto& st = states_[size_t(ch)];
                const float re = st.delayed;  // the A chain's output, one sample late (Niemitalo)
                st.delayed = chain(st.a, kA, x[ch]);
                const float im = chain(st.b, kB, x[ch]);
                y[ch] = float(re * c + im * s);  // positive shift = up
            }
            outL[n] = y[0];
            outR[n] = y[1];
        }
    }

private:
    struct AllpassState {
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };
    struct Channel {
        std::array<AllpassState, 4> a{}, b{};
        float delayed = 0.0f;
    };
    static constexpr std::array<float, 4> kA = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
    static constexpr std::array<float, 4> kB = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};

    // Second-order allpass sections in z^-2: y = c²(x + y[n-2]) - x[n-2].
    static float chain(std::array<AllpassState, 4>& st, const std::array<float, 4>& k, float x) {
        for (size_t i = 0; i < 4; ++i) {
            auto& s = st[i];
            const float c2 = k[i] * k[i];
            const float y = c2 * (x + s.y2) - s.x2;
            s.x2 = s.x1;
            s.x1 = x;
            s.y2 = s.y1;
            s.y1 = y;
            x = y;
        }
        return x;
    }

    float sampleRate_ = 48000.0f, shiftHz_ = 0.0f;
    double phase_ = 0.0;
    std::array<Channel, 2> states_{};
};

// PITCH: transposition (phase vocoder, -24..+24 semitones) then frequency shift
// (-50..+50 Hz), blended with what comes in by MIX.
//  - The transposed voice is brought back to the input's level (the vocoder loses
//    ~4 dB going up and halves its gain going down, a safety it needs inside SHIMMER).
//  - While transposing, the direct part is delayed by the vocoder's latency, so MIX
//    blends two aligned voices: at 50 % a clean interval, not a note and its echo.
// Latency: one FFT window (2048) while transposing, on the wet path only.
class Pitch {
public:
    struct Params {
        float semitones = 0.0f;
        float shiftHz = 0.0f;
        float mix = 1.0f;
    };

    void prepare(float sampleRate) {
        for (auto& s : shifters_) s.prepare(sampleRate, 2048, 4);
        frequency_.prepare(sampleRate);
        levelCoef_ = smoothingCoef(0.3f, sampleRate);
        for (auto& d : dry_) d.assign(size_t(shifters_[0].latencySamples()), 0.0f);
        dryPos_ = 0;
        inPower_ = outPower_ = 0.0f;
        gain_ = 1.0f;
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        for (auto& s : shifters_) s.setSemitones(p.semitones);
        frequency_.setShiftHz(p.shiftHz);
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const bool transpose = std::abs(params_.semitones) > 1.0e-3f;
        const float mix = std::clamp(params_.mix, 0.0f, 1.0f);
        for (int done = 0; done < numSamples; done += kChunk) {
            const int n = std::min(kChunk, numSamples - done);
            const float* in[2] = {inL + done, inR + done};
            float* out[2] = {outL + done, outR + done};
            for (int i = 0; i < n; ++i) {
                // The shifters always run, so their latency never changes under the user.
                const float l = shifters_[0].process(in[0][i]), r = shifters_[1].process(in[1][i]);
                const float delayedL = dry_[0][size_t(dryPos_)], delayedR = dry_[1][size_t(dryPos_)];
                dry_[0][size_t(dryPos_)] = in[0][i], dry_[1][size_t(dryPos_)] = in[1][i];
                dryPos_ = (dryPos_ + 1) % int(dry_[0].size());
                if (transpose) {
                    // Level match: what came in (aligned) against what the vocoder gives.
                    inPower_ += levelCoef_ * (0.5f * (delayedL * delayedL + delayedR * delayedR) - inPower_);
                    outPower_ += levelCoef_ * (0.5f * (l * l + r * r) - outPower_);
                    const float target = outPower_ > 1.0e-10f ? std::clamp(std::sqrt(inPower_ / outPower_), 1.0f, 4.0f) : gain_;
                    gain_ += levelCoef_ * (target - gain_);
                    direct_[0][size_t(i)] = delayedL, direct_[1][size_t(i)] = delayedR;
                    out[0][i] = l * gain_, out[1][i] = r * gain_;
                } else {
                    direct_[0][size_t(i)] = in[0][i], direct_[1][size_t(i)] = in[1][i];
                    out[0][i] = in[0][i], out[1][i] = in[1][i];
                }
            }
            if (std::abs(params_.shiftHz) > 1.0e-3f) frequency_.process(out[0], out[1], out[0], out[1], n);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) out[c][i] = direct_[c][size_t(i)] + mix * (out[c][i] - direct_[c][size_t(i)]);
        }
    }

private:
    static constexpr int kChunk = 256;
    Params params_;
    SpectralPitchShifter shifters_[2];
    FrequencyShifter frequency_;
    std::vector<float> dry_[2];
    std::array<std::array<float, kChunk>, 2> direct_{};
    int dryPos_ = 0;
    float levelCoef_ = 0.001f, inPower_ = 0.0f, outPower_ = 0.0f, gain_ = 1.0f;
};
}  // namespace stasis
