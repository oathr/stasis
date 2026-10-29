#pragma once

#include "common.h"
#include "lfo.h"

namespace stasis {

// Tape echo: wow (slow random + sine) and flutter on a Hermite-interpolated line,
// ADAA saturation and tape bandwidth filters inside the feedback loop (no hiss: the loop stays clean).
// Delay-time changes glide like a tape speed change (pitch bends, no clicks).
// SYNC locks the time to the host tempo (1/1 .. 1/32, dotted, triplet).
class TapeEcho {
public:
    // SYNC divisions of a 4/4 bar: straight, dotted (x1.5) and triplet (x2/3).
    enum class Division { Whole, Half, HalfDotted, HalfTriplet, Quarter, QuarterDotted, QuarterTriplet,
                          Eighth, EighthDotted, EighthTriplet, Sixteenth, SixteenthDotted, SixteenthTriplet, ThirtySecond };

    static float divisionBeats(Division d) {  // in quarter notes
        switch (d) {
            case Division::Whole: return 4.0f;
            case Division::Half: return 2.0f;
            case Division::HalfDotted: return 3.0f;
            case Division::HalfTriplet: return 4.0f / 3.0f;
            case Division::Quarter: return 1.0f;
            case Division::QuarterDotted: return 1.5f;
            case Division::QuarterTriplet: return 2.0f / 3.0f;
            case Division::Eighth: return 0.5f;
            case Division::EighthDotted: return 0.75f;
            case Division::EighthTriplet: return 1.0f / 3.0f;
            case Division::Sixteenth: return 0.25f;
            case Division::SixteenthDotted: return 0.375f;
            case Division::SixteenthTriplet: return 1.0f / 6.0f;
            case Division::ThirtySecond: return 0.125f;
        }
        return 1.0f;
    }

    struct Params {
        float timeMs = 420.0f;
        float feedback = 0.6f;   // > 1 self-oscillates, tamed by the saturation
        float wow = 0.4f;        // 0..1
        float flutter = 0.3f;    // 0..1
        float drive = 1.5f;      // saturation input gain
        float lowCutHz = 120.0f;
        float highCutHz = 4500.0f;
        float spread = 0.06f;    // right channel time offset ratio
        bool pingPong = false;
        bool sync = false;       // SYNC: time follows the host tempo instead of timeMs
        float bpm = 120.0f;      // host tempo, updated by the plugin every block
        Division division = Division::QuarterDotted;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& line : lines_) line.prepare(int(kMaxSeconds * sampleRate) + 64);
        wowA_.prepare(sampleRate, 11);
        wowA_.setShape(LfoShape::RandomSmooth);
        wowA_.setRate(0.5f);
        wowB_.prepare(sampleRate, 12);
        wowB_.setShape(LfoShape::Sine);
        wowB_.setRate(0.23f);
        flutterA_.prepare(sampleRate, 13);
        flutterA_.setShape(LfoShape::Sine);
        flutterA_.setRate(6.3f);
        flutterB_.prepare(sampleRate, 14);
        flutterB_.setShape(LfoShape::RandomSmooth);
        flutterB_.setRate(11.0f);
        setParams(params_);
        time_ = targetTime_;
        snapNextParams_ = true;
    }

    void setParams(const Params& p) {
        params_ = p;
        // In SYNC both channels stay on the grid: no left/right offset.
        spread_ = p.sync ? 0.0f : p.spread;
        const float ms = p.sync ? divisionBeats(p.division) * 60000.0f / std::max(p.bpm, 20.0f) : p.timeMs;
        // Tempo or division changes glide like a tape speed change, as in free mode.
        targetTime_ = std::clamp(ms * 0.001f * sampleRate_, 4.0f, kMaxSeconds * sampleRate_ / (1.0f + spread_) - 64.0f);
        // The first settings after prepare() apply at once: a glide there would bend the
        // pitch of the first echoes.
        if (snapNextParams_) {
            time_ = targetTime_;
            snapNextParams_ = false;
        }
        for (int c = 0; c < 2; ++c) {
            lowCut_[c].setCutoff(p.lowCutHz, sampleRate_);
            highCut_[c].set(p.highCutHz, 0.6f, sampleRate_);
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float glide = smoothingCoef(0.25f, sampleRate_);
        const float drive = std::max(params_.drive, 0.1f);

        for (int n = 0; n < numSamples; ++n) {
            // Modulation runs at control rate: every 16 samples.
            if ((controlCounter_++ & 15) == 0) {
                const float wow = 0.7f * wowA_.advance(16) + 0.3f * wowB_.advance(16);
                const float flutter = 0.6f * flutterA_.advance(16) + 0.4f * flutterB_.advance(16);
                speed_ = 1.0f + params_.wow * 0.006f * wow + params_.flutter * 0.0012f * flutter;
            }
            time_ += (targetTime_ - time_) * glide;

            const float d = time_ * speed_;
            const float y[2] = {lines_[0].read(d), lines_[1].read(d * (1.0f + spread_))};

            float fb[2];
            for (int c = 0; c < 2; ++c) {
                float v = saturators_[c].process(y[c] * drive) / drive;
                v = lowCut_[c].highpass(v);
                v = highCut_[c].process(v).low;
                fb[c] = v * params_.feedback;
            }

            if (params_.pingPong) {
                lines_[0].write(0.5f * (inL[n] + inR[n]) + fb[1]);
                lines_[1].write(fb[0]);
            } else {
                lines_[0].write(inL[n] + fb[0]);
                lines_[1].write(inR[n] + fb[1]);
            }
            outL[n] = y[0];
            outR[n] = y[1];
        }
    }

private:
    static constexpr float kMaxSeconds = 2.5f;

    float sampleRate_ = 48000.0f;
    Params params_;
    DelayLine lines_[2];
    TanhAdaa saturators_[2];
    OnePole lowCut_[2];
    Svf highCut_[2];
    Lfo wowA_, wowB_, flutterA_, flutterB_;
    float time_ = 0.0f, targetTime_ = 0.0f, speed_ = 1.0f, spread_ = 0.0f;
    uint32_t controlCounter_ = 0;
    bool snapNextParams_ = false;
};

}  // namespace stasis
