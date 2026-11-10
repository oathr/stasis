#pragma once

#include "color.h"
#include "common.h"

namespace stasis {

// 80s: what goes around the HALL engine to make it a digital reverb of the eighties.
//  before  the input band-limited, 120 Hz .. 8 kHz (the converters and the sample rates
//          of the time)
//  after   the GATE: open while the sound plays, held, then shut in 60 ms. The gated
//          drum reverb; SIZE sets the hold, 50 ms .. 1.5 s (long: an 80s hall). No bit
//          reduction: 12-bit rounding added 15 dB of grit above 2 kHz (measured), heard as noise.
class Eighties {
public:
    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (int c = 0; c < 2; ++c) {
            low_[c].set(SvfEq::Type::Highpass, 120.0f, 0.707f, 0.0f, sampleRate);
            high_[c].set(SvfEq::Type::Lowpass, 8000.0f, 0.707f, 0.0f, sampleRate);
        }
        attack_ = smoothingCoef(0.001f, sampleRate);
        release_ = smoothingCoef(0.02f, sampleRate);
        peakFall_ = smoothingCoef(2.0f, sampleRate);
        closeCoef_ = smoothingCoef(0.06f, sampleRate);
        openCoef_ = smoothingCoef(0.002f, sampleRate);
        env_ = peak_ = 0.0f;
        gate_ = 0.0f;
        held_ = 0;
        setHold(0.5f);
    }

    // SIZE 0..1 → how long the gate stays open after the sound stops.
    void setHold(float size) { holdSamples_ = int((0.05f + 1.45f * std::clamp(size, 0.0f, 1.0f) * std::clamp(size, 0.0f, 1.0f)) * sampleRate_); }

    void before(const float* inL, const float* inR, float* outL, float* outR, int n) {
        for (int i = 0; i < n; ++i) {
            outL[i] = high_[0].process(low_[0].process(inL[i]));
            outR[i] = high_[1].process(low_[1].process(inR[i]));
        }
    }

    // `inL / inR`: what came into SPACE (the gate listens to it); the tail in place.
    void after(const float* inL, const float* inR, float* tailL, float* tailR, int n) {
        for (int i = 0; i < n; ++i) {
            const float level = std::max(std::abs(inL[i]), std::abs(inR[i]));
            env_ += (level > env_ ? attack_ : release_) * (level - env_);
            peak_ = std::max(env_, peak_ + peakFall_ * (0.0f - peak_));
            // Open while the sound is within 26 dB of its recent peak (and above -60 dBFS).
            if (env_ > std::max(1.0e-3f, 0.05f * peak_)) held_ = holdSamples_;
            else if (held_ > 0) --held_;
            const float target = held_ > 0 ? 1.0f : 0.0f;
            gate_ += (target > gate_ ? openCoef_ : closeCoef_) * (target - gate_);
            tailL[i] *= gate_;
            tailR[i] *= gate_;
        }
    }

private:
    float sampleRate_ = 48000.0f;
    SvfEq low_[2], high_[2];
    float attack_ = 0.1f, release_ = 0.01f, peakFall_ = 0.001f, closeCoef_ = 0.001f, openCoef_ = 0.01f;
    float env_ = 0.0f, peak_ = 0.0f, gate_ = 0.0f;
    int held_ = 0, holdSamples_ = 0;
};

}  // namespace stasis
