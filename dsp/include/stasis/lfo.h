#pragma once

#include "common.h"

namespace stasis {

// Order of the SHAPE knob. A fractional shape morphs between neighbours.
enum class LfoShape : int {
    Sine,
    Triangle,
    SawUp,
    SawDown,
    Square,
    RandomStep,    // sample & hold
    RandomSlew,    // sample & hold, smoothed
    RandomSmooth,  // cosine-interpolated random ("random sine")
    Drunk,         // reflected brownian walk
    Perlin,        // 1D gradient noise
    Chaos,         // Lorenz attractor
    Breath,        // asymmetric inhale / exhale
    Count
};

// Bipolar LFO in [-1, 1]. Meant to run at control rate: call advance(blockSize).
// Every random state keeps running whatever the shape, so morphing never jumps.
class Lfo {
public:
    void prepare(float sampleRate, uint32_t seed = 1) {
        sampleRate_ = sampleRate;
        rng_ = Rng(seed);
        seed_ = seed * 0x9E3779B9u;
        reset();
    }
    void reset() {
        phase_ = 0.0f;
        prev_ = rng_.bipolar();
        target_ = rng_.bipolar();
        slew_ = prev_;
        drunk_ = 0.0f;
        noisePos_ = rng_.uniform() * 100.0;
        lx_ = 0.1;
        ly_ = 0.0;
        lz_ = 20.0;
        value_ = 0.0f;
    }

    void setRate(float hz) { rate_ = std::max(hz, 0.0f); }
    void setShape(float shape) { shape_ = std::clamp(shape, 0.0f, float(int(LfoShape::Count) - 1)); }
    void setShape(LfoShape shape) { setShape(float(int(shape))); }
    void setPhase(float phase) { phase_ = phase - std::floor(phase); }

    float value() const { return value_; }

    float advance(int samples) { return advanceBy(rate_ * float(samples) / sampleRate_); }
    // Tempo sync: move to `phase` (0..1), always forward, so the random shapes keep
    // stepping once per cycle.
    float advanceTo(float phase) {
        float inc = phase - std::floor(phase) - phase_;
        if (inc < 0.0f) inc += 1.0f;
        return advanceBy(inc);
    }

    float advanceBy(float inc) {
        phase_ += inc;
        while (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            prev_ = target_;
            target_ = rng_.bipolar();
        }

        slew_ += (target_ - slew_) * (1.0f - std::exp(-inc * 8.0f));

        drunk_ += rng_.bipolar() * std::sqrt(inc) * 1.2f;
        if (drunk_ > 1.0f) drunk_ = 2.0f - drunk_;
        if (drunk_ < -1.0f) drunk_ = -2.0f - drunk_;

        noisePos_ += double(inc) * 2.0;
        stepLorenz(double(inc) * 0.8);

        const int a = int(shape_);
        const float t = shape_ - float(a);
        float v = evaluate(a);
        if (t > 0.0f && a + 1 < int(LfoShape::Count)) v += t * (evaluate(a + 1) - v);
        value_ = v;
        return v;
    }

private:
    float evaluate(int shape) const {
        const float p = phase_;
        switch (LfoShape(shape)) {
            case LfoShape::Sine: return std::sin(kTwoPi * p);
            case LfoShape::Triangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
            case LfoShape::SawUp: return 2.0f * p - 1.0f;
            case LfoShape::SawDown: return 1.0f - 2.0f * p;
            case LfoShape::Square: return p < 0.5f ? 1.0f : -1.0f;
            case LfoShape::RandomStep: return prev_;
            case LfoShape::RandomSlew: return slew_;
            case LfoShape::RandomSmooth: return prev_ + (target_ - prev_) * (0.5f - 0.5f * std::cos(kPi * p));
            case LfoShape::Drunk: return drunk_;
            case LfoShape::Perlin: return perlin();
            case LfoShape::Chaos: return std::clamp(float(lx_ / 20.0), -1.0f, 1.0f);
            case LfoShape::Breath:
                return p < 0.4f ? -std::cos(kPi * p / 0.4f) : std::cos(kPi * (p - 0.4f) / 0.6f);
            default: return 0.0f;
        }
    }

    float perlin() const {
        const double fl = std::floor(noisePos_);
        const int32_t i = int32_t(fl);
        const float f = float(noisePos_ - fl);
        const float g0 = gradient(i), g1 = gradient(i + 1);
        const float u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
        const float n = (1.0f - u) * g0 * f + u * g1 * (f - 1.0f);
        return std::clamp(n * 2.0f, -1.0f, 1.0f);
    }
    float gradient(int32_t i) const {
        uint32_t h = uint32_t(i) * 0x27D4EB2Du ^ seed_;
        h ^= h >> 15;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        return float(h & 0xFFFFu) / 32767.5f - 1.0f;
    }

    void stepLorenz(double time) {
        const int steps = std::clamp(int(std::ceil(time / 0.005)), 1, 64);
        const double dt = time / steps;
        for (int s = 0; s < steps; ++s) {
            const double dx = 10.0 * (ly_ - lx_);
            const double dy = lx_ * (28.0 - lz_) - ly_;
            const double dz = lx_ * ly_ - (8.0 / 3.0) * lz_;
            lx_ += dx * dt;
            ly_ += dy * dt;
            lz_ += dz * dt;
        }
    }

    float sampleRate_ = 48000.0f;
    float rate_ = 0.1f;
    float shape_ = 0.0f;
    float phase_ = 0.0f;
    float prev_ = 0.0f, target_ = 0.0f, slew_ = 0.0f, drunk_ = 0.0f;
    double noisePos_ = 0.0;
    double lx_ = 0.1, ly_ = 0.0, lz_ = 20.0;
    float value_ = 0.0f;
    uint32_t seed_ = 1;
    Rng rng_;
};

}  // namespace stasis
