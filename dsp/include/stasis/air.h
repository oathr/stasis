#pragma once

#include "color.h"
#include "common.h"

namespace stasis {

// AIR: matter and space around the sound.
//  BREATH  0..1  airy noise that follows the signal's envelope (it breathes with the
//                music and stops with it), band-passed 2-9 kHz, decorrelated L/R
//  SMOOTH  seconds  how slowly the breath follows: the envelope is the signal's power
//                through two one-poles in series (no ripple from the waveform itself)
//  WIDTH   0..1  mid/side: 0 mono .. 0.67 unchanged .. 1 = 150 % width
//  LO-FI   0..1  worn medium: the top closes (16 → 3.5 kHz), the resolution drops
//                (16 → 8 bits) and a soft saturation rounds the peaks
//  TREMOLO 0..1  amplitude wobble (sine, per sample, no zipper): at 100 % the dips reach
//                silence; RATE 0.5 .. 16 Hz
class Air {
public:
    struct Params {
        float breath = 0.0f;
        float width = 0.67f;
        float lofi = 0.0f;
        float smoothSeconds = 0.14f;
        float tremolo = 0.0f;
        float tremoloHz = 2.8f;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (int c = 0; c < 2; ++c) {
            noiseHp_[c].set(SvfEq::Type::Highpass, 2000.0f, 0.7f, 0.0f, sampleRate);
            noiseLp_[c].set(SvfEq::Type::Lowpass, 9000.0f, 0.7f, 0.0f, sampleRate);
        }
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        const float lofi = std::clamp(p.lofi, 0.0f, 1.0f);
        for (auto& f : lofiLp_) f.set(SvfEq::Type::Lowpass, 16000.0f * std::pow(3500.0f / 16000.0f, lofi), 0.6f, 0.0f, sampleRate_);
        const float bits = 16.0f - 8.0f * lofi;
        step_ = std::pow(2.0f, 1.0f - bits);
        sideGain_ = std::clamp(p.width, 0.0f, 1.0f) * 1.5f;  // 0.67 → 1.0
        envCoef_ = smoothingCoef(0.5f * std::max(p.smoothSeconds, 0.005f), sampleRate_);  // two stages ≈ the whole time
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float breath = 0.25f * std::clamp(params_.breath, 0.0f, 1.0f);
        const float trem = std::clamp(params_.tremolo, 0.0f, 1.0f), tremInc = params_.tremoloHz / sampleRate_;
        const float lofi = std::clamp(params_.lofi, 0.0f, 1.0f);
        for (int n = 0; n < numSamples; ++n) {
            float x[2] = {inL[n], inR[n]};
            // Width.
            const float mid = 0.5f * (x[0] + x[1]), side = 0.5f * (x[0] - x[1]) * sideGain_;
            x[0] = mid + side;
            x[1] = mid - side;
            // Breath: noise shaped by the envelope.
            power1_ += envCoef_ * (mid * mid - power1_);
            power2_ += envCoef_ * (power1_ - power2_);
            const float env = std::sqrt(std::max(power2_, 0.0f)) * 0.9003f;  // RMS → mean |x| of a sine: the same level as before
            for (int c = 0; c < 2; ++c) {
                float v = x[c];
                if (breath > 0.0f) v += breath * env * noiseLp_[c].process(noiseHp_[c].process(noise_.bipolar()));
                if (lofi > 0.0f) {
                    v = lofiLp_[c].process(v);
                    v = std::round(v / step_) * step_;
                    v += lofi * (std::tanh(v) - v);
                }
                x[c] = v;
            }
            if (trem > 0.0f) {
                tremPhase_ += tremInc;
                if (tremPhase_ >= 1.0f) tremPhase_ -= 1.0f;
                const float g = 1.0f - trem * (0.5f + 0.5f * std::sin(kTwoPi * tremPhase_));
                x[0] *= g;
                x[1] *= g;
            }
            outL[n] = x[0];
            outR[n] = x[1];
        }
    }

private:
    float sampleRate_ = 48000.0f;
    Params params_;
    SvfEq noiseHp_[2], noiseLp_[2], lofiLp_[2];
    Rng noise_{0xA1B2};
    float power1_ = 0.0f, power2_ = 0.0f, envCoef_ = 0.001f, step_ = 0.0f, sideGain_ = 1.0f, tremPhase_ = 0.0f;
};

}  // namespace stasis
