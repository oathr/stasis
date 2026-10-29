#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// Every parameter of the plugin, in one table shared by the processor (APVTS layout),
// the editor (one web relay each) and the engine (value lookup by index).
//
// Continuous parameters are stored as 0..1, exactly the position of the knob in the
// UI: the engine converts them to real units, and `text` shows the same label as the
// UI, so the DAW's automation lanes read the same values as the plugin.
namespace stasis::params {

enum class Kind { Float, Bool, Choice };

struct Spec {
    std::string id;
    std::string name;
    Kind kind;
    float defaultValue;                      // 0..1, false/true, or choice index
    std::vector<std::string> choices = {};   // Choice only
    std::function<std::string(float)> text = {};
};

inline std::string fmt(const char* f, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}
inline std::string pct(float t) { return fmt("%.0f%%", 100.0 * t); }
inline std::string bip(float t, float scale, const char* unit, const char* f = "%+.1f") { return fmt(f, (2.0 * t - 1.0) * scale) + unit; }

// Units shared by the engine and the labels.
inline float stretchFactor(float t) { return std::max(1.0f, std::pow(1000.0f, t)); }
inline float blurMemory(float t) { return stretchFactor(t) / 8.0f; }  // seconds: 0.125 .. 125
inline float echoTimeMs(float t) { return 20.0f * std::pow(125.0f, t); }
inline int echoDivision(float t) { return int(std::lround(t * 13.0f)); }
inline float decaySeconds(float t) { return t > 0.98f ? INFINITY : 0.5f * std::pow(120.0f, t); }
inline float predelayMs(float t) { return std::pow(500.0f, t); }
inline float derezBits(float t) { return 16.0f - 14.0f * t; }
inline float highpassHz(float t) { return t <= 0.0f ? 0.0f : 20.0f * std::pow(50.0f, t); }      // off, 20 Hz .. 1 kHz
inline float lowpassHz(float t) { return t >= 1.0f ? 0.0f : 1000.0f * std::pow(20.0f, t); }     // 1 kHz .. 20 kHz, off
inline float derezSemitones(float t) { return std::round((2.0f * t - 1.0f) * 24.0f); }
inline float grainDensity(float t) { return 0.5f * std::pow(80.0f, t); }  // 0.5 .. 40 grains per second
inline float grainSizeMs(float t) { return 20.0f * std::pow(100.0f, t); }   // 20 ms .. 2 s
inline float airSmoothMs(float t) { return 20.0f * std::pow(50.0f, t); }    // 20 ms .. 1 s (BREATH's envelope)
inline float pitchSemitones(float t) { return std::round((2.0f * t - 1.0f) * 24.0f); }
// ±50 Hz with fine control near 0 (cubic): ±1 Hz beats live in the middle of the travel.
inline float shiftHz(float t) { const float x = 2.0f * t - 1.0f; return 50.0f * x * x * x; }
inline int derezMultiple(float t) { return int(std::lround(32.0f * std::pow(2.0f / 32.0f, t))); }

inline const std::array<const char*, 14> kDivisionNames = {"1/32", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.",
                                                           "1/4T", "1/4",   "1/4.", "1/2T",  "1/2", "1/2.", "1/1"};
inline const std::array<float, 14> kDivisionBeats = {0.125f, 1.0f / 6.0f, 0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f,
                                                     2.0f / 3.0f, 1.0f, 1.5f, 4.0f / 3.0f, 2.0f, 3.0f, 4.0f};
// The modules of the chain, in their default order. The order is part of the state
// (not a parameter): the UI changes it, the engine reads it.
// Indices are stable (new modules are added at the end); kDefaultOrder is the order
// a new instance starts with.
inline const std::array<const char*, 10> kModules = {"shape", "derez", "pitch", "stretch", "echo", "space", "focus", "air", "color", "grain"};
inline const std::array<int, 10> kDefaultOrder = {0, 1, 2, 3, 9, 4, 5, 6, 7, 8};  // SHAPE DEREZ PITCH STRETCH GRAIN ECHO SPACE FOCUS AIR COLOR
inline const char* const kDefaultOrderNames = "shape,derez,pitch,stretch,grain,echo,space,focus,air,color";

// MOTION: 12 LFO lanes. The user maps each one to any knob (the mapping is part of the
// state, not a parameter); by default only lane 1 is mapped, to the global DRY / WET.
// DEPTH is the share of the knob's whole travel the lane sweeps: at 100 % the knob goes
// from 0 to 100 %, at 50 % it sweeps halfway between where the user left it and that.
// A lane at depth 0 does nothing and costs nothing.
struct Lane {
    int shape;  // LfoShape index
    float rateHz;
};
inline const std::array<Lane, 12> kLanes = {{
    {7, 0.05f}, {9, 0.03f}, {6, 0.06f}, {7, 0.20f}, {7, 0.03f}, {9, 0.02f},
    {0, 0.08f}, {6, 0.05f}, {1, 0.07f}, {11, 0.12f}, {9, 0.04f}, {0, 0.05f},
}};
inline const std::array<const char*, 12> kDefaultTargets = {"mix", "", "", "", "", "", "", "", "", "", "", ""};
inline const std::array<const char*, 12> kShapeNames = {"SINE", "TRI", "SAW+", "SAW-", "SQUARE", "S&H", "SLEW", "R-SINE", "DRUNK", "PERLIN", "CHAOS", "BREATH"};
inline float laneRateHz(float t) { return 0.001f * std::pow(20000.0f, t); }  // 0.001 .. 20 Hz
inline float laneRateT(float hz) { return std::log(hz / 0.001f) / std::log(20000.0f); }
inline float driftScale(float t) { return std::pow(4.0f, 2.0f * t - 1.0f); }  // ×0.25 .. ×4
inline std::string laneId(int lane, const char* what) { return (lane < 9 ? "m0" : "m") + std::to_string(lane + 1) + "_" + what; }
// What MOTION may move: every continuous knob, except MOTION's own.
inline bool isMotionId(const std::string& id) { return id.rfind("motion_", 0) == 0 || (id.size() > 4 && id[0] == 'm' && id[3] == '_'); }


inline const std::vector<Spec>& all() {
    static const std::vector<Spec> specs = {
        // Global
        {"mix", "Dry / Wet", Kind::Float, 1.0f, {}, pct},
        {"out", "Output", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 12.0f, " dB"); }},
        {"freeze", "Freeze", Kind::Bool, 0.0f},
        // SHAPE
        {"shape_on", "Shape On", Kind::Bool, 0.0f},
        {"shape_type", "Shape Type", Kind::Choice, 0.0f, {"Tape", "Tube", "Fold", "Rectify", "Slew", "Bloom"}},
        {"shape_tilt", "Shape Tilt", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 6.0f, " dB"); }},
        {"shape_drive", "Shape Drive", Kind::Float, 0.2f, {}, pct},
        {"shape_harm", "Shape Harmonics", Kind::Float, 0.3f, {}, pct},
        // PITCH
        {"pitch_on", "Pitch On", Kind::Bool, 0.0f},
        {"pitch_semi", "Pitch Semitones", Kind::Float, 0.75f, {}, [](float t) { return fmt("%+.0f st", double(pitchSemitones(t))); }},
        {"pitch_shift", "Pitch Shift", Kind::Float, 0.5f, {}, [](float t) { return fmt("%+.1f Hz", double(shiftHz(t))); }},
        {"pitch_mix", "Pitch Mix", Kind::Float, 0.4f, {}, pct},
        // AIR
        {"air_on", "Air On", Kind::Bool, 0.0f},
        {"air_breath", "Air Breath", Kind::Float, 0.3f, {}, pct},
        {"air_width", "Air Width", Kind::Float, 0.67f, {}, [](float t) { return pct(1.5f * t); }},
        {"air_lofi", "Air Lo-fi", Kind::Float, 0.1f, {}, pct},
        {"air_smooth", "Air Smooth", Kind::Float, 0.5f, {}, [](float t) { return fmt("%.0f ms", double(airSmoothMs(t))); }},
        // GRAIN
        {"grain_on", "Grain On", Kind::Bool, 0.0f},
        {"grain_algo", "Grain Algorithm", Kind::Choice, 1.0f, {"Pulse", "Scatter", "Drift"}},
        {"grain_env", "Grain Envelope", Kind::Choice, 0.0f, {"Sine", "Triangle", "Square", "Perc"}},
        {"grain_sync", "Grain Sync", Kind::Choice, 0.0f, {"Free", "Sync"}},
        {"grain_density", "Grain Density", Kind::Float, 0.45f, {}, [](float t) { return fmt("%.1f /s", double(grainDensity(t))); }},
        {"grain_div", "Grain Division", Kind::Float, 5.0f / 13.0f, {}, [](float t) { return std::string(kDivisionNames[size_t(echoDivision(t))]); }},
        {"grain_chance", "Grain Chance", Kind::Float, 0.7f, {}, pct},
        {"grain_size", "Grain Size", Kind::Float, 0.45f, {}, [](float t) { return fmt("%.0f ms", double(grainSizeMs(t))); }},
        {"grain_mix", "Grain Mix", Kind::Float, 0.7f, {}, pct},
        // Allowed transpositions, in semitones from the sound (GrainCloud::kIntervals, in order).
        {"grain_m12", "Grain -12", Kind::Bool, 0.0f}, {"grain_m7", "Grain -7", Kind::Bool, 0.0f},
        {"grain_m5", "Grain -5", Kind::Bool, 0.0f},   {"grain_m3", "Grain -3", Kind::Bool, 0.0f},
        {"grain_0", "Grain 0", Kind::Bool, 1.0f},
        {"grain_p3", "Grain +3", Kind::Bool, 0.0f},   {"grain_p5", "Grain +5", Kind::Bool, 0.0f},
        {"grain_p7", "Grain +7", Kind::Bool, 1.0f},   {"grain_p12", "Grain +12", Kind::Bool, 1.0f},
        // DEREZ (before SPACE)
        {"derez_on", "Derez On", Kind::Bool, 0.0f},
        {"derez_interval", "Derez Interval", Kind::Float, 0.25f, {}, [](float t) { return fmt("%+.0f st", double(derezSemitones(t))); }},
        {"derez_crush", "Derez Crush", Kind::Float, 0.6f, {}, [](float t) { return "n=" + std::to_string(derezMultiple(t)); }},
        {"derez_bits", "Derez Bits", Kind::Float, 0.55f, {}, [](float t) { return fmt("%.1f", derezBits(t)); }},
        {"derez_mix", "Derez Mix", Kind::Float, 0.7f, {}, pct},
        // STRETCH
        {"stretch_on", "Stretch On", Kind::Bool, 0.0f},
        // STRETCH is BLUR only: MEMORY = how long the pad remembers what was played.
        {"stretch_factor", "Stretch Memory", Kind::Float, 0.3f, {}, [](float t) { return fmt(blurMemory(t) < 10.0f ? "%.1f s" : "%.0f s", double(blurMemory(t))); }},
        {"stretch_mix", "Stretch Mix", Kind::Float, 0.5f, {}, pct},
        // ECHO
        {"echo_on", "Echo On", Kind::Bool, 1.0f},
        {"echo_mode", "Echo Mode", Kind::Choice, 1.0f, {"Mono", "Ping-pong"}},
        {"echo_sync", "Echo Sync", Kind::Choice, 1.0f, {"Free", "Sync"}},
        {"echo_time", "Echo Time", Kind::Float, 0.55f, {}, [](float t) { return fmt("%.0f ms", echoTimeMs(t)); }},
        {"echo_div", "Echo Division", Kind::Float, 9.0f / 13.0f, {}, [](float t) { return std::string(kDivisionNames[size_t(echoDivision(t))]); }},
        {"echo_fdbk", "Echo Feedback", Kind::Float, 0.6f, {}, pct},
        {"echo_mix", "Echo Mix", Kind::Float, 0.35f, {}, pct},
        // SPACE
        {"space_on", "Space On", Kind::Bool, 1.0f},
        {"space_type", "Space Type", Kind::Choice, 3.0f, {"Room", "Plate", "Hall", "Cathedral"}},
        {"space_size", "Space Size", Kind::Float, 0.9f, {}, [](float t) { return fmt("%.2f", t); }},
        {"space_decay", "Space Decay", Kind::Float, 0.75f, {}, [](float t) { return t > 0.98f ? std::string("inf") : fmt("%.1f s", decaySeconds(t)); }},
        {"space_tone", "Space Tone", Kind::Float, 0.45f, {}, pct},
        {"space_mod", "Space Mod", Kind::Float, 0.5f, {}, pct},
        {"space_pre", "Space Predelay", Kind::Float, 0.3f, {}, [](float t) { return fmt("%.0f ms", predelayMs(t)); }},
        {"space_mix", "Space Mix", Kind::Float, 0.6f, {}, pct},
        {"shimmer_depth", "Shimmer Depth", Kind::Float, 0.0f, {}, pct},
        {"shimmer_pitch", "Shimmer Pitch", Kind::Float, 0.75f, {}, [](float t) { return bip(t, 24.0f, " st", "%+.0f"); }},
        {"space_width", "Space Width", Kind::Float, 0.67f, {}, [](float t) { return pct(1.5f * t); }},
        // FOCUS (zero latency, MIDI or AUTO)
        {"focus_on", "Focus On", Kind::Bool, 0.0f},
        {"focus_mode", "Focus Mode", Kind::Choice, 0.0f, {"Key", "Harm", "Auto"}},
        {"focus_amount", "Focus Amount", Kind::Float, 0.6f, {}, pct},
        {"focus_depth", "Focus Depth", Kind::Float, 0.9f, {}, pct},
        {"focus_mix", "Focus Mix", Kind::Float, 1.0f, {}, pct},
        // COLOR + TONAL
        {"color_on", "Color On", Kind::Bool, 1.0f},
        {"color_bass", "Color Bass", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 1.0f, "", "%+.2f"); }},
        {"color_body", "Color Body", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 1.0f, "", "%+.2f"); }},
        {"color_mud", "Color Mud", Kind::Float, 0.5f, {}, [](float t) { return fmt("%.2f", t); }},
        {"color_harsh", "Color Harsh", Kind::Float, 0.3f, {}, [](float t) { return fmt("%.2f", t); }},
        {"color_air", "Color Air", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 1.0f, "", "%+.2f"); }},
        {"color_hpf", "Color Low Cut", Kind::Float, 0.0f, {}, [](float t) { return t <= 0.0f ? std::string("off") : fmt("%.0f Hz", highpassHz(t)); }},
        {"color_lpf", "Color High Cut", Kind::Float, 1.0f, {}, [](float t) { return t >= 1.0f ? std::string("off") : lowpassHz(t) >= 1000.0f ? fmt("%.1f kHz", lowpassHz(t) / 1000.0) : fmt("%.0f Hz", lowpassHz(t)); }},
        {"tonal", "Tonal / Noise", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 1.0f, "", "%+.2f"); }},
        {"tonal_place", "Tonal Place", Kind::Choice, 1.0f, {"Pre space", "Post space"}},
        // MOTION
        {"motion_on", "Motion On", Kind::Bool, 1.0f},
        {"motion_amount", "Motion Amount", Kind::Float, 1.0f, {}, pct},
        {"motion_drift", "Motion Drift", Kind::Float, 0.5f, {}, [](float t) { return fmt("x%.2f", driftScale(t)); }},
    };
    static const std::vector<Spec> withLanes = [] {
        std::vector<Spec> all = specs;
        const std::vector<std::string> shapes(kShapeNames.begin(), kShapeNames.end());
        for (int i = 0; i < int(kLanes.size()); ++i) {
            const Lane& lane = kLanes[size_t(i)];
            const std::string name = "Motion " + std::to_string(i + 1);
            all.push_back({laneId(i, "rate"), name + " Rate", Kind::Float, laneRateT(lane.rateHz), {}, [](float t) {
                               const float hz = laneRateHz(t);
                               return fmt(hz < 0.1f ? "%.3f Hz" : hz < 1.0f ? "%.2f Hz" : "%.1f Hz", hz); }});
            all.push_back({laneId(i, "shape"), name + " Shape", Kind::Choice, float(lane.shape), shapes});
            all.push_back({laneId(i, "depth"), name + " Depth", Kind::Float, 0.0f, {}, pct});
        }
        return all;
    }();
    return withLanes;
}

// Index of each parameter in all(), resolved once.
inline int indexOf(const std::string& id) {
    const auto& specs = all();
    for (size_t i = 0; i < specs.size(); ++i)
        if (specs[i].id == id) return int(i);
    return -1;
}

}  // namespace stasis::params
