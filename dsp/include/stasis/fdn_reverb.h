#pragma once

#include <array>

#include "common.h"
#include "pitch_shifter.h"

namespace stasis {

// 16-line feedback delay network: Room → Hall → Cathedral, plus Shimmer and Freeze.
//  - input pre-diffusion (allpass chains per channel)
//  - fast Hadamard feedback matrix (orthogonal, lossless, O(N log N))
//  - 3-band frequency-dependent absorption per line (Jot): each band has its own RT60
//  - slowly modulated line lengths to break up metallic resonances
//  - shimmer: one orthonormal component of the feedback runs through a pitch
//    shifter, so the loop stays bounded by construction (shifter gain <= 1)
class FdnReverb {
public:
    static constexpr int kLines = 16;

    struct Params {
        float size = 0.7f;            // 0 = room .. 1 = cathedral
        float decaySeconds = 8.0f;    // mid-band RT60, INFINITY = endless
        float bassMultiplier = 1.2f;  // RT60 multiplier below 250 Hz
        float hfCrossoverHz = 4000.0f;
        float hfRatio = 0.5f;         // RT60 multiplier above the crossover
        float modDepth = 0.5f;        // 0..1
        float modRate = 0.3f;         // Hz
        float diffusion = 0.75f;      // 0..1
        float predelayMs = 20.0f;
        float shimmer = 0.0f;         // 0..1
        float shimmerSemitones = 12.0f;
        bool freeze = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        const int maxLine = int(kMaxLineMs * 0.001f * sampleRate + kMaxModMs * 0.001f * sampleRate) + 8;
        for (auto& line : lines_) line.prepare(maxLine);
        for (int c = 0; c < 2; ++c)
            for (auto& ap : diffusers_[size_t(c)]) ap.prepare(int(0.04f * sampleRate));
        predelayL_.prepare(int(0.5f * sampleRate));
        predelayR_.prepare(int(0.5f * sampleRate));
        shifter_.prepare(sampleRate, 90.0f);
        shimmerHp_.setCutoff(250.0f, sampleRate);
        shimmerLp_.setCutoff(7000.0f, sampleRate);

        Rng rng(0xA11CE);
        for (int i = 0; i < kLines; ++i) {
            lowSplit_[size_t(i)].setCutoff(250.0f, sampleRate);
            lengthJitter_[size_t(i)] = rng.bipolar() * 0.35f;
            modRateJitter_[size_t(i)] = 0.6f + 0.8f * rng.uniform();
            modSin_[size_t(i)] = std::sin(kTwoPi * rng.uniform());
            modCos_[size_t(i)] = std::sqrt(1.0f - modSin_[size_t(i)] * modSin_[size_t(i)]);
        }
        setParams(params_);
        lengths_ = targetLengths_;
        updateAbsorption();
        snapNextParams_ = true;
    }

    void setParams(const Params& p) {
        params_ = p;
        const float minMs = 12.0f + 70.0f * p.size;
        const float maxMs = 35.0f + 215.0f * p.size;
        for (int i = 0; i < kLines; ++i) {
            const float x = std::clamp((float(kOrder[size_t(i)]) + lengthJitter_[size_t(i)]) / float(kLines - 1), 0.0f, 1.0f);
            targetLengths_[size_t(i)] = minMs * std::pow(maxMs / minMs, x) * 0.001f * sampleRate_;
        }
        const float g = 0.4f + 0.3f * p.diffusion;
        const float scale = 0.5f + p.size;
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < kDiffusers; ++k) {
                auto& ap = diffusers_[size_t(c)][size_t(k)];
                ap.delay = kDiffuserMs[size_t(k)] * (c ? 1.13f : 1.0f) * scale * 0.001f * sampleRate_;
                ap.gain = g;
            }
        }
        for (auto& f : highSplit_) f.setCutoff(p.hfCrossoverHz, sampleRate_);
        modDepthSamples_ = p.modDepth * kMaxModMs * 0.001f * sampleRate_;
        shifter_.setSemitones(p.shimmerSemitones);
        // The first settings after prepare() apply at once: gliding the line lengths
        // there would bend the pitch of the whole start of the tail (Doppler).
        if (snapNextParams_) {
            lengths_ = targetLengths_;
            snapNextParams_ = false;
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        // Size glides instead of jumping (also what makes SIZE modulation smooth).
        const float glide = smoothingCoef(0.3f, sampleRate_, numSamples);
        for (int i = 0; i < kLines; ++i) lengths_[size_t(i)] += (targetLengths_[size_t(i)] - lengths_[size_t(i)]) * glide;
        updateAbsorption();

        const float predelay = params_.predelayMs * 0.001f * sampleRate_;
        const float inputGain = params_.freeze ? 0.0f : 0.35f;
        const float shimmer = params_.shimmer;
        const float modFreq = 2.0f * std::sin(kPi * params_.modRate / sampleRate_);
        std::array<float, kLines> y{};

        for (int n = 0; n < numSamples; ++n) {
            float l = inL[n] * inputGain, r = inR[n] * inputGain;
            if (predelay >= 2.0f) {
                predelayL_.write(l);
                predelayR_.write(r);
                l = predelayL_.read(predelay);
                r = predelayR_.read(predelay);
            }
            for (auto& ap : diffusers_[0]) l = ap.process(l);
            for (auto& ap : diffusers_[1]) r = ap.process(r);

            float wetL = 0.0f, wetR = 0.0f;
            for (int i = 0; i < kLines; ++i) {
                const size_t k = size_t(i);
                // "Magic circle" quadrature oscillator: one multiply-add per line, no trig.
                const float f = modFreq * modRateJitter_[k];
                modSin_[k] += f * modCos_[k];
                modCos_[k] -= f * modSin_[k];

                const float v = lines_[k].read(lengths_[k] + modDepthSamples_ * (1.0f + modSin_[k]));
                const float low = lowSplit_[k].lowpass(v);
                const float rest = v - low;
                const float mid = highSplit_[k].lowpass(rest);
                const float out = gainLow_[k] * low + gainMid_[k] * mid + gainHigh_[k] * (rest - mid);
                y[k] = out;
                if (i & 1)
                    wetR += out * kSigns[k];
                else
                    wetL += out * kSigns[k];
            }
            outL[n] = wetL * 0.35f;
            outR[n] = wetR * 0.35f;

            hadamard(y);

            if (shimmer > 0.0f) {
                // u = (1,1,1,1,0..)/2 is unit-norm: swap that component for its pitch-shifted copy.
                const float s = 0.5f * (y[0] + y[1] + y[2] + y[3]);
                const float shifted = shimmerLp_.lowpass(shimmerHp_.highpass(shifter_.process(s)));
                const float delta = 0.5f * shimmer * (shifted - s);
                for (size_t k = 0; k < 4; ++k) y[k] += delta;
            }

            for (int i = 0; i < kLines; ++i) {
                const size_t k = size_t(i);
                lines_[k].write(y[k] + ((i & 1) ? r : l) * kSigns[k]);
            }
        }
    }

private:
    static constexpr float kMaxLineMs = 260.0f;
    static constexpr float kMaxModMs = 2.0f;
    static constexpr int kDiffusers = 4;
    static constexpr std::array<float, kDiffusers> kDiffuserMs{4.7f, 3.6f, 12.7f, 9.3f};
    // Spreads short and long lines across both output channels (even = L, odd = R).
    static constexpr std::array<int, kLines> kOrder{0, 9, 3, 12, 6, 15, 1, 10, 4, 13, 7, 14, 2, 11, 5, 8};
    static constexpr std::array<float, kLines> kSigns{1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1, -1, -1, -1, 1, 1};

    static void hadamard(std::array<float, kLines>& x) {
        for (int h = 1; h < kLines; h <<= 1)
            for (int i = 0; i < kLines; i += h * 2)
                for (int j = i; j < i + h; ++j) {
                    const float a = x[size_t(j)], b = x[size_t(j + h)];
                    x[size_t(j)] = a + b;
                    x[size_t(j + h)] = a - b;
                }
        for (auto& v : x) v *= 0.25f;  // 1/sqrt(16): orthonormal
    }

    void updateAbsorption() {
        const float rt = params_.freeze ? INFINITY : params_.decaySeconds;
        for (int i = 0; i < kLines; ++i) {
            const float len = lengths_[size_t(i)];
            gainLow_[size_t(i)] = rt60Gain(len, rt * params_.bassMultiplier, sampleRate_);
            gainMid_[size_t(i)] = rt60Gain(len, rt, sampleRate_);
            gainHigh_[size_t(i)] = rt60Gain(len, rt * params_.hfRatio, sampleRate_);
        }
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    bool snapNextParams_ = false;
    std::array<DelayLine, kLines> lines_;
    std::array<std::array<Allpass, kDiffusers>, 2> diffusers_;
    DelayLine predelayL_, predelayR_;
    std::array<float, kLines> lengths_{}, targetLengths_{}, lengthJitter_{};
    std::array<float, kLines> gainLow_{}, gainMid_{}, gainHigh_{};
    std::array<OnePole, kLines> lowSplit_, highSplit_;
    std::array<float, kLines> modSin_{}, modCos_{}, modRateJitter_{};
    float modDepthSamples_ = 0.0f;
    GranularPitchShifter shifter_;
    OnePole shimmerHp_, shimmerLp_;
};

}  // namespace stasis
