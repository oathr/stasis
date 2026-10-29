#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace stasis {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float semitonesToRatio(float st) { return std::pow(2.0f, st / 12.0f); }

// Per-pass gain of a loop of `delaySamples` so the level drops 60 dB in `rt60` seconds.
inline float rt60Gain(float delaySamples, float rt60, float sampleRate) {
    if (!std::isfinite(rt60)) return 1.0f;
    return std::pow(10.0f, -3.0f * delaySamples / (std::max(rt60, 1.0e-3f) * sampleRate));
}

// One-pole smoothing coefficient for a time constant, applied every `samples` samples.
inline float smoothingCoef(float timeSeconds, float sampleRate, int samples = 1) {
    return 1.0f - std::exp(-float(samples) / (std::max(timeSeconds, 1.0e-4f) * sampleRate));
}

// Flush denormals to zero for the current thread. Long reverb tails decay towards
// denormal range, which is extremely slow on most CPUs.
inline void enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)));
#elif defined(__SSE__)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

// xorshift32: cheap, deterministic, plenty for modulation and noise.
struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : state(seed ? seed : 1u) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
    float bipolar() { return uniform() * 2.0f - 1.0f; }                     // [-1, 1)
};

// Power-of-two circular delay line. Read before write: delay 1 is the last written sample.
class DelayLine {
public:
    void prepare(int maxDelaySamples) {
        int size = 1;
        while (size < maxDelaySamples + 4) size <<= 1;
        buffer_.assign(size_t(size), 0.0f);
        mask_ = size - 1;
        writeIndex_ = 0;
    }
    void clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }
    int maxDelay() const { return mask_ - 3; }

    void write(float x) {
        buffer_[size_t(writeIndex_)] = x;
        writeIndex_ = (writeIndex_ + 1) & mask_;
    }
    float readInt(int delay) const { return buffer_[size_t((writeIndex_ - delay) & mask_)]; }

    // Fractional read, cubic Hermite (Catmull-Rom): smooth under modulation.
    float read(float delay) const {
        delay = std::clamp(delay, 2.0f, float(maxDelay()));
        const int d = int(delay);
        const float t = delay - float(d);
        const float ym1 = readInt(d - 1), y0 = readInt(d), y1 = readInt(d + 1), y2 = readInt(d + 2);
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

private:
    std::vector<float> buffer_;
    int mask_ = 0;
    int writeIndex_ = 0;
};

// Schroeder allpass built on DelayLine; negative gain flips the sign convention.
struct Allpass {
    DelayLine line;
    float delay = 100.0f;
    float gain = 0.5f;

    void prepare(int maxDelaySamples) { line.prepare(maxDelaySamples); }
    float process(float x) { return processAt(x, delay); }
    float processAt(float x, float delaySamples) {
        const float d = line.read(delaySamples);
        const float v = x - gain * d;
        line.write(v);
        return d + gain * v;
    }
};

// Topology-preserving one-pole (Zavalishin). Stable under fast modulation.
class OnePole {
public:
    void setCutoff(float hz, float sampleRate) {
        const float g = std::tan(kPi * std::clamp(hz, 1.0f, 0.49f * sampleRate) / sampleRate);
        a_ = g / (1.0f + g);
    }
    float lowpass(float x) {
        const float v = a_ * (x - s_);
        const float y = v + s_;
        s_ = y + v;
        return y;
    }
    float highpass(float x) { return x - lowpass(x); }
    void reset() { s_ = 0.0f; }

private:
    float a_ = 0.5f;
    float s_ = 0.0f;
};

// TPT state-variable filter (Simper / Cytomic). Zipper-free when modulated.
class Svf {
public:
    struct Out {
        float low, band, high;
    };
    void set(float hz, float q, float sampleRate) {
        const float g = std::tan(kPi * std::clamp(hz, 1.0f, 0.49f * sampleRate) / sampleRate);
        k_ = 1.0f / std::max(q, 0.05f);
        a1_ = 1.0f / (1.0f + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }
    Out process(float v0) {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return {v2, v1, v0 - k_ * v1 - v2};
    }
    void reset() { ic1_ = ic2_ = 0.0f; }

private:
    float k_ = 1.4f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

// tanh with first-order antiderivative anti-aliasing (Parker et al. 2016):
// near alias-free saturation without oversampling, hence zero latency.
class TanhAdaa {
public:
    float process(float x) {
        const double xd = x;
        const double diff = xd - x1_;
        double y;
        if (std::abs(diff) < 1.0e-5)
            y = std::tanh(0.5 * (xd + x1_));
        else
            y = (logCosh(xd) - logCosh(x1_)) / diff;
        x1_ = xd;
        return float(y);
    }
    void reset() { x1_ = 0.0; }

private:
    static double logCosh(double x) {
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453;
    }
    double x1_ = 0.0;
};

}  // namespace stasis
