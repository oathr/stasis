#pragma once

#include <algorithm>
#include <array>

#include "common.h"
#include "lfo.h"

namespace stasis {

// MODAL: not a room but a struck object. 64 resonant modes laid out like a rectangular
// plate's, f(m, n) = f0 · (m² + (n / 1.37)²): inharmonic, the ring of a gong, a bell, a
// sheet of metal. The sound going in excites them all; left and right listen at two
// different points of the plate (each mode with its own sign and weight per side).
//  SIZE   0..1: the plate, small (f0 240 Hz, bright and close) .. huge (f0 30 Hz, deep)
//  DECAY  RT60 of the lowest mode; higher modes shorter, by TONE
//  TONE   -1..1: dark (the highest modes ring ~5x shorter than the lowest) .. bright (all alike)
//  MOD    0..1: the modes drift slowly (up to ±0.4 %), the ring shimmers instead of standing
class ModalReverb {
public:
    static constexpr int kModes = 64;

    struct Params {
        float size = 0.5f;
        float decaySeconds = 6.0f;
        float tone = 0.0f;
        float mod = 0.3f;
        float predelayMs = 0.0f;
        bool freeze = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        // The plate's lowest 64 modes, in ratios to f0 (sorted), and how each side hears them.
        std::array<float, 400> ratios{};
        int count = 0;
        for (int m = 1; m <= 20; ++m)
            for (int n = 1; n <= 20; ++n) ratios[size_t(count++)] = float(m * m) + float(n * n) / (1.37f * 1.37f);
        std::sort(ratios.begin(), ratios.begin() + count);
        Rng rng(0x60D);
        for (int k = 0; k < kModes; ++k) {
            auto& mode = modes_[size_t(k)];
            mode.ratio = ratios[size_t(k)] / ratios[0];
            // A mode's shape: louder at some points of the plate than at others.
            mode.gainL = rng.bipolar();
            mode.gainR = rng.bipolar();
            mode.drift.prepare(sampleRate, 211 + uint32_t(k));
            mode.drift.setShape(LfoShape::RandomSmooth);
            mode.drift.setRate(0.05f + 0.25f * rng.uniform());
            mode.y1 = mode.y2 = 0.0f;
        }
        predelay_.prepare(int(0.5f * sampleRate) + 8);
        counter_ = 0;
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        f0_ = 240.0f * std::pow(0.125f, std::clamp(p.size, 0.0f, 1.0f));
        // Per mode: RT60 shortened above the lowest by TONE (exponent 0 .. 1 per ratio).
        const float damp = 0.5f * (1.0f - std::clamp(p.tone, -1.0f, 1.0f));
        for (auto& mode : modes_) {
            const float rt = p.freeze || !std::isfinite(p.decaySeconds) ? 1.0e4f
                                                                         : p.decaySeconds * std::pow(mode.ratio, -damp * 0.5f);
            mode.r = std::exp(-6.9078f / (std::max(rt, 0.05f) * sampleRate_));
        }
        update(0.0f);
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float predelay = params_.predelayMs * 0.001f * sampleRate_;
        const float inputGain = params_.freeze ? 0.0f : 1.0f;
        for (int n = 0; n < numSamples; ++n) {
            if ((counter_++ % kControl) == 0) update(std::clamp(params_.mod, 0.0f, 1.0f));
            float x = 0.5f * (inL[n] + inR[n]) * inputGain;
            if (predelay >= 2.0f) {
                const float d = predelay_.read(predelay);
                predelay_.write(x);
                x = d;
            }
            float l = 0.0f, r = 0.0f;
            for (auto& mode : modes_) {
                if (!mode.audible) continue;
                const float y = mode.a1 * mode.y1 - mode.a2 * mode.y2 + mode.b * x;
                mode.y2 = mode.y1;
                mode.y1 = y;
                l += mode.gainL * y;
                r += mode.gainR * y;
            }
            outL[n] = l * kOut;
            outR[n] = r * kOut;
        }
    }

private:
    static constexpr int kControl = 32;
    static constexpr float kOut = 0.18f;  // 64 modes of random sign (1/8) and their weights: the input's level

    struct Mode {
        float ratio = 1.0f, gainL = 0.0f, gainR = 0.0f, r = 0.99f;
        float a1 = 0.0f, a2 = 0.0f, b = 0.0f, y1 = 0.0f, y2 = 0.0f;
        bool audible = true;
        Lfo drift;
    };

    // Coefficients from each mode's frequency (drifting by MOD) and decay. b sets each mode's
    // energy: a steady noise comes out of every mode at its own level, whatever the decay.
    void update(float mod) {
        for (auto& mode : modes_) {
            const float drift = 1.0f + 0.004f * mod * mode.drift.advance(kControl);
            const float hz = f0_ * mode.ratio * drift;
            mode.audible = hz < 0.45f * sampleRate_;
            if (!mode.audible) continue;
            const float w = kTwoPi * hz / sampleRate_;
            mode.a1 = 2.0f * mode.r * std::cos(w);
            mode.a2 = mode.r * mode.r;
            mode.b = 2.0f * std::sin(w) * std::sqrt(1.0f - mode.a2);  // energy in = energy out (white noise)
        }
    }

    float sampleRate_ = 48000.0f, f0_ = 120.0f;
    Params params_;
    std::array<Mode, kModes> modes_{};
    DelayLine predelay_;
    uint32_t counter_ = 0;
};

}  // namespace stasis
