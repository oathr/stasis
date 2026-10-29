#pragma once

#include "common.h"

namespace stasis {

// Time-domain granular pitch shifter: two read heads sweep a delay line and
// crossfade with complementary sin² windows. Zero latency, a few ops per sample,
// gain <= 1, which makes it safe inside reverb feedback loops (shimmer).
class GranularPitchShifter {
public:
    void prepare(float sampleRate, float windowMs = 80.0f, float maxWindowMs = 120.0f) {
        window_ = windowMs * 0.001f * sampleRate;
        maxWindow_ = std::max(window_, maxWindowMs * 0.001f * sampleRate);
        line_.prepare(int(maxWindow_) + 16);
        phase_ = 0.0f;
    }
    // Pitch-synchronous use: when the input period T is known, a window of 2*k*T keeps
    // the two heads (half a window apart) in phase, so their crossfade never cancels.
    void setWindowSamples(float samples) { window_ = std::clamp(samples, 64.0f, maxWindow_); }
    void setSemitones(float semitones) { ratio_ = semitonesToRatio(semitones); }
    void reset() {
        line_.clear();
        phase_ = 0.0f;
    }

    float process(float x) {
        const float phaseB = phase_ < 0.5f ? phase_ + 0.5f : phase_ - 0.5f;
        const float s = std::sin(kPi * phase_);
        const float gainA = s * s;
        const float y = gainA * line_.read(2.0f + phase_ * window_) +
                        (1.0f - gainA) * line_.read(2.0f + phaseB * window_);
        line_.write(x);

        phase_ += (1.0f - ratio_) / window_;
        phase_ -= std::floor(phase_);
        return y;
    }

private:
    DelayLine line_;
    float window_ = 3840.0f, maxWindow_ = 3840.0f;
    float ratio_ = 2.0f;
    float phase_ = 0.0f;
};

}  // namespace stasis
