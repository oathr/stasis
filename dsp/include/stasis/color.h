#pragma once

#include "common.h"

namespace stasis {

// TPT SVF in its EQ forms (Simper, "Linear trapezoidal integrated SVF"):
// out = m0*v0 + m1*band + m2*low. Zipper-free when its gain is modulated.
class SvfEq {
public:
    enum class Type { Bell, LowShelf, HighShelf, Highpass, Lowpass };

    void set(Type type, float hz, float q, float gainDb, float sampleRate) {
        const float a = std::pow(10.0f, gainDb / 40.0f);
        float g = std::tan(kPi * std::clamp(hz, 1.0f, 0.49f * sampleRate) / sampleRate);
        float k = 1.0f / std::max(q, 0.05f);
        switch (type) {
            case Type::Bell:
                k = 1.0f / (std::max(q, 0.05f) * a);
                m0_ = 1.0f, m1_ = k * (a * a - 1.0f), m2_ = 0.0f;
                break;
            case Type::LowShelf:
                g /= std::sqrt(a);
                m0_ = 1.0f, m1_ = k * (a - 1.0f), m2_ = a * a - 1.0f;
                break;
            case Type::HighShelf:
                g *= std::sqrt(a);
                m0_ = a * a, m1_ = k * (1.0f - a) * a, m2_ = 1.0f - a * a;
                break;
            case Type::Highpass:
                m0_ = 1.0f, m1_ = -k, m2_ = -1.0f;
                break;
            case Type::Lowpass:
                m0_ = 0.0f, m1_ = 0.0f, m2_ = 1.0f;
                break;
        }
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    float process(float v0) {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return m0_ * v0 + m1_ * v1 + m2_ * v2;
    }

private:
    float a1_ = 1.0f, a2_ = 0.0f, a3_ = 0.0f;
    float m0_ = 1.0f, m1_ = 0.0f, m2_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

// COLOR: five words instead of an EQ, low to high, plus two filters.
//  BASS   -1..1  low shelf ±9 dB at 120 Hz; below 0 a high-pass also rises (20 → 120 Hz)
//  BODY   -1..1  bell ±6 dB at 180 Hz: the warmth of a pad, below the mud
//  MUD     0..1  cuts the 200-600 Hz build-up. Part static, part dynamic: the cut
//                deepens only when that band dominates the spectrum, so a clean
//                signal stays full and a muddy tail gets cleared.
//  HARSH   0..1  the same idea at 2-5 kHz: tames the glassy edge of shimmers and
//                bright pads, and leaves a soft signal alone.
//  AIR    -1..1  high shelf ±8 dB at 6 kHz
//  HPF / LPF     12 dB/oct, 20 Hz .. 1 kHz and 1 kHz .. 20 kHz (at the ends: off)
class Color {
public:
    struct Params {
        float bass = 0.0f;
        float body = 0.0f;
        float mud = 0.0f;
        float harsh = 0.0f;
        float air = 0.0f;
        float highpassHz = 0.0f;  // <= 20: off
        float lowpassHz = 0.0f;   // 0 or >= 20000: off
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        detector_.set(kMudHz, 0.7f, sampleRate);
        harshDetector_.set(kHarshHz, 0.7f, sampleRate);
        envCoef_ = smoothingCoef(0.12f, sampleRate);
        setParams(params_);
        updateDynamics(0.0f, 0.0f);
    }

    void setParams(const Params& p) {
        params_ = p;
        const float bass = std::clamp(p.bass, -1.0f, 1.0f), air = std::clamp(p.air, -1.0f, 1.0f);
        const float hpHz = std::max(bass < 0.0f ? 20.0f + 100.0f * bass * bass : 10.0f, p.highpassHz > 20.0f ? p.highpassHz : 10.0f);
        lowpassOn_ = p.lowpassHz > 0.0f && p.lowpassHz < 20000.0f;
        for (int c = 0; c < 2; ++c) {
            highpass_[c].set(SvfEq::Type::Highpass, hpHz, 0.707f, 0.0f, sampleRate_);
            lowpass_[c].set(SvfEq::Type::Lowpass, lowpassOn_ ? p.lowpassHz : 20000.0f, 0.707f, 0.0f, sampleRate_);
            lowShelf_[c].set(SvfEq::Type::LowShelf, 120.0f, 0.707f, 9.0f * bass, sampleRate_);
            bodyBell_[c].set(SvfEq::Type::Bell, 180.0f, 0.9f, 6.0f * std::clamp(p.body, -1.0f, 1.0f), sampleRate_);
            highShelf_[c].set(SvfEq::Type::HighShelf, 6000.0f, 0.707f, 8.0f * air, sampleRate_);
        }
    }

    // In-place safe.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        for (int n = 0; n < numSamples; ++n) {
            const float mid = 0.5f * (inL[n] + inR[n]);
            const float band = detector_.process(mid).band, harsh = harshDetector_.process(mid).band;
            bandEnv_ += envCoef_ * (band * band - bandEnv_);
            harshEnv_ += envCoef_ * (harsh * harsh - harshEnv_);
            fullEnv_ += envCoef_ * (mid * mid - fullEnv_);
            if ((counter_++ & 31) == 0)
                updateDynamics(std::sqrt(bandEnv_ / (fullEnv_ + 1.0e-12f)), std::sqrt(harshEnv_ / (fullEnv_ + 1.0e-12f)));

            const float in[2] = {inL[n], inR[n]};
            float out[2];
            for (int c = 0; c < 2; ++c) {
                float v = highpass_[c].process(in[c]);
                v = lowShelf_[c].process(v);
                v = bodyBell_[c].process(v);
                v = mudBell_[c].process(v);
                v = harshBell_[c].process(v);
                v = highShelf_[c].process(v);
                out[c] = lowpassOn_ ? lowpass_[c].process(v) : v;
            }
            outL[n] = out[0];
            outR[n] = out[1];
        }
    }

private:
    static constexpr float kMudHz = 320.0f, kHarshHz = 3500.0f;

    void updateDynamics(float mudRatio, float harshRatio) {
        // Ratios ≈ share of each band in the signal (0..1). High bands carry less energy
        // in a natural spectrum, so HARSH reacts from a lower share.
        const float mudExcess = std::clamp((mudRatio - 0.3f) / 0.4f, 0.0f, 1.0f);
        const float harshExcess = std::clamp((harshRatio - 0.12f) / 0.25f, 0.0f, 1.0f);
        const float mudDb = -std::clamp(params_.mud, 0.0f, 1.0f) * (3.0f + 9.0f * mudExcess);
        const float harshDb = -std::clamp(params_.harsh, 0.0f, 1.0f) * (2.0f + 8.0f * harshExcess);
        for (auto& bell : mudBell_) bell.set(SvfEq::Type::Bell, kMudHz, 0.8f, mudDb, sampleRate_);
        for (auto& bell : harshBell_) bell.set(SvfEq::Type::Bell, kHarshHz, 0.7f, harshDb, sampleRate_);
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    SvfEq highpass_[2], lowpass_[2], lowShelf_[2], bodyBell_[2], mudBell_[2], harshBell_[2], highShelf_[2];
    Svf detector_, harshDetector_;
    float bandEnv_ = 0.0f, harshEnv_ = 0.0f, fullEnv_ = 0.0f, envCoef_ = 0.001f;
    bool lowpassOn_ = false;
    uint32_t counter_ = 0;
};

}  // namespace stasis
