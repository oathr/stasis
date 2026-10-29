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
//  ENV      SINE, TRIANGLE, SQUARE (2 % ramps, no clicks), PERC (fast attack, decay)
//  SYNC     the rate follows the host tempo (a note division). PULSE then fires on
//           the host's grid (phase-locked to the song position while it plays);
//           SCATTER and DRIFT keep their randomness around that average rate.
//  NOTES    bit n = n semitones above the source is allowed (0 = unison, 7 = fifth)
//  OCTAVES  0, 1 or 2: the allowed set is repeated 1, 2 or 3 octaves up. It widens the
//           range; it does not shift the notes already allowed.
//
// Each grain: a random slice from the last ~1.5 s, a random pan, a level scaled by the
// expected overlap so the cloud stays at about the input level.
class GrainCloud {
public:
    enum class Algo { Pulse, Scatter, Drift };
    enum class Envelope { Sine, Triangle, Square, Perc };

    struct Params {
        Algo algo = Algo::Scatter;
        Envelope envelope = Envelope::Sine;
        float density = 4.0f;     // grains per second
        float chance = 0.7f;      // 0..1
        float sizeMs = 200.0f;
        uint16_t notes = 0x81;    // unison and fifth
        int octaves = 1;
        float spread = 0.8f;      // pan width 0..1
        bool sync = false;
        float divisionBeats = 0.5f;  // SYNC: one grain per this many quarter notes (average)
    };

    // Host tempo and song position (quarter notes) at the start of the next block.
    void setTransport(float bpm, double ppq, bool playing) {
        bpm_ = std::max(bpm, 20.0f);
        if (playing) beat_ = ppq;
    }

    // What the UI draws: one entry per grain that played.
    struct Event {
        float semitones, seconds, pan;
        int envelope;
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
    }

    void setParams(const Params& p) { params_ = p; }

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
                const int64_t step = int64_t(std::floor(beat_ / division));
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
                const float phase = float(v.age) / float(v.length);
                const float env = envelope(v.envelope, phase) * v.gain;
                const float a = read(0, v.position), b = read(1, v.position);
                l += env * v.panL * a;
                r += env * v.panR * b;
                v.position += v.ratio;
                if (++v.age >= v.length) v.active = false;
            }
            outL[n] = l;
            outR[n] = r;
        }
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
        float ratio = 1.0f, gain = 1.0f, panL = 1.0f, panR = 1.0f;
        int length = 1, age = 0;
        Envelope envelope = Envelope::Sine;
    };

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
        // Allowed transpositions: the chosen intervals, repeated over the octaves.
        std::array<int, 36> choices{};
        int count = 0;
        const uint16_t notes = params_.notes & 0x0FFF ? params_.notes : uint16_t(1);
        for (int octave = 0; octave <= std::clamp(params_.octaves, 0, 2); ++octave)
            for (int st = 0; st < 12; ++st)
                if (notes & (1u << st)) choices[size_t(count++)] = st + 12 * octave;
        const int semitones = choices[size_t(rng_.next() % uint32_t(count))];

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

        const float pan = std::clamp(params_.spread, 0.0f, 1.0f) * rng_.bipolar();
        const float angle = (pan + 1.0f) * 0.25f * kPi;
        *voice = {true, double(written_) - back, ratio, level, std::cos(angle) * 1.41421356f, std::sin(angle) * 1.41421356f, length, 0,
                  params_.envelope};

        const uint32_t head = eventHead_.load(std::memory_order_relaxed);
        events_[head % kEvents] = {float(semitones), float(length) / sampleRate_, pan, int(params_.envelope)};
        eventHead_.store(head + 1, std::memory_order_release);
    }

    float read(int c, double position) const {
        const double floorPos = std::floor(position);
        const size_t i = size_t(int64_t(floorPos)) & mask_;
        const float frac = float(position - floorPos);
        const auto& ring = ring_[size_t(c)];
        return ring[i] + frac * (ring[(i + 1) & mask_] - ring[i]);
    }

    static float envelope(Envelope e, float p) {
        switch (e) {
            case Envelope::Sine: return std::sin(kPi * p);
            case Envelope::Triangle: return 1.0f - std::abs(2.0f * p - 1.0f);
            case Envelope::Square: return std::min({1.0f, p / 0.02f, (1.0f - p) / 0.02f});
            case Envelope::Perc: {
                const float attack = std::min(1.0f, p / 0.03f), decay = 1.0f - p;
                return attack * decay * decay * decay;
            }
        }
        return 0.0f;
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    std::array<std::vector<float>, 2> ring_;
    size_t mask_ = 0;
    uint64_t written_ = 0;
    std::array<Voice, 32> voices_{};
    double countdown_ = 0.0, beat_ = 0.0;
    int64_t lastStep_ = -1;
    float drift_ = 0.0f, bpm_ = 120.0f;
    Rng rng_{0x6A41};
    std::array<Event, kEvents> events_{};
    std::atomic<uint32_t> eventHead_{0};
};

}  // namespace stasis
