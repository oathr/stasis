#pragma once

#include <array>

#include "color.h"
#include "common.h"

namespace stasis {

// Plate after Dattorro, "Effect Design Part 1" (JAES 1997): input diffusers feed a
// figure-of-eight tank, stereo output from 7 taps per side. Delay lengths are given
// at the paper's 29761 Hz and scaled to the host rate (and by SIZE).
//  - DECAY is a real RT60: the tank's gain (applied 4 times per trip round the figure of
//    eight) is set from the trip's length, so the seconds shown are the seconds heard.
//  - The tank's delays are read at whole samples (rounded once): an interpolated read
//    loses treble on every pass, and over a long tail that dulled the plate whatever
//    TONE said. Only the two modulated allpasses interpolate.
//  - TONE: damping in Hz (the same at any sample rate) and a tilt on the input.
class PlateReverb {
public:
    struct Params {
        float decaySeconds = 5.0f; // RT60, INFINITY = endless (freeze too)
        float dampingHz = 6000.0f; // tank lowpass, 0 = none
        float tilt = 0.0f;         // -1 dark .. 1 bright, on the input (TONE)
        float bandwidth = 0.9995f; // input lowpass
        float size = 1.0f;         // 0.5 .. 2 length scale
        float modDepth = 0.5f;     // 0..1 (1 = 16 samples at 29761 Hz, as in the paper)
        float modRate = 1.0f;      // Hz
        float predelayMs = 10.0f;
        bool freeze = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        const float maxScale = 2.0f * sampleRate / 29761.0f;
        auto prep = [&](auto& node, float len) { node.prepare(int(len * maxScale) + 64); };
        for (size_t i = 0; i < 4; ++i) prep(inputDiffusers_[i], kInputDiffuserLen[i]);
        prep(modAllpassL_, 672 + 32);
        prep(modAllpassR_, 908 + 32);
        prep(delayL1_, 4453);
        prep(delayL2_, 3720);
        prep(delayR1_, 4217);
        prep(delayR2_, 3163);
        prep(allpassL_, 1800);
        prep(allpassR_, 2656);
        predelay_.prepare(int(0.5f * sampleRate));
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        scale_ = std::clamp(p.size, 0.5f, 2.0f) * sampleRate_ / 29761.0f;
        for (size_t i = 0; i < 4; ++i) {
            inputDiffusers_[i].delay = std::round(kInputDiffuserLen[i] * scale_);
            inputDiffusers_[i].gain = i < 2 ? 0.75f : 0.625f;
        }
        // One trip round the tank, in seconds; the gain is applied 4 times on the way.
        const float trip = kTripLength * scale_ / sampleRate_;
        const float decay = p.freeze || !std::isfinite(p.decaySeconds) ? 1.0f
                                                                        : std::pow(10.0f, -0.75f * trip / std::max(p.decaySeconds, 0.05f));
        modAllpassL_.gain = modAllpassR_.gain = -0.7f;  // decay diffusion 1 (inverted sign)
        allpassL_.delay = std::round(1800 * scale_);
        allpassR_.delay = std::round(2656 * scale_);
        for (int i = 0; i < 4; ++i) lengths_[size_t(i)] = int(std::lround(kTankLength[size_t(i)] * scale_));
        const float tilt = std::clamp(p.tilt, -1.0f, 1.0f);
        lowShelf_.set(SvfEq::Type::LowShelf, 250.0f, 0.5f, -3.0f * tilt, sampleRate_);
        highShelf_.set(SvfEq::Type::HighShelf, 3000.0f, 0.5f, tilt < 0.0f ? 9.0f * tilt : 5.0f * tilt, sampleRate_);
        allpassL_.gain = allpassR_.gain = std::clamp(decay + 0.15f, 0.25f, 0.5f);  // decay diffusion 2
        decay_ = decay;
        damping_ = p.freeze || p.dampingHz <= 0.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * std::min(p.dampingHz, 0.45f * sampleRate_) / sampleRate_);
        excursion_ = p.modDepth * 16.0f * sampleRate_ / 29761.0f;
        modInc_ = kTwoPi * p.modRate / sampleRate_;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float predelay = params_.predelayMs * 0.001f * sampleRate_;
        const float inputGain = params_.freeze ? 0.0f : 1.0f;
        const auto tap = [&](const DelayLine& line, float len) { return line.readInt(int(len * scale_) + 1); };

        for (int n = 0; n < numSamples; ++n) {
            float x = 0.5f * (inL[n] + inR[n]) * inputGain;
            if (predelay >= 2.0f) {
                const float delayed = predelay_.read(predelay);
                predelay_.write(x);
                x = delayed;
            }
            bandwidthState_ += params_.bandwidth * (x - bandwidthState_);
            x = highShelf_.process(lowShelf_.process(bandwidthState_));
            for (auto& ap : inputDiffusers_) x = ap.process(x);

            modPhase_ += modInc_;
            if (modPhase_ > kTwoPi) modPhase_ -= kTwoPi;
            const float modL = excursion_ * std::sin(modPhase_);
            const float modR = excursion_ * std::cos(modPhase_);

            // Left half of the tank, fed by the end of the right half.
            float a = modAllpassL_.processAt(x + decay_ * tankR_, (672.0f + 16.0f) * scale_ + modL);
            const float l1 = delayL1_.readInt(lengths_[0]);
            delayL1_.write(a);
            dampL_ += damping_ * (l1 - dampL_);
            a = allpassL_.process(dampL_ * decay_);
            tankL_ = delayL2_.readInt(lengths_[1]);
            delayL2_.write(a);

            // Right half, fed by the end of the left half.
            float b = modAllpassR_.processAt(x + decay_ * tankLPrev_, (908.0f + 16.0f) * scale_ + modR);
            const float r1 = delayR1_.readInt(lengths_[2]);
            delayR1_.write(b);
            dampR_ += damping_ * (r1 - dampR_);
            b = allpassR_.process(dampR_ * decay_);
            tankR_ = delayR2_.readInt(lengths_[3]);
            delayR2_.write(b);
            tankLPrev_ = tankL_;

            float yl = tap(delayR1_, 266) + tap(delayR1_, 2974) - tap(allpassR_.line, 1913) + tap(delayR2_, 1996) -
                       tap(delayL1_, 1990) - tap(allpassL_.line, 187) - tap(delayL2_, 1066);
            float yr = tap(delayL1_, 353) + tap(delayL1_, 3627) - tap(allpassL_.line, 1228) + tap(delayL2_, 2673) -
                       tap(delayR1_, 2111) - tap(allpassR_.line, 335) - tap(delayR2_, 121);
            outL[n] = yl * 0.6f;
            outR[n] = yr * 0.6f;
        }
    }

private:
    static constexpr std::array<float, 4> kInputDiffuserLen{142.0f, 107.0f, 379.0f, 277.0f};
    static constexpr std::array<float, 4> kTankLength{4453.0f, 3720.0f, 4217.0f, 3163.0f};  // L1 L2 R1 R2
    // Both halves: modulated allpass + delay + allpass + delay (paper samples).
    static constexpr float kTripLength = 672.0f + 4453.0f + 1800.0f + 3720.0f + 908.0f + 4217.0f + 2656.0f + 3163.0f;

    float sampleRate_ = 48000.0f;
    Params params_;
    float scale_ = 1.0f, decay_ = 0.7f, damping_ = 1.0f, excursion_ = 0.0f;
    std::array<int, 4> lengths_{};
    SvfEq lowShelf_, highShelf_;
    float modPhase_ = 0.0f, modInc_ = 0.0f;
    float bandwidthState_ = 0.0f, dampL_ = 0.0f, dampR_ = 0.0f;
    float tankL_ = 0.0f, tankR_ = 0.0f, tankLPrev_ = 0.0f;
    std::array<Allpass, 4> inputDiffusers_;
    Allpass modAllpassL_, modAllpassR_, allpassL_, allpassR_;
    DelayLine delayL1_, delayL2_, delayR1_, delayR2_, predelay_;
};

}  // namespace stasis
