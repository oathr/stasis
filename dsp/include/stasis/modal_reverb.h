#pragma once

#include <algorithm>
#include <array>

#include "color.h"
#include "common.h"
#include "lfo.h"

namespace stasis {

// MODAL: not a room but a struck object. 160 resonant modes laid out like a rectangular
// plate's, f(m, n) = f0 · (m² + (n / 1.37)²): inharmonic, the ring of a gong, a bell, a
// sheet of metal. The sound going in excites them all; left and right listen at two
// different points of the plate (each mode with its own sign and weight per side). The
// highs are a little softer (weight ratio^-1/4), and a short diffusion (4 allpasses per
// side) blooms the strike into a resonance: with 64 bare modes it rang as a few notes.
//  SIZE   0..1: the plate, small (f0 240 Hz, bright and close) .. huge (f0 30 Hz, deep)
//  DECAY  RT60 of the lowest mode; higher modes shorter, by TONE
//  TONE   -1..1: dark (a muffled strike, the high modes die up to 80x sooner than the
//         lowest) .. bright (a hard strike, all modes ring alike)
//  MOD    0..1: TENSION, the gong's pitch bend: struck hard, a plate rings higher, and
//         falls back as it decays (the "whoosh" down of a tam-tam or a tom). The modes
//         follow the ring's energy, up to +25 % (four semitones) on a loud strike; they
//         also drift slowly (up to ±1 %), so the ring moves instead of standing
class ModalReverb {
public:
    static constexpr int kModes = 160;

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
        std::array<float, 1600> ratios{};
        int count = 0;
        for (int m = 1; m <= 40; ++m)
            for (int n = 1; n <= 40; ++n) ratios[size_t(count++)] = float(m * m) + float(n * n) / (1.37f * 1.37f);
        std::sort(ratios.begin(), ratios.begin() + count);
        Rng rng(0x60D);
        for (int k = 0; k < kModes; ++k) {
            auto& mode = modes_[size_t(k)];
            mode.ratio = ratios[size_t(k)] / ratios[0];
            // A mode's shape: louder at some points of the plate than at others.
            const float weight = std::pow(mode.ratio, -0.25f);
            mode.gainL = weight * rng.bipolar();
            mode.gainR = weight * rng.bipolar();
            mode.drift.prepare(sampleRate, 211 + uint32_t(k));
            mode.drift.setShape(LfoShape::RandomSmooth);
            mode.drift.setRate(0.05f + 0.25f * rng.uniform());
            mode.y1 = mode.y2 = 0.0f;
        }
        predelay_.prepare(int(0.5f * sampleRate) + 8);
        ring_ = 0.0f;
        ringUp_ = smoothingCoef(0.005f, sampleRate);
        ringDown_ = smoothingCoef(0.05f, sampleRate);
        // The bloom: 4 allpasses per side, 3 to 17 ms, different for left and right.
        static constexpr float kDiffuseMs[2][4] = {{3.1f, 5.3f, 9.7f, 16.1f}, {3.7f, 6.1f, 11.3f, 17.3f}};
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4; ++i) {
                auto& ap = diffuse_[c][i];
                ap.prepare(int(0.02f * sampleRate) + 8);
                ap.delay = std::round(kDiffuseMs[c][i] * 0.001f * sampleRate);
                ap.gain = 0.6f;
            }
        counter_ = 0;
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        f0_ = 240.0f * std::pow(0.125f, std::clamp(p.size, 0.0f, 1.0f));
        // Per mode: RT60 shortened above the lowest by TONE (exponent 0 .. 1 per ratio).
        const float damp = 0.75f * (1.0f - std::clamp(p.tone, -1.0f, 1.0f));  // 0 .. 1.5
        // The strike itself: a hard mallet (bright) or a felt one (dark).
        strike_.set(SvfEq::Type::HighShelf, 1500.0f, 0.5f, 10.0f * std::clamp(p.tone, -1.0f, 1.0f), sampleRate_);
        for (auto& mode : modes_) {
            const float rt = p.freeze || !std::isfinite(p.decaySeconds) ? 1.0e4f
                                                                         : p.decaySeconds * std::pow(mode.ratio, -damp);
            mode.r = std::exp(-6.9078f / (std::max(rt, 0.05f) * sampleRate_));
        }
        update(0.0f);
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float predelay = params_.predelayMs * 0.001f * sampleRate_;
        const float inputGain = params_.freeze ? 0.0f : 1.0f;
        for (int n = 0; n < numSamples; ++n) {
            if ((counter_++ % kControl) == 0) update(std::clamp(params_.mod, 0.0f, 1.0f));
            float x = strike_.process(0.5f * (inL[n] + inR[n]) * inputGain);
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
            // TENSION follows the ring's energy: fast up, then down with the ring itself.
            const float energy = l * l + r * r;
            ring_ += (energy > ring_ ? ringUp_ : ringDown_) * (energy - ring_);
            l *= kOut, r *= kOut;
            for (auto& ap : diffuse_[0]) l = ap.process(l);
            for (auto& ap : diffuse_[1]) r = ap.process(r);
            outL[n] = l;
            outR[n] = r;
        }
    }

private:
    static constexpr int kControl = 32;
    static constexpr float kOut = 0.14f;  // 160 modes of random sign and their weights: the input's level

    struct Mode {
        float ratio = 1.0f, gainL = 0.0f, gainR = 0.0f, r = 0.99f;
        float a1 = 0.0f, a2 = 0.0f, b = 0.0f, y1 = 0.0f, y2 = 0.0f;
        bool audible = true;
        Lfo drift;
    };

    // Coefficients from each mode's frequency (drifting by MOD) and decay. b sets each mode's
    // energy: a steady noise comes out of every mode at its own level, whatever the decay.
    void update(float mod) {
        // How hard the plate rings now (0 .. 1 at about -10 dBFS out), as a pitch factor.
        const float bend = 1.0f + 0.25f * mod * std::min(1.0f, std::sqrt(ring_) * kOut * 3.0f);
        for (auto& mode : modes_) {
            const float drift = (1.0f + 0.01f * mod * mode.drift.advance(kControl)) * bend;
            const float hz = f0_ * mode.ratio * drift;
            mode.audible = hz < 0.45f * sampleRate_;
            if (!mode.audible) continue;
            const float w = kTwoPi * hz / sampleRate_;
            mode.a1 = 2.0f * mode.r * std::cos(w);
            mode.a2 = mode.r * mode.r;
            mode.b = 2.0f * std::sin(w) * std::sqrt(1.0f - mode.a2);  // energy in = energy out (white noise)
        }
    }

    float sampleRate_ = 48000.0f, f0_ = 120.0f, ring_ = 0.0f, ringUp_ = 0.01f, ringDown_ = 0.001f;
    SvfEq strike_;
    Params params_;
    std::array<Mode, kModes> modes_{};
    DelayLine predelay_;
    Allpass diffuse_[2][4];
    uint32_t counter_ = 0;
};

}  // namespace stasis
