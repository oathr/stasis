#pragma once

#include "color.h"
#include "common.h"

namespace stasis {

// SHAPE: the first touch on the sound, before the chain.
//  TILT   -1..1  ±6 dB tilt around 800 Hz (dark ◂▸ bright), two shelves
//  DRIVE   0..1  tanh saturation, antialiased (ADAA). Level is matched at -12 dBFS:
//                quieter parts come up a little, louder ones round off
//  HARM    0..1  asymmetry: adds even harmonics (warm, tube-like); the DC it creates
//                is removed
class Shape {
public:
    struct Params {
        float tilt = 0.0f;
        float drive = 0.0f;
        float harm = 0.0f;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& d : dcBlock_) d.setCutoff(8.0f, sampleRate);
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        const float tiltDb = 6.0f * std::clamp(p.tilt, -1.0f, 1.0f);
        for (int c = 0; c < 2; ++c) {
            low_[c].set(SvfEq::Type::LowShelf, 800.0f, 0.5f, -tiltDb, sampleRate_);
            high_[c].set(SvfEq::Type::HighShelf, 800.0f, 0.5f, tiltDb, sampleRate_);
        }
        gain_ = 1.0f + 7.0f * p.drive * p.drive;
        bias_ = 0.4f * std::clamp(p.harm, 0.0f, 1.0f);
        biasOut_ = std::tanh(bias_);
        constexpr float kReference = 0.25f;  // -12 dBFS keeps its level
        norm_ = kReference / (std::tanh(gain_ * kReference + bias_) - biasOut_);
        saturate_ = p.drive > 1.0e-3f || p.harm > 1.0e-3f;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float* in[2] = {inL, inR};
        float* out[2] = {outL, outR};
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < numSamples; ++n) {
                float v = high_[c].process(low_[c].process(in[c][n]));
                if (saturate_) v = dcBlock_[c].highpass((sat_[c].process(gain_ * v + bias_) - biasOut_) * norm_);
                out[c][n] = v;
            }
    }

private:
    float sampleRate_ = 48000.0f;
    Params params_;
    SvfEq low_[2], high_[2];
    TanhAdaa sat_[2];
    OnePole dcBlock_[2];
    float gain_ = 1.0f, bias_ = 0.0f, biasOut_ = 0.0f, norm_ = 1.0f;
    bool saturate_ = false;
};

}  // namespace stasis
