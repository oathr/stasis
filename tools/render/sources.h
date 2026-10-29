#pragma once

#include <string>

#include <stasis/common.h>

#include "wav.h"

// What a host would send alongside the audio.
struct NoteEvent {
    float time;
    int note;
    bool on;
};

struct Source {
    std::string name;
    Stereo audio;
    std::vector<NoteEvent> midi;
    uint16_t scale = 0;  // pitch-class mask, bit 0 = C
};

// Synthetic test material, so renders are comparable without any input file.
namespace sources {

using namespace stasis;

inline float midiToHz(float note) { return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f); }

inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline void normalizePeak(Stereo& s, float peak) {
    float m = 1.0e-9f;
    for (size_t i = 0; i < s.size(); ++i) m = std::max({m, std::abs(s.left[i]), std::abs(s.right[i])});
    const float g = peak / m;
    for (size_t i = 0; i < s.size(); ++i) {
        s.left[i] *= g;
        s.right[i] *= g;
    }
}

// Two slow chords of detuned polyBLEP saws through a breathing lowpass, then silence.
inline Source pad(float fs, float totalSeconds = 30.0f) {
    struct Chord {
        std::vector<int> notes;
        float start, end;
    };
    const Chord chords[] = {{{45, 52, 55, 59, 60, 64}, 0.0f, 6.0f}, {{41, 48, 52, 57, 60, 67}, 5.5f, 11.0f}};
    const float attack = 1.8f, release = 2.5f;

    Stereo out;
    out.sampleRate = fs;
    out.resize(size_t(totalSeconds * fs));
    Rng rng(7);
    for (const auto& chord : chords) {
        const float gain = 1.0f / std::sqrt(float(chord.notes.size() * 3));
        for (const int note : chord.notes) {
            for (const float cents : {-7.0f, 0.0f, 7.0f}) {
                const float dt = midiToHz(float(note) + cents / 100.0f) / fs;
                const float pan = 0.5f + 0.4f * rng.bipolar();
                float phase = rng.uniform();
                const size_t from = size_t(chord.start * fs), to = std::min(out.size(), size_t((chord.end + release) * fs));
                for (size_t i = from; i < to; ++i) {
                    const float t = float(i) / fs - chord.start;
                    float env = std::min(1.0f, t / attack);
                    if (float(i) / fs > chord.end) env *= std::exp(-(float(i) / fs - chord.end) * 2.5f);
                    const float saw = 2.0f * phase - 1.0f - polyBlep(phase, dt);
                    phase += dt;
                    if (phase >= 1.0f) phase -= 1.0f;
                    const float v = saw * env * gain;
                    out.left[i] += v * std::sqrt(1.0f - pan);
                    out.right[i] += v * std::sqrt(pan);
                }
            }
        }
    }
    Svf filterL, filterR;
    for (size_t i = 0; i < out.size(); ++i) {
        if ((i & 31) == 0) {
            const float t = float(i) / fs;
            const float cutoff = 900.0f + 900.0f * (0.5f - 0.5f * std::cos(kTwoPi * t / 10.0f));
            filterL.set(cutoff, 0.8f, fs);
            filterR.set(cutoff, 0.8f, fs);
        }
        out.left[i] = filterL.process(out.left[i]).low;
        out.right[i] = filterR.process(out.right[i]).low;
    }
    normalizePeak(out, 0.5f);

    Source src{"pad", std::move(out), {}, 0};
    for (const auto& chord : chords) {
        for (const int note : chord.notes) {
            src.midi.push_back({chord.start, note, true});
            src.midi.push_back({chord.end, note, false});
        }
    }
    std::sort(src.midi.begin(), src.midi.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.time < b.time; });
    return src;
}

// Karplus-Strong plucks on A minor pentatonic: reveals density and diffusion.
inline Source plucks(float fs, float totalSeconds = 30.0f) {
    const int scale[] = {57, 60, 62, 64, 67, 69, 72, 74, 76};
    Stereo out;
    out.sampleRate = fs;
    out.resize(size_t(totalSeconds * fs));
    Rng rng(42);
    std::vector<NoteEvent> midi;
    for (int n = 0; n < 16; ++n) {
        const int note = scale[rng.next() % 9];
        const float startTime = 0.2f + 0.4f * float(n);
        midi.push_back({startTime, note, true});
        midi.push_back({startTime + 0.35f, note, false});
        const size_t start = size_t(startTime * fs);
        const int period = std::max(2, int(fs / midiToHz(float(note))));
        std::vector<float> ring(static_cast<size_t>(period));
        float lp = 0.0f;
        for (auto& v : ring) v = lp += 0.5f * (rng.bipolar() - lp);
        const float pan = 0.5f + 0.35f * rng.bipolar();
        const size_t length = std::min(size_t(3.0f * fs), out.size() - start);
        size_t idx = 0;
        for (size_t i = 0; i < length; ++i) {
            const size_t next = (idx + 1) % size_t(period);
            const float v = ring[idx];
            ring[idx] = 0.5f * (v + ring[next]) * 0.996f;
            idx = next;
            out.left[start + i] += v * std::sqrt(1.0f - pan);
            out.right[start + i] += v * std::sqrt(pan);
        }
    }
    normalizePeak(out, 0.5f);
    // A minor pentatonic: A C D E G.
    const uint16_t pentatonic = (1u << 9) | (1u << 0) | (1u << 2) | (1u << 4) | (1u << 7);
    return {"plucks", std::move(out), std::move(midi), pentatonic};
}

}  // namespace sources
