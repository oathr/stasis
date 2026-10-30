#pragma once

#include "common.h"

namespace stasis {

// GLUE: the last stage, a soft and slow compressor on the whole output, one knob.
//  AMOUNT 0..1  0 = off (nothing computed). Up: the threshold goes down (-8 → -30 dBFS)
//               and the ratio up (1.5:1 → 3:1). A 12 dB soft knee, a stereo-linked RMS
//               detector (30 ms), 60 ms attack and 600 ms release: it rides the level,
//               it never pumps or grabs the attacks.
// Makeup is automatic and set so a signal at -14 dBFS RMS (a sensible mix level) comes out
// unchanged: louder passages are brought down towards it, quieter ones lifted a little.
class Glue {
public:
    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        detectCoef_ = smoothingCoef(0.03f, sampleRate);
        attackCoef_ = smoothingCoef(0.06f, sampleRate);
        releaseCoef_ = smoothingCoef(0.6f, sampleRate);
        power_ = 0.0f;
        reductionDb_ = 0.0f;
        setAmount(amount_);
    }

    void setAmount(float amount) {
        amount_ = std::clamp(amount, 0.0f, 1.0f);
        thresholdDb_ = -8.0f - 22.0f * amount_;
        ratio_ = 1.5f + 1.5f * amount_;
        makeupDb_ = gainReductionDb(kReferenceDb);
    }
    bool active() const { return amount_ > 0.0f; }
    float reductionDb() const { return reductionDb_; }

    // One stereo sample, in place.
    void process(float& l, float& r) {
        power_ += detectCoef_ * (0.5f * (l * l + r * r) - power_);
        const float levelDb = 10.0f * std::log10(power_ + 1.0e-12f);
        const float target = gainReductionDb(levelDb);
        reductionDb_ += (target - reductionDb_) * (target > reductionDb_ ? attackCoef_ : releaseCoef_);
        const float gain = dbToGain(makeupDb_ - reductionDb_);
        l *= gain;
        r *= gain;
    }

private:
    static constexpr float kKneeDb = 12.0f, kReferenceDb = -14.0f;

    // dB taken off a signal at levelDb (soft knee: quadratic over kKneeDb around the threshold).
    float gainReductionDb(float levelDb) const {
        const float over = levelDb - thresholdDb_, slope = 1.0f - 1.0f / ratio_;
        if (over <= -0.5f * kKneeDb) return 0.0f;
        if (over >= 0.5f * kKneeDb) return slope * over;
        const float x = over + 0.5f * kKneeDb;
        return slope * x * x / (2.0f * kKneeDb);
    }

    float sampleRate_ = 48000.0f, amount_ = 0.0f, thresholdDb_ = -8.0f, ratio_ = 1.5f, makeupDb_ = 0.0f;
    float detectCoef_ = 0.01f, attackCoef_ = 0.001f, releaseCoef_ = 0.0001f, power_ = 0.0f, reductionDb_ = 0.0f;
};

}  // namespace stasis
