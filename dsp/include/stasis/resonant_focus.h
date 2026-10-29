#pragma once

#include <array>

#include "common.h"
#include "note_set.h"

namespace stasis {

// FOCUS, zero-latency engine. Same idea as HarmonicFocus, built from resonators
// instead of an FFT, so nothing waits for a frame: declared latency is 0.
//
//  KEY       one constant-Q bandpass (TPT SVF) per semitone from C1 to ~5 kHz;
//            only the notes whose pitch class is allowed are summed.
//  HARMONIC  one feedback comb per held note (delay = 1 / f0): peaks on every
//            harmonic n * f0, i.e. a tuned resonator that keeps the note's timbre.
//
// Above kAirHz the signal passes untouched (air, breath, shimmer sparkle): pitch
// filtering there only dulls the sound.
//
// The physics still apply: a resonator narrow enough to separate semitones in the
// bass takes time to build up and to die out (that is its frequency resolution).
// It just does it causally, with no lookahead, and it rings softly instead of
// delaying everything. Gains glide on chord changes, so nothing clicks.
class ResonantFocus {
public:
    enum class Mode { Key, Harmonic };

    struct Params {
        Mode mode = Mode::Key;
        float focus = 0.5f;   // 0 = gentle .. 1 = laser (narrower, longer ringing)
        float depth = 0.85f;  // how much out-of-key content is removed
        float mix = 1.0f;
        bool latch = true;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        numNotes_ = 0;
        for (int note = kLowNote; note < 128 && noteHz(note) < kAirHz && numNotes_ < kMaxNotes; ++note) ++numNotes_;
        for (auto& g : keyGain_) g = 0.0f;
        for (auto& v : voices_) {
            for (auto& line : v.lines) line.prepare(int(sampleRate / noteHz(kLowNote)) + 8);
            v.gain = v.target = 0.0f;
            v.note = -1;
        }
        for (int c = 0; c < 2; ++c) air_[c].set(kAirHz, 0.6f, sampleRate);
        gainStep_ = 1.0f / (0.15f * sampleRate / float(kControlInterval));
        envCoef_ = smoothingCoef(0.3f, sampleRate);
        dirty_ = true;
        setParams(params_);
        configureBank();
    }

    int latencySamples() const { return 0; }

    void setParams(const Params& p) {
        const bool retune = p.focus != params_.focus;
        dirty_ = dirty_ || p.mode != params_.mode || p.latch != params_.latch;
        params_ = p;
        notes_.setLatch(p.latch);
        if (retune && numNotes_ > 0) configureBank();
    }

    // Fallback when no note is held (fixed scale, or the AUTO key detector).
    void setScale(uint16_t pitchClassMask) {
        dirty_ = dirty_ || pitchClassMask != scale_;
        scale_ = pitchClassMask;
    }
    void noteOn(int note) {
        notes_.noteOn(note);
        dirty_ = true;
    }
    void noteOff(int note) {
        notes_.noteOff(note);
        dirty_ = true;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float* in[2] = {inL, inR};
        float* out[2] = {outL, outR};
        const float depth = std::clamp(params_.depth, 0.0f, 1.0f);
        const float mix = std::clamp(params_.mix, 0.0f, 1.0f);

        for (int n = 0; n < numSamples; ++n) {
            if ((counter_++ % kControlInterval) == 0) updateGains();

            float wet[2] = {0.0f, 0.0f};
            if (bypass_) {
                wet[0] = in[0][n];
                wet[1] = in[1][n];
            } else if (effectiveMode_ == Mode::Key) {
                for (int i = 0; i < numNotes_; ++i) {
                    const float g = keyGain_[size_t(i)];
                    if (g <= 0.0f) continue;
                    for (int c = 0; c < 2; ++c) wet[c] += g * bandK_[size_t(i)] * bank_[c][size_t(i)].process(in[c][n]).band;
                }
            } else {
                for (auto& v : voices_) {
                    if (v.gain <= 0.0f) continue;
                    for (int c = 0; c < 2; ++c) {
                        const float fb = v.lines[c].read(v.period);
                        const float y = (1.0f - combFeedback_) * in[c][n] + combFeedback_ * fb;
                        v.lines[c].write(y);
                        wet[c] += v.gain * v.dcBlock[c].highpass(y);
                    }
                }
            }

            for (int c = 0; c < 2; ++c) {
                const float x = in[c][n];
                if (!bypass_) wet[c] += air_[c].process(x).high;
                inEnv_ += envCoef_ * (x * x - inEnv_);
                outEnv_ += envCoef_ * (wet[c] * wet[c] - outEnv_);
                const float focused = (1.0f - depth) * x + depth * wet[c] * makeup_;
                out[c][n] = x + mix * (focused - x);
            }
        }
    }

private:
    static constexpr int kLowNote = 24;  // C1, 32.7 Hz
    static constexpr int kMaxNotes = 96;
    static constexpr int kVoices = 16;
    static constexpr int kControlInterval = 32;
    static constexpr float kAirHz = 5000.0f;

    struct Voice {
        DelayLine lines[2];
        OnePole dcBlock[2];
        float period = 100.0f, gain = 0.0f, target = 0.0f;
        int note = -1;
    };

    static float noteHz(int note) { return 440.0f * std::pow(2.0f, float(note - 69) / 12.0f); }

    void configureBank() {
        // Bandwidth in semitones: 1.2 (gentle) .. 0.2 (laser). Q of a band that wide.
        const float bw = 1.2f * std::pow(0.2f / 1.2f, std::clamp(params_.focus, 0.0f, 1.0f));
        const float q = 1.0f / (std::pow(2.0f, bw / 24.0f) - std::pow(2.0f, -bw / 24.0f));
        for (int i = 0; i < numNotes_; ++i) {
            for (int c = 0; c < 2; ++c) bank_[c][size_t(i)].set(noteHz(kLowNote + i), q, sampleRate_);
            bandK_[size_t(i)] = 1.0f / q;  // SVF band output peaks at Q: normalise to unity
        }
        combFeedback_ = 0.85f + 0.13f * std::clamp(params_.focus, 0.0f, 1.0f);
    }

    void updateGains() {
        if (dirty_) {
            applyNotes();
            dirty_ = false;
        }
        for (int i = 0; i < numNotes_; ++i) {
            float& g = keyGain_[size_t(i)];
            const float t = keyTarget_[size_t(i)];
            if (g == t) continue;
            g = g < t ? std::min(t, g + gainStep_) : std::max(t, g - gainStep_);
            if (g <= 0.0f) {  // fully faded: restart clean next time
                bank_[0][size_t(i)].reset();
                bank_[1][size_t(i)].reset();
            }
        }
        for (auto& v : voices_) {
            v.gain = v.gain < v.target ? std::min(v.target, v.gain + gainStep_) : std::max(v.target, v.gain - gainStep_);
            if (v.gain <= 0.0f && v.target <= 0.0f) v.note = -1;
        }
        const float target = std::sqrt((inEnv_ + 1.0e-12f) / (outEnv_ + 1.0e-12f));
        makeup_ += (std::clamp(target, 1.0f, 2.5f) - makeup_) * 0.05f;
    }

    void applyNotes() {
        const uint16_t classes = notes_.empty() ? scale_ : notes_.pitchClasses();
        const bool harmonic = params_.mode == Mode::Harmonic && !notes_.empty();
        bypass_ = classes == 0;
        // Harmonic mode without MIDI notes falls back to the key bank.
        effectiveMode_ = harmonic ? Mode::Harmonic : Mode::Key;

        for (int i = 0; i < numNotes_; ++i) keyTarget_[size_t(i)] = (classes >> ((kLowNote + i) % 12)) & 1u ? 1.0f : 0.0f;

        for (auto& v : voices_) v.target = 0.0f;
        if (!harmonic) return;
        for (int note = 0; note < 128; ++note) {
            if (!notes_.active().test(size_t(note))) continue;
            Voice* voice = nullptr;
            for (auto& v : voices_)
                if (v.note == note) voice = &v;
            for (auto& v : voices_)
                if (!voice && v.note < 0) voice = &v;
            if (!voice) continue;  // more than kVoices notes: ignore the extras
            if (voice->note != note) {
                voice->note = note;
                voice->period = sampleRate_ / noteHz(note);
                for (int c = 0; c < 2; ++c) {
                    voice->lines[c].clear();
                    voice->dcBlock[c].reset();
                    voice->dcBlock[c].setCutoff(0.5f * noteHz(note), sampleRate_);
                }
            }
            voice->target = 1.0f;
        }
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    NoteSet notes_;
    uint16_t scale_ = 0;
    Mode effectiveMode_ = Mode::Key;
    bool dirty_ = true, bypass_ = true;

    int numNotes_ = 0;
    std::array<Svf, kMaxNotes> bank_[2];
    std::array<float, kMaxNotes> bandK_{}, keyGain_{}, keyTarget_{};
    std::array<Voice, kVoices> voices_;
    float combFeedback_ = 0.9f;
    Svf air_[2];

    float gainStep_ = 0.01f, envCoef_ = 0.001f, inEnv_ = 0.0f, outEnv_ = 0.0f, makeup_ = 1.0f;
    uint32_t counter_ = 0;
};

}  // namespace stasis
