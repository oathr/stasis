#pragma once

#include <array>
#include <atomic>

#include "common.h"

namespace stasis {

// GRAIN: turns a continuous sound into separate notes. The input is recorded into a
// short ring buffer; a scheduler fires grains at random, and each grain replays a
// recent slice under an envelope, transposed to one of the allowed intervals.
//
//  ALGO     PULSE    a steady clock (DENSITY per second), CHANCE skips some ticks
//           SCATTER  random gaps (Poisson): same average rate, no pulse
//           DRIFT    the rate wanders slowly (×0.25 .. ×4): clusters, then sparse air
//  SHAPE    the envelope: 0 square (2 % ramps, no clicks) → .5 sine (Hann) → 1 triangle
//  SKEW     -1 percussive (instant attack, long curved release) → 0 even → +1 a swell cut
//           short. Both drawn once into a table when they change: no cost per sample.
//  STEREO   RANDOM (anywhere within WIDTH), L / R (grains alternate sides), PITCH (low grains
//           left, high right), ORBIT (the position circles slowly; each grain takes it)
//  SYNC     the rate follows the host tempo (a note division). PULSE then fires on
//           the host's grid (phase-locked to the song position while it plays);
//           SCATTER and DRIFT keep their randomness around that average rate.
//  INTERVALS  bit i = kIntervals[i] semitones from the source is allowed: relative to
//             whatever is playing, so no note detection is needed
//
// Each grain: a random slice from the last ~1.5 s, a place in the stereo field, a level
// scaled by the expected overlap so the cloud stays at about the input level.
class GrainCloud {
public:
    enum class Algo { Pulse, Scatter, Drift };
    enum class Stereo { Random, Alternate, Pitch, Orbit };
    static constexpr std::array<int, 15> kIntervals = {-12, -9, -7, -5, -4, -3, -2, 0, 2, 3, 4, 5, 7, 9, 12};

    struct Params {
        Algo algo = Algo::Scatter;
        float shape = 0.5f;       // 0 square, .5 sine, 1 triangle
        float skew = 0.0f;        // -1 percussive .. +1 swell
        float density = 4.0f;     // grains per second
        float chance = 0.7f;      // 0..1
        float sizeMs = 200.0f;
        uint16_t intervals = 0x5080;  // unison, fifth, octave (bits of kIntervals)
        Stereo stereo = Stereo::Random;
        float spread = 0.8f;      // WIDTH 0..1
        bool sync = false;
        float divisionBeats = 0.5f;  // SYNC: one grain per this many quarter notes (average)
        float reverse = 0.0f;        // chance that a grain plays its slice backwards: 0 all forward, 0.5 half, 1 all
    };

    // Host tempo and song position (quarter notes) at the start of the next block.
    void setTransport(float bpm, double ppq, bool playing) {
        bpm_ = std::max(bpm, 20.0f);
        if (playing) beat_ = ppq;
    }

    // What the UI draws: one entry per grain that played.
    struct Event {
        float semitones, seconds, pan;
        float shape, skew;  // its envelope
        float level;  // RMS of the slice it plays, times its gain
    };
    static constexpr int kEvents = 128;

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        size_t length = 1;
        while (length < size_t(4.0f * sampleRate)) length <<= 1;
        for (auto& r : ring_) r.assign(length, 0.0f);
        mask_ = length - 1;
        written_ = 0;
        for (auto& v : voices_) v.active = false;
        countdown_ = 0.0;
        drift_ = 0.0f;
        orbit_ = 0.0f;
        drawEnvelope();
    }

    void setParams(const Params& p) {
        const bool redraw = p.shape != params_.shape || p.skew != params_.skew;
        params_ = p;
        if (redraw) drawEnvelope();
    }

    // The envelope at p (0..1) for a shape and skew: the table is drawn from this, and the
    // interface draws the same curve.
    static float envelopeAt(float p, float shape, float skew) {
        shape = std::clamp(shape, 0.0f, 1.0f);
        skew = std::clamp(skew, -1.0f, 1.0f);
        const float peak = 0.5f + 0.47f * skew;
        const float q = p < peak ? 0.5f * p / peak : 0.5f + 0.5f * (p - peak) / (1.0f - peak);  // the peak moved to the middle
        const float taper = 0.04f + 0.96f * std::min(shape, 0.5f) / 0.5f;  // Tukey: 4 % ramps (square) .. all of it (Hann)
        float e = q < 0.5f * taper ? 0.5f - 0.5f * std::cos(kTwoPi * q / taper)
                : q > 1.0f - 0.5f * taper ? 0.5f - 0.5f * std::cos(kTwoPi * (1.0f - q) / taper) : 1.0f;
        if (shape > 0.5f) e += (shape - 0.5f) / 0.5f * ((1.0f - std::abs(2.0f * q - 1.0f)) - e);  // Hann → triangle
        // The long side curves: a percussive tail falls fast then lingers, a swell grows slowly.
        if (p >= peak && skew < 0.0f) e = std::pow(e, 1.0f + 2.2f * -skew);
        if (p < peak && skew > 0.0f) e = std::pow(e, 1.0f + 2.2f * skew);
        return e;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float division = std::max(params_.divisionBeats, 1.0f / 16.0f);
        const float density = std::clamp(params_.sync ? bpm_ / 60.0f / division : params_.density, 0.05f, 60.0f);
        const bool grid = params_.sync && params_.algo == Algo::Pulse;
        const double beatsPerSample = double(bpm_) / 60.0 / sampleRate_;
        const float size = std::clamp(params_.sizeMs, 5.0f, 4000.0f) * 0.001f;
        // Level: overlapping grains add up in power.
        const float level = 1.0f / std::sqrt(std::max(1.0f, density * size * 0.5f));
        for (int n = 0; n < numSamples; ++n) {
            ring_[0][written_ & mask_] = inL[n];
            ring_[1][written_ & mask_] = inR[n];
            ++written_;

            if (grid) {  // on the host's grid
                beat_ += beatsPerSample;
                // A grain that swells is loudest at its peak: it starts that much early so the
                // peak lands on the grid (started on it, a 160 ms sine sounded 80 ms late). A
                // square or percussive grain is loud from its start: no lead.
                const double lead = double(swell_) * double(size) * double(bpm_) / 60.0;
                const int64_t step = int64_t(std::floor((beat_ + lead) / division));
                if (step != lastStep_) {
                    lastStep_ = step;
                    if (rng_.uniform() < params_.chance) spawn(size, level);
                }
            } else if (--countdown_ <= 0.0) {
                countdown_ += nextGap(density);
                if (rng_.uniform() < params_.chance) spawn(size, level);
            }

            float l = 0.0f, r = 0.0f;
            for (auto& v : voices_) {
                if (!v.active) continue;
                const float x = float(v.age) / float(v.length) * float(kEnvTable - 1);
                const int i = std::min(int(x), kEnvTable - 2);
                const float env = (envTable_[size_t(i)] + (x - float(i)) * (envTable_[size_t(i + 1)] - envTable_[size_t(i)])) * v.gain;
                const float a = read(0, v.position), b = read(1, v.position);
                l += env * v.panL * a;
                r += env * v.panR * b;
                v.position += v.ratio;
                if (++v.age >= v.length) v.active = false;
            }
            outL[n] = l;
            outR[n] = r;
        }
        // ORBIT: one turn every ~7 s.
        orbit_ = std::fmod(orbit_ + kTwoPi * 0.15f * float(numSamples) / sampleRate_, kTwoPi);
    }

    // UI side: events written since `cursor` (which is advanced). Lock-free, single
    // writer (the audio thread); a reader that falls 128 events behind skips ahead.
    int readEvents(Event* out, int max, uint32_t& cursor) const {
        const uint32_t head = eventHead_.load(std::memory_order_acquire);
        if (head - cursor > uint32_t(kEvents)) cursor = head - uint32_t(kEvents);
        int count = 0;
        while (cursor != head && count < max) out[count++] = events_[cursor++ % kEvents];
        return count;
    }

private:
    struct Voice {
        bool active = false;
        double position = 0.0;
        float ratio = 1.0f, gain = 1.0f, panL = 1.0f, panR = 1.0f;  // ratio < 0: backwards
        int length = 1, age = 0;
    };

    static constexpr int kEnvTable = 512;
    void drawEnvelope() {
        for (int i = 0; i < kEnvTable; ++i) envTable_[size_t(i)] = envelopeAt(float(i) / float(kEnvTable - 1), params_.shape, params_.skew);
        // How far into the grain its weight is centred (for the grid lead): 0 for a square or
        // a percussive grain, the peak for one that swells.
        swell_ = std::min(params_.shape, 0.5f) / 0.5f * (0.5f + 0.47f * std::clamp(params_.skew, -1.0f, 1.0f));
    }

    double nextGap(float density) {
        const double mean = sampleRate_ / density;
        switch (params_.algo) {
            case Algo::Pulse: return mean;
            case Algo::Scatter: return -std::log(std::max(1.0e-6, double(rng_.uniform()))) * mean;
            case Algo::Drift: {
                // A slow random walk of the rate in octaves (±2), then Poisson gaps.
                drift_ = std::clamp(drift_ + 0.35f * rng_.bipolar(), -2.0f, 2.0f);
                return -std::log(std::max(1.0e-6, double(rng_.uniform()))) * mean / std::exp2(double(drift_));
            }
        }
        return mean;
    }

    void spawn(float size, float level) {
        // Allowed transpositions: the chosen intervals (none chosen = unison).
        std::array<int, kIntervals.size()> choices{};
        int count = 0;
        for (size_t i = 0; i < kIntervals.size(); ++i)
            if (params_.intervals & (1u << i)) choices[size_t(count++)] = kIntervals[i];
        const int semitones = count > 0 ? choices[size_t(rng_.next() % uint32_t(count))] : 0;

        Voice* voice = nullptr;
        for (auto& v : voices_)
            if (!v.active) {
                voice = &v;
                break;
            }
        if (voice == nullptr) return;  // all voices busy: skip this grain

        const float ratio = semitonesToRatio(float(semitones));
        // A long grain transposed far up would read more than the ring holds: shorten it.
        const int length = std::max(16, std::min(int(size * sampleRate_), int(0.85 * double(mask_) / ratio)));
        // The slice must be fully recorded: it reads length·ratio samples.
        const double needed = double(length) * ratio + 4.0;
        const double reach = std::min(1.5 * sampleRate_, double(mask_) - needed - 8.0);
        if (double(written_) < needed + 8.0) return;
        const double back = needed + rng_.uniform() * std::max(0.0, std::min(reach, double(written_) - needed - 8.0));

        // How loud this grain is: the RMS of its slice (16 points), times its gain.
        const double start = double(written_) - back;
        double energy = 0.0;
        for (int i = 0; i < 16; ++i) {
            const double at = start + (double(length) * ratio) * (i + 0.5) / 16.0;
            const float a = read(0, at), b = read(1, at);
            energy += 0.5 * (double(a) * a + double(b) * b);
        }
        const float sliceLevel = float(std::sqrt(energy / 16.0)) * level;

        const float width = std::clamp(params_.spread, 0.0f, 1.0f);
        float pan = width * rng_.bipolar();
        switch (params_.stereo) {
            case Stereo::Random: break;
            case Stereo::Alternate: side_ = -side_; pan = side_ * width * (0.75f + 0.25f * rng_.uniform()); break;
            case Stereo::Pitch: pan = std::clamp(float(semitones) / 12.0f + 0.05f * rng_.bipolar(), -1.0f, 1.0f) * width; break;
            case Stereo::Orbit: pan = std::sin(orbit_) * width; break;
        }
        const float angle = (pan + 1.0f) * 0.25f * kPi;
        // REVERSE: the same slice, read from its end back to its start.
        const bool backwards = rng_.uniform() < params_.reverse;
        *voice = {true, backwards ? start + double(length) * ratio : start, backwards ? -ratio : ratio, level, std::cos(angle) * 1.41421356f, std::sin(angle) * 1.41421356f, length, 0};

        const uint32_t head = eventHead_.load(std::memory_order_relaxed);
        events_[head % kEvents] = {float(semitones), float(length) / sampleRate_, pan, params_.shape, params_.skew, sliceLevel};
        eventHead_.store(head + 1, std::memory_order_release);
    }

    float read(int c, double position) const {
        const double floorPos = std::floor(position);
        const size_t i = size_t(int64_t(floorPos)) & mask_;
        const float frac = float(position - floorPos);
        const auto& ring = ring_[size_t(c)];
        return ring[i] + frac * (ring[(i + 1) & mask_] - ring[i]);
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    std::array<std::vector<float>, 2> ring_;
    size_t mask_ = 0;
    uint64_t written_ = 0;
    std::array<Voice, 32> voices_{};
    double countdown_ = 0.0, beat_ = 0.0;
    int64_t lastStep_ = -1;
    float drift_ = 0.0f, bpm_ = 120.0f, orbit_ = 0.0f, side_ = 1.0f, swell_ = 0.5f;
    std::array<float, kEnvTable> envTable_{};
    Rng rng_{0x6A41};
    std::array<Event, kEvents> events_{};
    std::atomic<uint32_t> eventHead_{0};
};

}  // namespace stasis
