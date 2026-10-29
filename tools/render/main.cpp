// Phase 0 sandbox: runs test material through each algorithm and writes WAV files
// to listen to, with the processing cost of each render.
//
//   stasis_render [--input file.wav] [--out renders]   render everything
//   stasis_render --analyse                            objective reverb metrics

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

#include <stasis/capture_stretch.h>
#include <stasis/color.h>
#include <stasis/harmonic_focus.h>
#include <stasis/key_detector.h>
#include <stasis/lfo.h>
#include <stasis/paul_stretch.h>
#include <stasis/pitched_derez.h>
#include <stasis/plate_reverb.h>
#include <stasis/resonant_focus.h>
#include <stasis/space_reverb.h>
#include <stasis/tape_echo.h>
#include <stasis/tonal_noise.h>

#include "sources.h"
#include "wav.h"

using namespace stasis;

namespace {

constexpr int kBlock = 256;

// Runs any module with process(inL, inR, outL, outR, n) block by block, like a host would.
// `perBlock` gets called before each block with its start position, for modulation and MIDI.
template <typename Module>
Stereo runWet(Module& m, const Stereo& in, size_t length, const std::function<void(size_t)>& perBlock = {}) {
    Stereo out;
    out.sampleRate = in.sampleRate;
    out.resize(length);
    std::vector<float> zeros(kBlock, 0.0f);
    for (size_t pos = 0; pos < length; pos += kBlock) {
        const int n = int(std::min<size_t>(kBlock, length - pos));
        if (perBlock) perBlock(pos);
        const bool hasInput = pos + size_t(n) <= in.size();
        m.process(hasInput ? &in.left[pos] : zeros.data(), hasInput ? &in.right[pos] : zeros.data(),
                  &out.left[pos], &out.right[pos], n);
    }
    return out;
}

Stereo mix(const Stereo& dry, float dryGain, const Stereo& wet, float wetGain) {
    Stereo out;
    out.sampleRate = wet.sampleRate;
    out.resize(wet.size());
    for (size_t i = 0; i < wet.size(); ++i) {
        const float dl = i < dry.size() ? dry.left[i] : 0.0f, dr = i < dry.size() ? dry.right[i] : 0.0f;
        out.left[i] = dl * dryGain + wet.left[i] * wetGain;
        out.right[i] = dr * dryGain + wet.right[i] * wetGain;
    }
    return out;
}

struct Stats {
    float peakDb, rmsDb;
    bool finite;
};

Stats analyse(const Stereo& s) {
    double sum = 0.0;
    float peak = 0.0f;
    bool finite = true;
    for (size_t i = 0; i < s.size(); ++i) {
        for (const float v : {s.left[i], s.right[i]}) {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, std::abs(v));
            sum += double(v) * v;
        }
    }
    const float rms = float(std::sqrt(sum / std::max<size_t>(1, 2 * s.size())));
    return {20.0f * std::log10(std::max(peak, 1e-9f)), 20.0f * std::log10(std::max(rms, 1e-9f)), finite};
}

// Same loudness for every render (RMS -20 dBFS), peak capped at -1 dBFS.
void levelMatch(Stereo& s) {
    const Stats st = analyse(s);
    float gain = dbToGain(-20.0f - st.rmsDb);
    gain = std::min(gain, dbToGain(-1.0f - st.peakDb));
    for (size_t i = 0; i < s.size(); ++i) {
        s.left[i] *= gain;
        s.right[i] *= gain;
    }
}

float seconds(const Stereo& s) { return float(s.size()) / s.sampleRate; }

// --- Module helpers -------------------------------------------------------------

SpaceReverb::Params hall() {
    SpaceReverb::Params p;
    p.size = 0.7f;
    p.decaySeconds = 8.0f;
    p.predelayMs = 25.0f;
    return p;
}

SpaceReverb::Params cathedral() {
    auto p = hall();
    p.size = 1.0f;
    p.decaySeconds = 25.0f;
    p.hfRatio = 0.35f;
    p.hfCrossoverHz = 3000.0f;
    p.modDepth = 0.7f;
    p.predelayMs = 40.0f;
    return p;
}

template <typename Reverb, typename Params>
Stereo reverb(const Stereo& in, const Params& p, float lengthSeconds) {
    Reverb r;
    r.prepare(in.sampleRate);
    r.setParams(p);
    return runWet(r, in, size_t(lengthSeconds * in.sampleRate));
}

Stereo space(const Stereo& in, const SpaceReverb::Params& p) { return reverb<SpaceReverb>(in, p, seconds(in)); }

Stereo tape(const Stereo& in, const TapeEcho::Params& p) {
    TapeEcho echo;
    echo.prepare(in.sampleRate);
    echo.setParams(p);
    return runWet(echo, in, in.size());
}

Stereo color(const Stereo& in, const Color::Params& p) {
    Color c;
    c.prepare(in.sampleRate);
    c.setParams(p);
    return runWet(c, in, in.size());
}

enum class Engine { Fft8192, Fft4096, Zero };
enum class KeySource { Midi, Auto };  // Midi = the source's notes (or its scale)

// FOCUS on `in` (a wet signal), driven by the source's MIDI / scale, or by the AUTO
// detector listening to the source's dry audio.
template <typename Focus>
Stereo runFocus(Focus& f, const Source& src, const Stereo& in, KeySource keys) {
    KeyDetector detector;
    detector.prepare(in.sampleRate);
    if (keys == KeySource::Midi && src.scale) f.setScale(src.scale);
    size_t nextEvent = 0;
    Stereo out = runWet(f, in, in.size(), [&](size_t pos) {
        if (keys == KeySource::Auto) {
            if (pos + kBlock <= src.audio.size()) detector.analyse(&src.audio.left[pos], &src.audio.right[pos], kBlock);
            f.setScale(detector.pitchClasses());
            return;
        }
        if (src.scale) return;
        const float t = float(pos) / in.sampleRate;
        for (; nextEvent < src.midi.size() && src.midi[nextEvent].time <= t; ++nextEvent) {
            const auto& e = src.midi[nextEvent];
            e.on ? f.noteOn(e.note) : f.noteOff(e.note);
        }
    });
    // Realign with the dry signal: in the plugin the host compensates (or the wet just lags).
    const size_t lat = size_t(f.latencySamples());
    out.left.erase(out.left.begin(), out.left.begin() + long(lat));
    out.right.erase(out.right.begin(), out.right.begin() + long(lat));
    out.left.resize(in.size(), 0.0f);
    out.right.resize(in.size(), 0.0f);
    return out;
}

constexpr float kFocusAmount = 0.6f, kFocusDepth = 0.9f;

Stereo focus(const Source& src, const Stereo& in, bool harmonic, Engine engine, KeySource keys = KeySource::Midi) {
    if (engine == Engine::Zero) {
        ResonantFocus f;
        f.prepare(in.sampleRate);
        f.setParams({harmonic ? ResonantFocus::Mode::Harmonic : ResonantFocus::Mode::Key, kFocusAmount, kFocusDepth, 1.0f, true});
        return runFocus(f, src, in, keys);
    }
    HarmonicFocus f;
    f.prepare(in.sampleRate, engine == Engine::Fft8192 ? 8192 : 4096);
    f.setParams({harmonic ? HarmonicFocus::Mode::Harmonic : HarmonicFocus::Mode::Key, kFocusAmount, kFocusDepth, 1.0f, true});
    return runFocus(f, src, in, keys);
}

// TONAL ◂▸ NOISE, realigned with the dry (the host would compensate its latency).
Stereo tonal(const Stereo& in, float balance) {
    TonalNoise t;
    t.prepare(in.sampleRate);
    t.setParams({balance, 0.5f, 1.0f});
    Stereo out = runWet(t, in, in.size() + size_t(t.latencySamples()));
    const size_t lat = size_t(t.latencySamples());
    out.left.erase(out.left.begin(), out.left.begin() + long(lat));
    out.right.erase(out.right.begin(), out.right.begin() + long(lat));
    return out;
}

// DEREZ fed with the source's MIDI (the lowest held note is the root).
Stereo derez(const Source& src, const Stereo& in, const PitchedDerez::Params& p) {
    PitchedDerez d;
    d.prepare(in.sampleRate);
    d.setParams(p);
    size_t nextEvent = 0;
    return runWet(d, in, in.size(), [&](size_t pos) {
        const float t = float(pos) / in.sampleRate;
        for (; nextEvent < src.midi.size() && src.midi[nextEvent].time <= t; ++nextEvent) {
            const auto& e = src.midi[nextEvent];
            e.on ? d.noteOn(e.note) : d.noteOff(e.note);
        }
    });
}

PitchedDerez::Params derezParams(float interval, bool free = false) {
    PitchedDerez::Params p;
    p.interval = interval;
    p.crush = 0.6f;
    p.bits = 8.0f;
    p.free = free;
    p.freeRateHz = 3000.0f;
    return p;
}

// --- Recipes --------------------------------------------------------------------

struct Recipe {
    std::string name;
    std::string description;
    std::function<Stereo(const Source&)> render;
};

std::vector<Recipe> recipes() {
    std::vector<Recipe> r;

    r.push_back({"00_dry", "source, untouched", [](const Source& s) { return s.audio; }});

    r.push_back({"01_hall", "hall, size 0.7, RT60 8 s", [](const Source& s) {
                     return mix(s.audio, 0.7f, space(s.audio, hall()), 0.8f);
                 }});
    r.push_back({"02_cathedral", "cathedral, size 1, RT60 25 s", [](const Source& s) {
                     return mix(s.audio, 0.5f, space(s.audio, cathedral()), 1.0f);
                 }});

    r.push_back({"03_plate", "Dattorro plate, decay 0.85", [](const Source& s) {
                     PlateReverb::Params p;
                     p.decay = 0.85f;
                     p.damping = 0.25f;
                     p.size = 1.3f;
                     return mix(s.audio, 0.7f, reverb<PlateReverb>(s.audio, p, seconds(s.audio)), 0.8f);
                 }});

    struct ShimmerVariant {
        const char* name;
        const char* description;
        float pitch, depth;
    };
    const ShimmerVariant shimmers[] = {
        {"04a_shimmer_+12_depth50", "shimmer PITCH +12, DEPTH 0.5", 12.0f, 0.5f},
        {"04b_shimmer_+12_depth100", "shimmer PITCH +12, DEPTH 1", 12.0f, 1.0f},
        {"04c_shimmer_+7", "shimmer PITCH +7, DEPTH 0.8", 7.0f, 0.8f},
        {"04d_shimmer_+24", "shimmer PITCH +24, DEPTH 0.7", 24.0f, 0.7f},
        {"04e_shimmer_-12", "shimmer PITCH -12, DEPTH 0.8 (dark cascade)", -12.0f, 0.8f},
        {"04f_shimmer_-24", "shimmer PITCH -24, DEPTH 0.6", -24.0f, 0.6f},
    };
    for (const auto& v : shimmers) {
        r.push_back({v.name, v.description, [v](const Source& s) {
                         auto p = hall();
                         p.size = 0.9f;
                         p.decaySeconds = 18.0f;
                         p.hfRatio = 0.6f;
                         p.shimmerSemitones = v.pitch;
                         p.shimmer = v.depth;
                         return mix(s.audio, 0.5f, space(s.audio, p), 1.0f);
                     }});
    }

    r.push_back({"05_tape_echo", "tape echo 420 ms, fb 0.65, into a small room", [](const Source& s) {
                     TapeEcho::Params ep;
                     ep.feedback = 0.65f;
                     const Stereo withEcho = mix(s.audio, 1.0f, tape(s.audio, ep), 0.7f);
                     auto p = hall();
                     p.size = 0.4f;
                     p.decaySeconds = 3.0f;
                     return mix(withEcho, 0.8f, space(withEcho, p), 0.4f);
                 }});

    r.push_back({"05b_tape_sync", "tape echo SYNC 1/8 dotted @ 96 BPM, ping-pong, into a small room", [](const Source& s) {
                     TapeEcho::Params ep;
                     ep.sync = true;
                     ep.bpm = 96.0f;
                     ep.division = TapeEcho::Division::EighthDotted;
                     ep.feedback = 0.55f;
                     ep.pingPong = true;
                     const Stereo withEcho = mix(s.audio, 1.0f, tape(s.audio, ep), 0.7f);
                     auto p = hall();
                     p.size = 0.4f;
                     p.decaySeconds = 3.0f;
                     return mix(withEcho, 0.8f, space(withEcho, p), 0.4f);
                 }});

    r.push_back({"06_paulstretch_x8", "PaulStretch x8, window 0.68 s, then hall", [](const Source& s) {
                     const Stereo& in = s.audio;
                     PaulStretch ps;
                     ps.prepare(32768);
                     Stereo st;
                     st.sampleRate = in.sampleRate;
                     st.resize(size_t(std::min(seconds(in) * 8.0f, 45.0f) * in.sampleRate));
                     ps.render(in.left.data(), in.right.data(), in.size(), 0.0, 8.0, st.left.data(), st.right.data(), st.size());
                     auto p = hall();
                     p.decaySeconds = 6.0f;
                     return mix(st, 0.8f, space(st, p), 0.5f);
                 }});

    r.push_back({"07_paulstretch_x50_wide", "PaulStretch x50, window 2.7 s", [](const Source& s) {
                     const Stereo& in = s.audio;
                     PaulStretch ps;
                     ps.prepare(131072);
                     Stereo st;
                     st.sampleRate = in.sampleRate;
                     st.resize(size_t(45.0f * in.sampleRate));
                     ps.render(in.left.data(), in.right.data(), in.size(), 0.0, 50.0, st.left.data(), st.right.data(), st.size());
                     return st;
                 }});

    r.push_back({"08_spectral_freeze", "spectral freeze of the moment at 3 s", [](const Source& s) {
                     const Stereo& in = s.audio;
                     PaulStretch ps;
                     ps.prepare(16384);
                     Stereo st;
                     st.sampleRate = in.sampleRate;
                     st.resize(size_t(20.0f * in.sampleRate));
                     ps.render(in.left.data(), in.right.data(), in.size(), 3.0 * in.sampleRate, 0.0, st.left.data(), st.right.data(), st.size());
                     return st;
                 }});

    r.push_back({"09_motion", "LFOs on room size (random sine), echo time (drunk), shimmer (perlin)", [](const Source& s) {
                     const Stereo& in = s.audio;
                     const float fs = in.sampleRate;
                     Lfo sizeLfo, timeLfo, shimmerLfo;
                     sizeLfo.prepare(fs, 1);
                     sizeLfo.setShape(LfoShape::RandomSmooth);
                     sizeLfo.setRate(0.15f);
                     timeLfo.prepare(fs, 2);
                     timeLfo.setShape(LfoShape::Drunk);
                     timeLfo.setRate(0.3f);
                     shimmerLfo.prepare(fs, 3);
                     shimmerLfo.setShape(LfoShape::Perlin);
                     shimmerLfo.setRate(0.1f);

                     TapeEcho echo;
                     echo.prepare(fs);
                     TapeEcho::Params ep;
                     ep.feedback = 0.55f;
                     const Stereo wetEcho = runWet(echo, in, in.size(), [&](size_t) {
                         ep.timeMs = 380.0f + 120.0f * timeLfo.advance(kBlock);
                         echo.setParams(ep);
                     });
                     const Stereo withEcho = mix(in, 1.0f, wetEcho, 0.6f);

                     SpaceReverb rev;
                     rev.prepare(fs);
                     auto p = hall();
                     p.decaySeconds = 14.0f;
                     const Stereo wet = runWet(rev, withEcho, in.size(), [&](size_t) {
                         p.size = 0.55f + 0.35f * sizeLfo.advance(kBlock);
                         p.shimmer = 0.25f + 0.25f * shimmerLfo.advance(kBlock);
                         rev.setParams(p);
                     });
                     return mix(in, 0.5f, wet, 1.0f);
                 }});

    const auto chainWet = [](const Source& s) {
        TapeEcho::Params ep;
        ep.timeMs = 550.0f;
        ep.feedback = 0.6f;
        ep.pingPong = true;
        const Stereo withEcho = mix(s.audio, 1.0f, tape(s.audio, ep), 0.6f);
        auto p = cathedral();
        p.decaySeconds = 30.0f;
        p.hfRatio = 0.45f;
        p.shimmer = 0.45f;
        p.modDepth = 0.8f;
        return space(withEcho, p);
    };

    r.push_back({"10_ambient_chain", "tape ping-pong -> shimmer cathedral", [=](const Source& s) {
                     return mix(s.audio, 0.4f, chainWet(s), 1.0f);
                 }});

    r.push_back({"11_chain_color", "same chain + COLOR: bass -0.3, mud 0.8, air +0.3", [=](const Source& s) {
                     return mix(s.audio, 0.4f, color(chainWet(s), {.bass = -0.3f, .mud = 0.8f, .air = 0.3f}), 1.0f);
                 }});

    struct FocusVariant {
        const char* name;
        const char* description;
        bool harmonic;
        Engine engine;
        KeySource keys;
    };
    const FocusVariant variants[] = {
        {"12a_focus_key_fft_170ms", "cathedral -> FOCUS key, FFT 8192 (170 ms wet latency)", false, Engine::Fft8192, KeySource::Midi},
        {"12b_focus_key_fft_85ms", "cathedral -> FOCUS key, FFT 4096 (85 ms)", false, Engine::Fft4096, KeySource::Midi},
        {"12c_focus_key_zero", "cathedral -> FOCUS key, resonator bank (0 latency)", false, Engine::Zero, KeySource::Midi},
        {"13a_focus_harmonic_fft_170ms", "cathedral -> FOCUS harmonic, FFT 8192 (170 ms)", true, Engine::Fft8192, KeySource::Midi},
        {"13c_focus_harmonic_zero", "cathedral -> FOCUS harmonic, tuned combs (0 latency)", true, Engine::Zero, KeySource::Midi},
        {"15_focus_auto_zero", "cathedral -> FOCUS key, AUTO key detection from audio, no MIDI (0 latency)", false, Engine::Zero, KeySource::Auto},
    };
    for (const auto& v : variants) {
        r.push_back({v.name, v.description, [v](const Source& s) {
                         return mix(s.audio, 0.5f, focus(s, space(s.audio, cathedral()), v.harmonic, v.engine, v.keys), 1.0f);
                     }});
    }

    struct DerezVariant {
        const char* name;
        const char* description;
        float interval;
        bool free;
    };
    const DerezVariant derezVariants[] = {
        {"16a_derez_free_cathedral", "classic DeRez (unlocked) -> cathedral", 0.0f, true},
        {"16b_derez_tuned_0_cathedral", "tuned DeRez 0 -> cathedral", 0.0f, false},
        {"16c_derez_tuned_-12_cathedral", "tuned DeRez -12 -> cathedral", -12.0f, false},
        {"16d_derez_tuned_+12_cathedral", "tuned DeRez +12 -> cathedral", 12.0f, false},
        {"16e_derez_tuned_+7_cathedral", "tuned DeRez +7 -> cathedral", 7.0f, false},
        {"16f_derez_tuned_-24_cathedral", "tuned DeRez -24 -> cathedral", -24.0f, false},
    };
    // DEREZ sits before SPACE: the big reverb turns the crunch into a texture.
    for (const auto& v : derezVariants) {
        r.push_back({v.name, v.description, [v](const Source& s) {
                         const Stereo crushed = mix(s.audio, 0.5f, derez(s, s.audio, derezParams(v.interval, v.free)), 0.9f);
                         return mix(s.audio, 0.4f, space(crushed, cathedral()), 1.0f);
                     }});
    }

    // STRETCH (capture) -> ECHO (tape ping-pong) -> SPACE (cathedral): the module order of the plugin.
    // TONAL sits either before SPACE (PRE: the reverb rebuilds a tail from what is kept) or
    // after it (POST: the tail itself is filtered).
    const auto captureChain = [](const Source& s, CaptureStretch::Params cp, float captureAt, float tonalBalance = 0.0f,
                                 bool tonalPre = false) {
        const Stereo& in = s.audio;
        const float length = std::max(seconds(in), 40.0f);
        CaptureStretch cap;
        cap.prepare(in.sampleRate);
        cap.setParams(cp);
        bool captured = false;
        const Stereo stretched = runWet(cap, in, size_t(length * in.sampleRate), [&](size_t pos) {
            if (!captured && captureAt >= 0.0f && float(pos) / in.sampleRate >= captureAt) {
                cap.capture();
                captured = true;
            }
        });
        TapeEcho::Params ep;
        ep.sync = true;
        ep.bpm = 80.0f;
        ep.division = TapeEcho::Division::QuarterDotted;
        ep.feedback = 0.6f;
        ep.pingPong = true;
        Stereo withEcho = mix(stretched, 1.0f, tape(stretched, ep), 0.6f);
        if (tonalBalance != 0.0f && tonalPre) withEcho = tonal(withEcho, tonalBalance);
        auto p = cathedral();
        p.decaySeconds = 20.0f;
        Stereo wet = mix(withEcho, 0.5f, space(withEcho, p), 1.0f);
        if (tonalBalance != 0.0f && !tonalPre) wet = tonal(wet, tonalBalance);  // last stage of the wet chain
        return mix(in, 0.35f, wet, 1.0f);
    };
    r.push_back({"17a_capture_smear_pong_cathedral", "STRETCH smear x8 (live) -> tape ping-pong SYNC 1/4. @80 -> cathedral", [=](const Source& s) {
                     return captureChain(s, {CaptureStretch::Mode::Smear, 8.0f, 4.0f}, -1.0f);
                 }});
    r.push_back({"17b_capture_loop_pong_cathedral", "CAPTURE 4 s at 5 s, x30 loop -> ping-pong SYNC 1/4. @80 -> cathedral, under the dry", [=](const Source& s) {
                     return captureChain(s, {CaptureStretch::Mode::Capture, 30.0f, 4.0f}, 5.0f);
                 }});

    r.push_back({"18a_chain_tonal", "capture smear -> ping-pong -> cathedral -> TONAL +0.8 (pure notes)", [=](const Source& s) {
                     return captureChain(s, {CaptureStretch::Mode::Smear, 8.0f, 4.0f}, -1.0f, 0.8f);
                 }});
    r.push_back({"18b_chain_noise", "capture smear -> ping-pong -> cathedral -> NOISE -0.8 (air, breath)", [=](const Source& s) {
                     return captureChain(s, {CaptureStretch::Mode::Smear, 8.0f, 4.0f}, -1.0f, -0.8f);
                 }});
    r.push_back({"18d_chain_tonal_pre", "capture smear -> ping-pong -> TONAL +0.8 -> cathedral (PRE)", [=](const Source& s) {
                     return captureChain(s, {CaptureStretch::Mode::Smear, 8.0f, 4.0f}, -1.0f, 0.8f, true);
                 }});
    r.push_back({"18e_chain_noise_pre", "capture smear -> ping-pong -> NOISE -0.8 -> cathedral (PRE)", [=](const Source& s) {
                     return captureChain(s, {CaptureStretch::Mode::Smear, 8.0f, 4.0f}, -1.0f, -0.8f, true);
                 }});
    r.push_back({"18c_cathedral_tonal_full", "cathedral -> TONAL +1", [](const Source& s) {
                     return mix(s.audio, 0.5f, tonal(space(s.audio, cathedral()), 1.0f), 1.0f);
                 }});

    r.push_back({"14_full_chain", "tape -> shimmer cathedral -> FOCUS key zero latency -> COLOR", [=](const Source& s) {
                     const Stereo wet = color(focus(s, chainWet(s), false, Engine::Zero), {.bass = -0.3f, .mud = 0.8f, .air = 0.3f});
                     return mix(s.audio, 0.4f, wet, 1.0f);
                 }});
    return r;
}

// --- Objective metrics ----------------------------------------------------------

// Normalized echo density (Abel & Huang 2006): share of samples beyond one standard
// deviation in a 20 ms window, divided by the Gaussian expectation. 1 = noise-like.
std::vector<float> echoDensity(const std::vector<float>& x, float fs, int hop) {
    const int win = int(0.02f * fs);
    std::vector<float> ned;
    for (size_t start = 0; start + size_t(win) < x.size(); start += size_t(hop)) {
        double sumSq = 0.0;
        for (int i = 0; i < win; ++i) sumSq += double(x[start + size_t(i)]) * x[start + size_t(i)];
        const float sd = float(std::sqrt(sumSq / win));
        int outside = 0;
        for (int i = 0; i < win; ++i) outside += std::abs(x[start + size_t(i)]) > sd;
        ned.push_back(sd > 0.0f ? float(outside) / float(win) / 0.3173f : 0.0f);
    }
    return ned;
}

// Max normalized autocorrelation of a tail segment over 2..50 ms lags:
// high = periodic ringing ("metallic"), low = smooth.
float ringing(const std::vector<float>& x, size_t from, size_t length, float fs) {
    double energy = 0.0;
    for (size_t i = from; i < from + length; ++i) energy += double(x[i]) * x[i];
    float worst = 0.0f;
    for (int lag = int(0.002f * fs); lag < int(0.05f * fs); ++lag) {
        double acc = 0.0;
        for (size_t i = from; i < from + length; ++i) acc += double(x[i]) * x[i + size_t(lag)];
        worst = std::max(worst, float(std::abs(acc) / energy));
    }
    return worst;
}

float correlation(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i) {
        ab += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    return float(ab / std::sqrt(aa * bb + 1e-30));
}

template <typename Reverb, typename Params>
void reportImpulse(const char* name, Params p, float fs) {
    p.predelayMs = 0.0f;
    Stereo impulse;
    impulse.sampleRate = fs;
    impulse.resize(size_t(3.0f * fs));
    impulse.left[0] = 1.0f;
    impulse.right[0] = 1.0f;
    const Stereo ir = reverb<Reverb>(impulse, p, 3.0f);

    std::vector<float> mono(ir.size());
    for (size_t i = 0; i < ir.size(); ++i) mono[i] = 0.5f * (ir.left[i] + ir.right[i]);
    const int hop = int(0.005f * fs);
    const auto ned = echoDensity(mono, fs, hop);
    float onsetMs = -1.0f, sum = 0.0f;
    int count = 0;
    for (size_t i = 0; i < ned.size(); ++i) {
        const float t = float(i) * 0.005f;
        if (onsetMs < 0.0f && ned[i] >= 0.9f) onsetMs = t * 1000.0f;
        if (t >= 0.2f && t < 1.0f) sum += ned[i], ++count;
    }
    std::printf("%-18s %10.0f ms %10.2f %10.3f %10.2f\n", name, onsetMs, sum / float(count),
                ringing(mono, size_t(0.3f * fs), size_t(1.0f * fs), fs),
                correlation(ir.left, ir.right, size_t(0.2f * fs), size_t(2.0f * fs)));
}

void analyseReverbs(float fs) {
    std::printf("%-18s %13s %10s %10s %10s\n", "reverb", "dense after", "NED tail", "ringing", "L/R corr");
    std::printf("%-18s %13s %10s %10s %10s\n", "", "(lower=better)", "(1=ideal)", "(lower)", "(0=wide)");
    reportImpulse<SpaceReverb>("hall", hall(), fs);
    reportImpulse<SpaceReverb>("cathedral", cathedral(), fs);
    PlateReverb::Params plate;
    plate.decay = 0.85f;
    plate.size = 1.3f;
    reportImpulse<PlateReverb>("plate", plate, fs);

    // Freeze stability: excite, freeze, check the level stays put for a minute.
    const Source pad = sources::pad(fs, 70.0f);
    SpaceReverb rev;
    rev.prepare(fs);
    auto p = cathedral();
    const Stereo frozen = runWet(rev, pad.audio, pad.audio.size(), [&](size_t pos) {
        p.freeze = pos > size_t(4.0f * fs);
        rev.setParams(p);
    });
    auto levelAt = [&](float t) {
        double acc = 0.0;
        const size_t from = size_t(t * fs), n = size_t(fs);
        for (size_t i = from; i < from + n; ++i) acc += double(frozen.left[i]) * frozen.left[i];
        return 10.0f * std::log10(float(acc / double(n)) + 1e-20f);
    };
    std::printf("\nfreeze: level at 10 s %.1f dB, at 65 s %.1f dB (drift %.2f dB / minute)\n", levelAt(10.0f),
                levelAt(65.0f), levelAt(65.0f) - levelAt(10.0f));
}

std::string pitchClassNames(uint16_t mask) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::string s;
    for (int pc = 0; pc < 12; ++pc)
        if (mask & (1u << pc)) s += std::string(s.empty() ? "" : " ") + names[pc];
    return s.empty() ? "-" : s;
}

// Tuning of a reverb tail: a sine burst goes in, and in successive windows we look at
// the tail around the expected pitch (times `ratio` for shimmer): where the peak sits
// (cents off) and how much energy stays within ±10 cents ("purity"). Averaged over
// five notes, since a single sine mostly reports whichever room mode sits nearest.
template <typename Reverb, typename Params>
void reportTuning(const char* name, const Params& p, float fs, float ratio = 1.0f) {
    const float notes[] = {261.63f, 329.63f, 392.0f, 440.0f, 523.25f};
    const float windows[] = {0.4f, 1.2f, 3.0f};
    float cents[3] = {}, purity[3] = {};
    const int n = 32768;
    Fft fft;
    fft.prepare(n);
    for (const float f0 : notes) {
        Stereo burst;
        burst.sampleRate = fs;
        burst.resize(size_t(6.0f * fs));
        for (size_t i = 0; i < size_t(0.4f * fs); ++i) {
            const float t = float(i) / fs;
            const float env = std::min({1.0f, t / 0.02f, (0.4f - t) / 0.02f});
            burst.left[i] = burst.right[i] = 0.3f * env * std::sin(kTwoPi * f0 * t);
        }
        const Stereo tail = reverb<Reverb>(burst, p, 6.0f);
        const float f = f0 * ratio, binHz = fs / float(n);
        for (int w = 0; w < 3; ++w) {
            std::vector<Fft::Complex> spec(static_cast<size_t>(n));
            const size_t start = size_t(windows[w] * fs);
            for (int i = 0; i < n; ++i)
                spec[size_t(i)] = (tail.left[start + size_t(i)] + tail.right[start + size_t(i)]) *
                                  (0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(n)));
            fft.forward(spec);
            const int lo = int(f * 0.944f / binHz), hi = int(f * 1.059f / binHz);  // ±100 cents
            int best = lo;
            double around = 0.0, inTune = 0.0;
            for (int k = lo; k <= hi; ++k)
                if (std::norm(spec[size_t(k)]) > std::norm(spec[size_t(best)])) best = k;
            const float a = std::log(std::abs(spec[size_t(best - 1)]) + 1e-12f), b = std::log(std::abs(spec[size_t(best)]) + 1e-12f),
                        c = std::log(std::abs(spec[size_t(best + 1)]) + 1e-12f);
            const float peakHz = (float(best) + 0.5f * (a - c) / (a - 2.0f * b + c)) * binHz;
            for (int k = lo; k <= hi; ++k) {
                const double e = std::norm(spec[size_t(k)]);
                around += e;
                if (std::abs(12.0f * std::log2(float(k) * binHz / f)) < 0.1f) inTune += e;
            }
            cents[w] += std::abs(1200.0f * std::log2(peakHz / f)) / 5.0f;
            purity[w] += float(100.0 * inTune / around) / 5.0f;
        }
    }
    std::printf("  %-22s", name);
    for (int w = 0; w < 3; ++w) std::printf("  %5.1f ct %5.1f %%", cents[w], purity[w]);
    std::printf("\n");
}

void analyseTuning(float fs) {
    std::printf("\nTUNING of the tail, 5 sine bursts C4..C5 (mean |peak offset|, energy within ±10 cents):\n");
    std::printf("  %-22s  %-17s  %-17s  %-17s\n", "", "0.4-1.1 s (start)", "1.2-1.9 s", "3.0-3.7 s");
    reportTuning<SpaceReverb>("hall", hall(), fs);
    reportTuning<SpaceReverb>("cathedral", cathedral(), fs);
    auto s = hall();
    s.size = 0.9f;
    s.decaySeconds = 18.0f;
    s.shimmer = 0.6f;
    reportTuning<SpaceReverb>("shimmer +12 (octave)", s, fs, 2.0f);
    s.shimmer = 1.0f;
    reportTuning<SpaceReverb>("shimmer +12 depth 1", s, fs, 2.0f);
    s.shimmerSemitones = 7.0f;
    reportTuning<SpaceReverb>("shimmer +7 (fifth)", s, fs, semitonesToRatio(7.0f));
    s.shimmerSemitones = -12.0f;
    reportTuning<SpaceReverb>("shimmer -12 (octave)", s, fs, 0.5f);
}

// Share of the energy that sits on the target note's harmonics (±35 cents).
// 1 = perfectly in tune; the classic unlocked DeRez scatters energy in between.
void analyseDerez(float fs) {
    const float f0 = 110.0f;
    Stereo saw;
    saw.sampleRate = fs;
    saw.resize(size_t(2.5f * fs));
    float phase = 0.0f;
    for (size_t i = 0; i < saw.size(); ++i) {
        const float dt = f0 / fs;
        saw.left[i] = saw.right[i] = 0.3f * (2.0f * phase - 1.0f - sources::polyBlep(phase, dt));
        phase += dt;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    const int n = 65536;
    Fft fft;
    fft.prepare(n);
    auto harmonicity = [&](const Stereo& s, float g) {
        std::vector<Fft::Complex> spec(static_cast<size_t>(n));
        const size_t start = size_t(0.5f * fs);
        for (int i = 0; i < n; ++i)
            spec[size_t(i)] = s.left[start + size_t(i)] * (0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(n)));
        fft.forward(spec);
        double on = 0.0, total = 0.0;
        for (int k = int(50.0f * n / fs); k < n / 2; ++k) {
            const float f = float(k) * fs / float(n);
            const double e = std::norm(spec[size_t(k)]);
            total += e;
            const float h = std::max(1.0f, std::round(f / g));
            if (std::abs(12.0f * std::log2(f / (h * g))) < 0.35f) on += e;
        }
        return float(on / total);
    };
    std::printf("\nDEREZ in-tune energy on a 110 Hz saw (share on the target's harmonics, 1 = in tune):\n");
    std::printf("  %-10s %10s %10s\n", "interval", "classic", "tuned");
    for (const float interval : {0.0f, 7.0f, -12.0f, 12.0f}) {
        // Same amount of crush for both: the classic rate is the tuned one, 6 % off.
        PitchedDerez tuned;
        tuned.prepare(fs);
        tuned.setParams(derezParams(interval));
        tuned.setRoot(f0);
        auto classicParams = derezParams(interval, true);
        classicParams.freeRateHz = tuned.holdRateHz() * 1.06f;
        PitchedDerez classic;
        classic.prepare(fs);
        classic.setParams(classicParams);
        const float g = f0 * semitonesToRatio(interval);
        const float results[2] = {harmonicity(runWet(classic, saw, saw.size()), g), harmonicity(runWet(tuned, saw, saw.size()), g)};
        std::printf("  %+-10.0f %10.2f %10.2f\n", interval, results[0], results[1]);
    }
}

// TONAL ◂▸ NOISE separation: a chord of sines (with harmonics) inside pink-ish noise.
// Tonal-to-noise ratio = energy within ±15 cents of the partials vs everything else.
void analyseTonal(float fs) {
    const float partials[] = {220.0f, 261.63f, 329.63f, 440.0f, 523.25f, 659.26f, 880.0f};
    Stereo test;
    test.sampleRate = fs;
    test.resize(size_t(3.0f * fs));
    Rng rng(5);
    float pink = 0.0f;
    for (size_t i = 0; i < test.size(); ++i) {
        float v = 0.0f;
        for (const float f : partials) v += 0.05f * std::sin(kTwoPi * f * float(i) / fs);
        pink += 0.02f * (rng.bipolar() - pink);  // gentle lowpass: noise darker than white
        v += 0.9f * pink;
        test.left[i] = test.right[i] = v;
    }
    const int n = 65536;
    Fft fft;
    fft.prepare(n);
    auto ratioDb = [&](const Stereo& s) {
        std::vector<Fft::Complex> spec(static_cast<size_t>(n));
        const size_t start = size_t(1.0f * fs);
        for (int i = 0; i < n; ++i) spec[size_t(i)] = s.left[start + size_t(i)] * (0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(n)));
        fft.forward(spec);
        double tonal = 0.0, noise = 0.0;
        for (int k = int(100.0f * n / fs); k < int(8000.0f * n / fs); ++k) {
            const float f = float(k) * fs / float(n);
            bool near = false;
            for (const float p : partials) near = near || std::abs(1200.0f * std::log2(f / p)) < 15.0f;
            (near ? tonal : noise) += std::norm(spec[size_t(k)]);
        }
        return 10.0f * std::log10(float(tonal / noise));
    };
    std::printf("\nTONAL / NOISE separation (tonal-to-noise ratio, chord of sines in noise):\n");
    std::printf("  input                 %+6.1f dB\n", ratioDb(test));
    for (const float balance : {1.0f, 0.5f, -0.5f, -1.0f}) {
        TonalNoise t;
        t.prepare(fs);
        t.setParams({balance, 0.5f, 1.0f});
        std::printf("  balance %+4.1f          %+6.1f dB\n", balance, ratioDb(runWet(t, test, test.size())));
    }
    // PRE vs POST around a cathedral (wet only): the reverb smears partials over time, so
    // the ratio measured after it is lower than the module's own.
    auto p = cathedral();
    p.decaySeconds = 6.0f;
    std::printf("  cathedral alone       %+6.1f dB\n", ratioDb(space(test, p)));
    for (const float balance : {1.0f, -1.0f}) {
        const float pre = ratioDb(space(tonal(test, balance), p));
        const float post = ratioDb(tonal(space(test, p), balance));
        std::printf("  balance %+4.1f   PRE %+6.1f dB   POST %+6.1f dB\n", balance, pre, post);
    }
}

// What the AUTO detector hears over time, against what is really played.
void analyseKeys(float fs) {
    std::printf("\nAUTO key detection (no MIDI):\n");
    // CHORD memory (1.5 s) follows harmony; SCALE memory (6 s) collects a melody's notes.
    const std::pair<const char*, KeyDetector::Params> settings[] = {{"chord", {1.5f, 0.45f, 7}}, {"scale", {6.0f, 0.3f, 7}}};
    for (const auto& [label, params] : settings)
    for (const Source& src : {sources::pad(fs), sources::plucks(fs)}) {
        KeyDetector detector;
        detector.prepare(fs);
        detector.setParams(params);
        std::printf("  %-6s %-7s", label, src.name.c_str());
        for (size_t pos = 0; pos + kBlock <= src.audio.size(); pos += kBlock) {
            detector.analyse(&src.audio.left[pos], &src.audio.right[pos], kBlock);
            const float t = float(pos + kBlock) / fs;
            for (const float at : {2.0f, 5.0f, 8.0f, 11.0f})
                if (t >= at && t - float(kBlock) / fs < at - 1.0e-4f)
                    std::printf("  t=%2.0fs [%s]", at, pitchClassNames(detector.pitchClasses()).c_str());
        }
        std::printf("\n");
    }
    std::printf("  truth:          pad chord 1 = A C E G B, chord 2 (from 5.5 s) = F A C E G; plucks = A C D E G\n");
}

}  // namespace

int main(int argc, char** argv) {
    enableFlushToZero();

    std::string inputPath, outDir = "renders";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--analyse") {
            analyseReverbs(48000.0f);
            analyseKeys(48000.0f);
            analyseDerez(48000.0f);
            analyseTuning(48000.0f);
            analyseTonal(48000.0f);
            return 0;
        }
        if (i + 1 < argc && arg == "--input") inputPath = argv[++i];
        else if (i + 1 < argc && arg == "--out") outDir = argv[++i];
    }
    std::filesystem::create_directories(outDir);

    std::vector<Source> inputs;
    if (!inputPath.empty()) {
        Source src;
        std::string error;
        if (!readWav(inputPath, src.audio, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        // Room for the tails.
        src.audio.left.resize(src.audio.size() + size_t(20.0f * src.audio.sampleRate), 0.0f);
        src.audio.right.resize(src.audio.left.size(), 0.0f);
        src.name = std::filesystem::path(inputPath).stem().string();
        inputs.push_back(std::move(src));
    } else {
        inputs.push_back(sources::pad(48000.0f));
        inputs.push_back(sources::plucks(48000.0f));
    }

    std::printf("%-30s %-8s %8s %8s %10s  %s\n", "render", "source", "peak dB", "rms dB", "CPU %core", "");
    for (const auto& source : inputs) {
        for (const auto& recipe : recipes()) {
            const auto t0 = std::chrono::steady_clock::now();
            Stereo out = recipe.render(source);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            const Stats st = analyse(out);
            levelMatch(out);
            writeWav(outDir + "/" + source.name + "_" + recipe.name + ".wav", out);
            std::printf("%-30s %-8s %8.1f %8.1f %9.2f%%  %s%s\n", recipe.name.c_str(), source.name.c_str(), st.peakDb,
                        st.rmsDb, 100.0 * elapsed / double(seconds(out)), recipe.description.c_str(),
                        st.finite ? "" : "  !! NaN/Inf");
        }
    }
    std::printf("\nWAV files in %s/ (level-matched to -20 dBFS RMS)\n", outDir.c_str());
    return 0;
}
