#pragma once

#include <array>

#include "common.h"

namespace stasis {

// TPT SVF in its EQ forms (Simper, "Linear trapezoidal integrated SVF"):
// out = m0*v0 + m1*band + m2*low. Zipper-free when its gain is modulated.
class SvfEq {
public:
    enum class Type { Bell, LowShelf, HighShelf, Highpass, Lowpass };
    struct Coefs {
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;
        bool operator==(const Coefs&) const = default;
    };

    static Coefs compute(Type type, float hz, float q, float gainDb, float sampleRate) {
        Coefs c;
        const float a = std::pow(10.0f, gainDb / 40.0f);
        float g = std::tan(kPi * std::clamp(hz, 1.0f, 0.49f * sampleRate) / sampleRate);
        float k = 1.0f / std::max(q, 0.05f);
        switch (type) {
            case Type::Bell:
                k = 1.0f / (std::max(q, 0.05f) * a);
                c.m0 = 1.0f, c.m1 = k * (a * a - 1.0f), c.m2 = 0.0f;
                break;
            case Type::LowShelf:
                g /= std::sqrt(a);
                c.m0 = 1.0f, c.m1 = k * (a - 1.0f), c.m2 = a * a - 1.0f;
                break;
            case Type::HighShelf:
                g *= std::sqrt(a);
                c.m0 = a * a, c.m1 = k * (1.0f - a) * a, c.m2 = 1.0f - a * a;
                break;
            case Type::Highpass:
                c.m0 = 1.0f, c.m1 = -k, c.m2 = -1.0f;
                break;
            case Type::Lowpass:
                c.m0 = 0.0f, c.m1 = 0.0f, c.m2 = 1.0f;
                break;
        }
        c.a1 = 1.0f / (1.0f + g * (g + k));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
        return c;
    }

    void set(Type type, float hz, float q, float gainDb, float sampleRate) { c_ = compute(type, hz, q, gainDb, sampleRate); }
    void setCoefs(const Coefs& c) { c_ = c; }

    // HARD: scale the integrators' memory (1 = untouched, 0 = dropped).
    void scaleState(float keep) { ic1_ *= keep, ic2_ *= keep; }

    float process(float v0) {
        const float v3 = v0 - ic2_;
        const float v1 = c_.a1 * ic1_ + c_.a2 * v3;
        const float v2 = ic2_ + c_.a2 * ic1_ + c_.a3 * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return c_.m0 * v0 + c_.m1 * v1 + c_.m2 * v2;
    }

private:
    Coefs c_;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

// COLOR: five words instead of an EQ, low to high, two filters and a tremolo. Every
// word is bipolar: centre = flat, up = boost, down = cut.
//  BASS   -1..1  low shelf ±24 dB at 110 Hz; below 0 a high-pass also rises (20 → 120 Hz)
//  BODY   -1..1  bell ±18 dB at 220 Hz: the warmth of a pad
//  MUD    -1..1  bell ±18 dB at 450 Hz: the boxy build-up of long tails (cut it to clear)
//  HARSH  -1..1  bell ±18 dB at 3.5 kHz: the glassy edge of shimmers and bright pads
//  AIR    -1..1  high shelf ±24 dB at 7 kHz
//  HPF / LPF     12 dB/oct, 20 Hz .. 1 kHz and 1 kHz .. 20 kHz (at the ends: off)
//  TREMOLO 0..1  amplitude wobble (sine, per sample): at 100 % the dips reach silence
// Moves glide: each change computes its target coefficients once, then the coefficients
// themselves glide every sample over ~20 ms (stepping them every 4 or 16 samples left a grit
// at -58 / -50 dB), so turning a knob (or an LFO) never crackles. HARD mode
// skips the glide on purpose: each change lands at once and the filters are kicked (part
// of their state dropped, all of it at 100 %) and each jump fires a short click sized by the
// jump and by the sound's level: the crack.
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
        float hard = 0.0f;  // 0 = glide; above, jumps land at once and kick the filters
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        glide_ = smoothingCoef(0.02f, sampleRate);
        computed_.fill(NAN);
        setParams(params_);
        cur_ = tgt_;  // no glide from nothing
        for (int k = 0; k < kCount; ++k) apply(k);
        moving_ = 0;
    }

    void setParams(const Params& p) {
        params_ = p;
        const auto clamp = [](float x) { return std::clamp(x, -1.0f, 1.0f); };
        const float bass = clamp(p.bass);
        const float hpHz = std::max(bass < 0.0f ? 20.0f + 100.0f * bass * bass : 10.0f, p.highpassHz > 20.0f ? p.highpassHz : 10.0f);
        const bool lowpassOn = p.lowpassHz > 0.0f && p.lowpassHz < 20000.0f;
        // Cutoffs in octaves, gains in dB.
        target_ = {std::log2(hpHz), std::log2(lowpassOn ? p.lowpassHz : 22000.0f), kShelfDb * bass, kBellDb * clamp(p.body),
                   kBellDb * clamp(p.mud), kBellDb * clamp(p.harsh), kShelfDb * clamp(p.air)};
        // The changed bands get new target coefficients (the costly part, once per change).
        unsigned changed = 0;
        for (int k = 0; k < kCount; ++k) {
            if (target_[size_t(k)] == computed_[size_t(k)]) continue;
            computed_[size_t(k)] = target_[size_t(k)];
            tgt_[size_t(k)] = coefsFor(k, target_[size_t(k)]);
            changed |= 1u << k;
        }
        if (p.hard > 0.0f && changed) {
            // At once, as the old zipper did; the moved filters lose part of their state and
            // a click sized by the jump (and the sound's level) marks it: the crack.
            float jump = 0.0f;
            for (int k = 0; k < kCount; ++k) {
                const float d = target_[size_t(k)] - current_[size_t(k)];
                jump += k <= kLp ? std::abs(d) * 0.5f : std::abs(d) / kBellDb;  // octaves for the cuts, dB for the rest
                if (d != 0.0f) clickSign_ = d > 0.0f ? 1.0f : -1.0f;
            }
            clickPending_ += std::clamp(p.hard, 0.0f, 1.0f) * std::min(1.0f, jump) * clickSign_;
            for (int k = 0; k < kCount; ++k)
                if (changed & (1u << k)) cur_[size_t(k)] = tgt_[size_t(k)], apply(k);
            kick(changed, 1.0f - std::clamp(p.hard, 0.0f, 1.0f));
            moving_ &= ~changed;
        } else {
            moving_ |= changed;
        }
        current_ = target_;
        lowpassOn_ = lowpassOn || (moving_ & (1u << kLp));
    }
    // Tempo sync: where the tremolo's cycle stands (0..1) at the start of the next block.
    void setTremoloPhase(float phase) { tremPhase_ = phase - std::floor(phase); }

    // In-place safe.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float trem = std::clamp(params_.tremolo, 0.0f, 1.0f), tremInc = params_.tremoloHz / sampleRate_;
        for (int n = 0; n < numSamples; ++n) {
            if (moving_) glide();
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
            // HARD's click: up to 4x the sound's level, gone in ~16 samples.
            level_ += 0.002f * (std::abs(out[0]) + std::abs(out[1]) - level_);
            if (clickPending_ != 0.0f) {
                const float c = 4.0f * level_ * clickPending_;
                out[0] += c;
                out[1] -= c * 0.7f;  // a little apart: the crack has a place in the stereo
                clickPending_ *= -0.62f;
                if (std::abs(clickPending_) < 1.0e-3f) clickPending_ = 0.0f;
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

    // A band's coefficients for its value (octaves for the cuts, dB for the rest).
    SvfEq::Coefs coefsFor(int k, float value) const {
        switch (k) {
            case kHp: return SvfEq::compute(SvfEq::Type::Highpass, std::exp2(value), 0.707f, 0.0f, sampleRate_);
            case kLp: return SvfEq::compute(SvfEq::Type::Lowpass, std::min(std::exp2(value), 20000.0f), 0.707f, 0.0f, sampleRate_);
            case kBass: return SvfEq::compute(SvfEq::Type::LowShelf, 110.0f, 0.707f, value, sampleRate_);
            case kBody: return SvfEq::compute(SvfEq::Type::Bell, 220.0f, 0.9f, value, sampleRate_);
            case kMud: return SvfEq::compute(SvfEq::Type::Bell, 450.0f, 1.0f, value, sampleRate_);
            case kHarsh: return SvfEq::compute(SvfEq::Type::Bell, 3500.0f, 0.8f, value, sampleRate_);
            default: return SvfEq::compute(SvfEq::Type::HighShelf, 7000.0f, 0.707f, value, sampleRate_);
        }
    }
    SvfEq* bands(int k) {
        SvfEq* all[kCount] = {highpass_, lowpass_, lowShelf_, bodyBell_, mudBell_, harshBell_, highShelf_};
        return all[k];
    }
    void apply(int k) {
        bands(k)[0].setCoefs(cur_[size_t(k)]);
        bands(k)[1].setCoefs(cur_[size_t(k)]);
    }
    void kick(unsigned mask, float keep) {
        for (int k = 0; k < kCount; ++k)
            if (mask & (1u << k)) bands(k)[0].scaleState(keep), bands(k)[1].scaleState(keep);
    }
    // One sample of glide: the moving bands' coefficients move ~20 ms towards their targets
    // (multiply-adds only: recomputing tan / pow every sample cost 2x the CPU at 96 kHz).
    void glide() {
        for (int k = 0; k < kCount; ++k) {
            if (!(moving_ & (1u << k))) continue;
            auto& c = cur_[size_t(k)];
            const auto& t = tgt_[size_t(k)];
            float* cv[6] = {&c.a1, &c.a2, &c.a3, &c.m0, &c.m1, &c.m2};
            const float tv[6] = {t.a1, t.a2, t.a3, t.m0, t.m1, t.m2};
            float far = 0.0f;
            for (int j = 0; j < 6; ++j) {
                *cv[j] += (tv[j] - *cv[j]) * glide_;
                far = std::max(far, std::abs(tv[j] - *cv[j]));
            }
            if (far < 1.0e-6f) {
                c = t;
                moving_ &= ~(1u << k);
                if (k == kLp) lowpassOn_ = std::exp2(target_[kLp]) < 20000.0f;
            }
            apply(k);
        }
    }
    float sampleRate_ = 48000.0f;
    Params params_;
    std::array<float, kCount> target_{}, current_{}, computed_{};
    std::array<SvfEq::Coefs, kCount> cur_{}, tgt_{};
    unsigned moving_ = 0;
    SvfEq highpass_[2], lowpass_[2], lowShelf_[2], bodyBell_[2], mudBell_[2], harshBell_[2], highShelf_[2];
    bool lowpassOn_ = false;
    float glide_ = 0.1f, tremPhase_ = 0.0f, clickPending_ = 0.0f, clickSign_ = 1.0f, level_ = 0.0f;
};

}  // namespace stasis
