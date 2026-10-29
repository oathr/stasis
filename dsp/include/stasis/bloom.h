#pragma once

#include <array>

#include "common.h"

namespace stasis {

// BLOOM: a reverb that swells in instead of starting at its loudest. Two 8-line feedback
// networks in series (Hadamard, no direct path) run in front of the SPACE reverb: their
// decays convolve with the reverb's, so the tail's envelope rises from silence, peaks
// well after the attack (the longer SWELL, the later) and then fades with DECAY.
class BloomFeeder {
public:
    static constexpr int kLines = 8, kStages = 2;

    struct Params {
        float size = 0.7f;           // 0..1, scales the line lengths
        float swellSeconds = 2.0f;   // RT60 of each stage: sets how long the swell takes
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& stage : lines_)
            for (auto& l : stage) l.prepare(int(kMaxMs * 0.001f * sampleRate) + 8);
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        const float scale = 0.6f + 0.8f * std::clamp(p.size, 0.0f, 1.0f);
        for (int s = 0; s < kStages; ++s) {
            float energy = 0.0f;
            for (int i = 0; i < kLines; ++i) {
                const size_t k = size_t(i);
                const float ms = kLineMs[k] * (s == 0 ? 1.0f : kSecondStage) * scale;
                lengths_[size_t(s)][k] = std::max(2, int(ms * 0.001f * sampleRate_));
                gains_[size_t(s)][k] = rt60Gain(float(lengths_[size_t(s)][k]), std::max(p.swellSeconds, 0.05f), sampleRate_);
                energy += gains_[size_t(s)][k] * gains_[size_t(s)][k];
            }
            // A loop of gain g carries 1 / (1 - g²) times its input's energy: keep the
            // level the same whatever the swell.
            norm_[size_t(s)] = std::sqrt(std::max(1.0f - energy / float(kLines), 1.0e-4f));
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        for (int n = 0; n < numSamples; ++n) {
            float l = inL[n], r = inR[n];
            for (int s = 0; s < kStages; ++s) stage(s, l, r);
            outL[n] = l * 0.8f;
            outR[n] = r * 0.8f;
        }
    }

private:
    static constexpr float kMaxMs = 230.0f;
    static constexpr float kSecondStage = 1.37f;  // the second network's lines are longer: no shared echoes
    static constexpr std::array<float, kLines> kLineMs{43.1f, 50.7f, 58.3f, 67.9f, 77.3f, 88.1f, 99.7f, 113.9f};
    static constexpr std::array<float, kLines> kInputSign{1, 1, -1, 1, 1, -1, -1, -1};

    // One network: l, r in, its delayed output back in l, r.
    void stage(int s, float& l, float& r) {
        auto& lines = lines_[size_t(s)];
        std::array<float, kLines> y{};
        for (int i = 0; i < kLines; ++i) y[size_t(i)] = lines[size_t(i)].readInt(lengths_[size_t(s)][size_t(i)]) * gains_[size_t(s)][size_t(i)];
        float outL = 0.0f, outR = 0.0f;
        for (int i = 0; i < kLines; i += 2) outL += y[size_t(i)], outR += y[size_t(i + 1)];
        hadamard(y);
        for (int i = 0; i < kLines; ++i) lines[size_t(i)].write(y[size_t(i)] + ((i & 1) ? r : l) * kInputSign[size_t(i)]);
        l = outL * norm_[size_t(s)];
        r = outR * norm_[size_t(s)];
    }

    static void hadamard(std::array<float, kLines>& x) {
        for (int h = 1; h < kLines; h <<= 1)
            for (int i = 0; i < kLines; i += h * 2)
                for (int j = i; j < i + h; ++j) {
                    const float a = x[size_t(j)], b = x[size_t(j + h)];
                    x[size_t(j)] = a + b;
                    x[size_t(j + h)] = a - b;
                }
        for (auto& v : x) v *= 0.35355339f;  // 1/sqrt(8)
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    std::array<std::array<DelayLine, kLines>, kStages> lines_;
    std::array<std::array<int, kLines>, kStages> lengths_{};
    std::array<std::array<float, kLines>, kStages> gains_{};
    std::array<float, kStages> norm_{};
};

}  // namespace stasis
