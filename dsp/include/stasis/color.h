#pragma once

#include <array>

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

    // Same filter on another channel: the coefficients only, each keeps its own state.
    void copyCoefficients(const SvfEq& o) {
        a1_ = o.a1_, a2_ = o.a2_, a3_ = o.a3_, m0_ = o.m0_, m1_ = o.m1_, m2_ = o.m2_;
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

// COLOR: five words instead of an EQ, low to high, two filters and a tremolo. Every
// word is bipolar: centre = flat, up = boost, down = cut.
//  BASS   -1..1  low shelf Â±24 dB at 110 Hz; below 0 a high-pass also rises (20 â†’ 120 Hz)
//  BODY   -1..1  bell Â±18 dB at 220 Hz: the warmth of a pad
//  MUD    -1..1  bell Â±18 dB at 450 Hz: the boxy build-up of long tails (cut it to clear)
//  HARSH  -1..1  bell Â±18 dB at 3.5 kHz: the glassy edge of shimmers and bright pads
//  AIR    -1..1  high shelf Â±24 dB at 7 kHz
//  HPF / LPF     12 dB/oct, 20 Hz .. 1 kHz and 1 kHz .. 20 kHz (at the ends: off)
//  TREMOLO 0..1  amplitude wobble (sine, per sample): at 100 % the dips reach silence
// Moves glide: gains and cutoffs follow their targets over ~20 ms, and the moving bands'
// coefficients are recomputed every sample (every 4 or 16 still left a grit at -50 /
// -58 dB), so turning a knob (or an LFO) never crackles. HARD mode
// skips the glide on purpose: each change lands at once, and the filters' jumps click.
class Color {
public:
    static constexpr float kShelfDb = 24.0f, kBellDb = 18.0f;

    struct Params {
        float bass = 0.0f;
        float body = 0.0f;
        float mud = 0.0f;
        float harsh = 0.0f;
        float air = 0.0f;
        float highpassHz = 0.0f;  // <= 20: off
        float lowpassHz = 0.0f;   // 0 or >= 20000: off
        float tremolo = 0.0f;
        float tremoloHz = 2.8f;
        bool hard = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        glide_ = smoothingCoef(0.02f, sampleRate);
        setParams(params_);
        current_ = target_;
        updateFilters(~0u);
    }

    void setParams(const Params& p) {
        params_ = p;
        const auto clamp = [](float x) { return std::clamp(x, -1.0f, 1.0f); };
        const float bass = clamp(p.bass);
        const float hpHz = std::max(bass < 0.0f ? 20.0f + 100.0f * bass * bass : 10.0f, p.highpassHz > 20.0f ? p.highpassHz : 10.0f);
        const bool lowpassOn = p.lowpassHz > 0.0f && p.lowpassHz < 20000.0f;
        // Cutoffs glide in octaves, gains in dB.
        target_ = {std::log2(hpHz), std::log2(lowpassOn ? p.lowpassHz : 22000.0f), kShelfDb * bass, kBellDb * clamp(p.body),
                   kBellDb * clamp(p.mud), kBellDb * clamp(p.harsh), kShelfDb * clamp(p.air)};
        if (p.hard) {  // at once, as the old zipper did
            current_ = target_;
            updateFilters(~0u);
        }
    }

    // Tempo sync: where the tremolo's cycle stands (0..1) at the start of the next block.
    void setTremoloPhase(float phase) { tremPhase_ = phase - std::floor(phase); }

    // In-place safe.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float trem = std::clamp(params_.tremolo, 0.0f, 1.0f), tremInc = params_.tremoloHz / sampleRate_;
        for (int n = 0; n < numSamples; ++n) {
            if (current_ != target_) {
                unsigned moving = 0;
                for (size_t k = 0; k < current_.size(); ++k) {
                    const float d = target_[k] - current_[k];
                    if (d == 0.0f) continue;
                    current_[k] = std::abs(d) < 1.0e-3f ? target_[k] : current_[k] + d * glide_;
                    moving |= 1u << k;
                }
                updateFilters(moving);
            }
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
            if (trem > 0.0f) {
                tremPhase_ += tremInc;
                if (tremPhase_ >= 1.0f) tremPhase_ -= 1.0f;
                const float g = 1.0f - trem * (0.5f + 0.5f * std::sin(kTwoPi * tremPhase_));
                out[0] *= g;
                out[1] *= g;
            }
            outL[n] = out[0];
            outR[n] = out[1];
        }
    }

private:
    enum { kHp, kLp, kBass, kBody, kMud, kHarsh, kAir, kCount };

    // Only the bands in mask (bit = band); computed once, copied to the right channel.
    void updateFilters(unsigned mask) {
        const auto band = [&](int k, SvfEq* f, SvfEq::Type type, float hz, float q, float db) {
            if (!(mask & (1u << k))) return;
            f[0].set(type, hz, q, db, sampleRate_);
            f[1].copyCoefficients(f[0]);
        };
        const float lp = std::exp2(current_[kLp]);
        lowpassOn_ = lp < 20000.0f;
        band(kHp, highpass_, SvfEq::Type::Highpass, std::exp2(current_[kHp]), 0.707f, 0.0f);
        band(kLp, lowpass_, SvfEq::Type::Lowpass, std::min(lp, 20000.0f), 0.707f, 0.0f);
        band(kBass, lowShelf_, SvfEq::Type::LowShelf, 110.0f, 0.707f, current_[kBass]);
        band(kBody, bodyBell_, SvfEq::Type::Bell, 220.0f, 0.9f, current_[kBody]);
        band(kMud, mudBell_, SvfEq::Type::Bell, 450.0f, 1.0f, current_[kMud]);
        band(kHarsh, harshBell_, SvfEq::Type::Bell, 3500.0f, 0.8f, current_[kHarsh]);
        band(kAir, highShelf_, SvfEq::Type::HighShelf, 7000.0f, 0.707f, current_[kAir]);
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    std::array<float, kCount> target_{}, current_{};
    SvfEq highpass_[2], lowpass_[2], lowShelf_[2], bodyBell_[2], mudBell_[2], harshBell_[2], highShelf_[2];
    bool lowpassOn_ = false;
    float glide_ = 0.1f, tremPhase_ = 0.0f;
};

}  // namespace stasis
