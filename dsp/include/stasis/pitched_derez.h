#pragma once

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
                    v = std::round(v * scale) / scale;
                    const float h = v - ch.held;
                    ch.held = v;
                    current = v;
                    // 2-point polyBLEP: smooth the step across this and the previous sample.
                    correction = -h * (1.0f - d) * (1.0f - d) * 0.5f;
                    ch.pending += h * d * d * 0.5f;
                }
                ch.lastInput = shifted[c];
                const float y = ch.pending;  // previous sample, now final
                ch.pending = current + correction;
                const float dry = ch.dryDelay;
                ch.dryDelay = in[c][n];
                out[c][n] = dry + mix * (y - dry);
            }
        }
    }

private:
    struct Channel {
        float held = 0.0f, lastInput = 0.0f, pending = 0.0f, dryDelay = 0.0f;
    };

    void updateRate() {
        if (params_.free) {
            rate_ = std::clamp(params_.freeRateHz, 50.0f, sampleRate_ * 0.5f);
            shifterL_.setWindowSamples(0.06f * sampleRate_);
            shifter_.setWindowSamples(0.06f * sampleRate_);
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
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    NoteSet notes_;
    GranularPitchShifter shifterL_, shifter_;
    Channel channels_[2];
    float rootHz_ = 110.0f, rate_ = 3520.0f, phase_ = 0.0f;
};

}  // namespace stasis
