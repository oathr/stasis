#pragma once

#include <array>
#include <complex>

#include "common.h"

namespace stasis {

// RESONATOR: a bank of tuned feedback combs (string-like, damped by a lowpass in the
// loop) in front of the SPACE reverb, so the tail rings on notes. The notes are pitch
// classes (bit n = class n, 0 = C), each played in two octaves; SIZE picks the register.
// The lowpass's phase delay is taken off each loop so the combs stay in tune, and a
// chord change glides the combs (portamento) and fades the slots in and out.
class Resonator {
public:
    static constexpr int kVoices = 24;  // 12 classes x 2 octaves

    struct Params {
        uint16_t classes = 0x81;   // C and G
        float size = 0.7f;         // 0 = C4..B5, 1 = C2..B3
        float decaySeconds = 6.0f; // ring time (RT60) of the combs
        float tone = 0.5f;         // damping: 0 dark .. 1 bright
        bool freeze = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& side : lines_)
            for (auto& l : side) l.prepare(int(sampleRate / 60.0f) + 8);  // lowest note: C2, 65 Hz
        for (auto& side : damp_)
            for (auto& d : side) d.reset();
        voiceGain_.fill(0.0f);
        glide_ = smoothingCoef(0.04f, sampleRate);
        fade_ = smoothingCoef(0.05f, sampleRate);
        snap_ = true;
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        const int lowC = 60 - int(std::round(std::clamp(p.size, 0.0f, 1.0f) * 2.0f)) * 12;  // MIDI of the lower octave's C
        const float cutoff = 1200.0f * std::pow(12.0f, std::clamp(p.tone, 0.0f, 1.0f));
        const float rt = p.freeze ? 600.0f : std::max(p.decaySeconds, 0.1f);
        const float g = std::tan(kPi * std::min(cutoff, 0.45f * sampleRate_) / sampleRate_);
        int active = 0;
        for (int v = 0; v < kVoices; ++v) {
            const size_t k = size_t(v);
            const int pc = v % 12, octave = v / 12;
            on_[k] = (p.classes >> pc) & 1;
            active += on_[k];
            const float hz = 440.0f * std::pow(2.0f, float(lowC + 12 * octave + pc - 69) / 12.0f);
            // The loop's lowpass (bilinear one-pole) at the note: its gain and phase delay.
            const float w = kTwoPi * hz / sampleRate_;
            const std::complex<float> z1 = std::polar(1.0f, -w);
            const std::complex<float> h = g * (1.0f + z1) / ((1.0f + g) + (g - 1.0f) * z1);
            const float lpDelay = -std::arg(h) / w;
            for (int c = 0; c < 2; ++c) {
                // Left and right a few cents apart: the same note, wide.
                const float detune = c == 0 ? 0.9985f : 1.0015f;
                targetPeriod_[size_t(c)][k] = std::max(2.0f, sampleRate_ / (hz * detune) - lpDelay);
                damp_[size_t(c)][k].setCutoff(cutoff, sampleRate_);
            }
            feedback_[k] = std::min(rt60Gain(sampleRate_ / hz, rt, sampleRate_) / std::abs(h), 0.99995f);
            excite_[k] = std::sqrt(std::max(1.0f - feedback_[k] * feedback_[k], 1.0e-4f));
        }
        level_ = 0.5f / std::sqrt(float(std::max(active, 1)));
        if (snap_) {
            period_ = targetPeriod_;
            snap_ = false;
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float inputGain = params_.freeze ? 0.0f : 1.0f;
        for (int n = 0; n < numSamples; ++n) {
            float l = 0.0f, r = 0.0f;
            for (int v = 0; v < kVoices; ++v) {
                const size_t k = size_t(v);
                float& gain = voiceGain_[k];
                gain += ((on_[k] ? 1.0f : 0.0f) - gain) * fade_;
                if (gain < 1.0e-5f && !on_[k]) continue;
                for (int c = 0; c < 2; ++c) {
                    float& period = period_[size_t(c)][k];
                    period += (targetPeriod_[size_t(c)][k] - period) * glide_;
                    auto& line = lines_[size_t(c)][k];
                    const float x = (c == 0 ? inL[n] : inR[n]) * inputGain * excite_[k] * gain;
                    const float y = damp_[size_t(c)][k].lowpass(line.read(period)) * feedback_[k];
                    line.write(x + y);
                    (c == 0 ? l : r) += y * gain;
                }
            }
            outL[n] = l * level_;
            outR[n] = r * level_;
        }
    }

private:
    float sampleRate_ = 48000.0f;
    Params params_;
    bool snap_ = true;
    std::array<std::array<DelayLine, kVoices>, 2> lines_;
    std::array<std::array<OnePole, kVoices>, 2> damp_;
    std::array<std::array<float, kVoices>, 2> period_{}, targetPeriod_{};
    std::array<float, kVoices> feedback_{}, excite_{}, voiceGain_{};
    std::array<bool, kVoices> on_{};
    float glide_ = 0.001f, fade_ = 0.001f, level_ = 0.1f;
};

}  // namespace stasis
