#pragma once

#include "color.h"
#include "common.h"
#include "note_set.h"
#include "pitch_shifter.h"

namespace stasis {

// DEREZ, tuned. Classic sample-rate reduction (Airwindows DeRez and friends) holds
// the signal at a rate R: every partial f folds to |k*R ± f|, which has nothing to
// do with the note played, hence the "chaotic" pitch.
//
// Here R is locked to the note: R = n * g, where g is the target fundamental (the
// root note shifted by INTERVAL). A partial m*g then folds to (k*n ± m) * g: every
// alias lands on a harmonic of g. The crunch stays, the tuning stays.
//
//  INTERVAL  semitones applied before the crush (clean pitch shift)
//  CRUSH     0..1 → n from 32 (gentle) down to 2 (brutal)
//  BITS      16 .. 2, continuous
//  SMOOTH    0..1: 0 = the raw staircase and hard bit steps; towards 1 the held values
//            are joined by slopes, the bit steps become soft (no jumps, so no crackle on
//            quiet sounds) and a 12 dB/oct low-pass follows the rate down to 0.6 R,
//            which removes the images (pure grit) and keeps the tuned aliases
//  ROOT      lowest held MIDI note, else setRoot() (AUTO detector or a fixed note)
//  FREE      unlocked mode: R in Hz, the classic chaotic DeRez
//
// The hold steps are band-limited (polyBLEP), so the only aliasing is the tuned one
// the effect intends, not extra fold-over at the host rate. Latency: 1 sample.
class PitchedDerez {
public:
    struct Params {
        float interval = 0.0f;  // semitones
        float crush = 0.5f;     // 0..1
        float bits = 16.0f;     // 2..16
        float smooth = 0.7f;    // 0..1
        float mix = 1.0f;
        bool free = false;
        float freeRateHz = 4000.0f;
        bool latch = true;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        shifterL_.prepare(sampleRate, 60.0f);
        shifter_.prepare(sampleRate, 60.0f);
        for (auto& ch : channels_) ch = {};
        setParams(params_);
    }

    int latencySamples() const { return 1; }

    void setParams(const Params& p) {
        params_ = p;
        notes_.setLatch(p.latch);
        shifterL_.setSemitones(p.interval);
        shifter_.setSemitones(p.interval);
        updateRate();
    }

    void setRoot(float hz) {
        rootHz_ = hz;
        updateRate();
    }
    void noteOn(int note) {
        notes_.noteOn(note);
        updateRate();
    }
    void noteOff(int note) {
        notes_.noteOff(note);
        updateRate();
    }

    float holdRateHz() const { return rate_; }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float inc = rate_ / sampleRate_;
        const float bits = std::clamp(params_.bits, 2.0f, 16.0f);
        const float scale = std::pow(2.0f, bits - 1.0f);
        const float smooth = std::clamp(params_.smooth, 0.0f, 1.0f);
        // Soft bit steps: the rounding ramps over this share of a step (0 = a hard step).
        const float ramp = 0.85f * smooth;
        const float mix = std::clamp(params_.mix, 0.0f, 1.0f);
        const float* in[2] = {inL, inR};
        float* out[2] = {outL, outR};

        for (int n = 0; n < numSamples; ++n) {
            const bool shift = params_.interval != 0.0f;
            const float shifted[2] = {shift ? shifterL_.process(in[0][n]) : in[0][n],
                                      shift ? shifter_.process(in[1][n]) : in[1][n]};
            phase_ += inc;
            const bool step = phase_ >= 1.0f;
            float d = 0.0f;  // how far past the hold instant this sample is, in samples
            if (step) {
                phase_ -= std::floor(phase_);
                d = std::min(phase_ / inc, 1.0f);
            }
            for (int c = 0; c < 2; ++c) {
                Channel& ch = channels_[c];
                float current = ch.held;
                float correction = 0.0f;
                if (step) {
                    // Sample the input at the exact hold instant (linear between samples).
                    float v = ch.lastInput + (shifted[c] - ch.lastInput) * (1.0f - d);
                    v = quantize(v * scale, ramp) / scale;
                    const float h = (v - ch.held) * (1.0f - smooth);  // what is left of the step
                    ch.previous = ch.held;
                    ch.held = v;
                    // 2-point polyBLEP: smooth the step across this and the previous sample.
                    correction = -h * (1.0f - d) * (1.0f - d) * 0.5f;
                    ch.pending += h * d * d * 0.5f;
                }
                // SMOOTH: from the held value towards a slope between the last two holds.
                const float slope = ch.previous + (ch.held - ch.previous) * std::min(phase_, 1.0f);
                current = ch.held + smooth * (slope - ch.held);
                ch.lastInput = shifted[c];
                float y = ch.pending;  // previous sample, now final
                if (smooth > 0.0f) y = ch.lowpass.process(y);
                ch.pending = current + correction;
                const float dry = ch.dryDelay;
                ch.dryDelay = in[c][n];
                out[c][n] = dry + mix * (y - dry);
            }
        }
    }

private:
    struct Channel {
        float held = 0.0f, previous = 0.0f, lastInput = 0.0f, pending = 0.0f, dryDelay = 0.0f;
        SvfEq lowpass;
    };

    // Rounding to the nearest step; ramp (0..1 of a step) turns the jump into a smooth
    // S-curve centred between two steps: continuous, so quiet sounds hiss instead of crackle.
    static float quantize(float x, float ramp) {
        const float k = std::floor(x), f = x - k;
        if (ramp <= 0.0f) return f < 0.5f ? k : k + 1.0f;
        const float t = std::clamp((f - 0.5f) / ramp + 0.5f, 0.0f, 1.0f);
        return k + t * t * (3.0f - 2.0f * t);
    }

    void updateFilter() {
        // Off (20 kHz) at SMOOTH 0, down to 0.6 R at 1, on a log scale.
        const float smooth = std::clamp(params_.smooth, 0.0f, 1.0f);
        const float top = 0.45f * sampleRate_, low = std::clamp(0.6f * rate_, 200.0f, top);
        const float hz = top * std::pow(low / top, smooth);
        for (auto& ch : channels_) ch.lowpass.set(SvfEq::Type::Lowpass, hz, 0.707f, 0.0f, sampleRate_);
    }

    void updateRate() {
        if (params_.free) {
            rate_ = std::clamp(params_.freeRateHz, 50.0f, sampleRate_ * 0.5f);
            shifterL_.setWindowSamples(0.06f * sampleRate_);
            shifter_.setWindowSamples(0.06f * sampleRate_);
            updateFilter();
            return;
        }
        float root = rootHz_;
        for (int note = 0; note < 128; ++note) {
            if (notes_.active().test(size_t(note))) {
                root = 440.0f * std::pow(2.0f, float(note - 69) / 12.0f);
                break;  // lowest note
            }
        }
        // Pitch-synchronous grains: half a window = whole periods of the root (~30 ms).
        const float period = sampleRate_ / root;
        const float halfWindow = period * std::max(1.0f, std::round(0.03f * sampleRate_ / period));
        shifterL_.setWindowSamples(2.0f * halfWindow);
        shifter_.setWindowSamples(2.0f * halfWindow);

        const float target = root * semitonesToRatio(params_.interval);
        // Any integer multiple keeps the aliases harmonic; cap it below the host Nyquist.
        float multiple = std::round(32.0f * std::pow(2.0f / 32.0f, std::clamp(params_.crush, 0.0f, 1.0f)));
        multiple = std::clamp(multiple, 1.0f, std::max(1.0f, std::floor(0.5f * sampleRate_ / target)));
        rate_ = target * multiple;
        updateFilter();
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    NoteSet notes_;
    GranularPitchShifter shifterL_, shifter_;
    Channel channels_[2];
    float rootHz_ = 110.0f, rate_ = 3520.0f, phase_ = 0.0f;
};

}  // namespace stasis
