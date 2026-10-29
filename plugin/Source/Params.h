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
    const char* id;
    const char* name;
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
inline float captureSeconds(float t) { return std::pow(30.0f, t); }
inline float echoTimeMs(float t) { return 20.0f * std::pow(125.0f, t); }
inline int echoDivision(float t) { return int(std::lround(t * 13.0f)); }
inline float decaySeconds(float t) { return t > 0.98f ? INFINITY : 0.5f * std::pow(120.0f, t); }
inline float predelayMs(float t) { return std::pow(500.0f, t); }
inline float derezBits(float t) { return 16.0f - 14.0f * t; }
inline int derezMultiple(float t) { return int(std::lround(32.0f * std::pow(2.0f / 32.0f, t))); }

inline const std::array<const char*, 14> kDivisionNames = {"1/32", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.",
                                                           "1/4T", "1/4",   "1/4.", "1/2T",  "1/2", "1/2.", "1/1"};
inline const std::array<float, 9> kIntervals = {-24, -12, -7, -1, 0, 1, 7, 12, 24};

inline const std::vector<Spec>& all() {
    static const std::vector<Spec> specs = {
        // Global
        {"mix", "Mix", Kind::Float, 0.5f, {}, pct},
        {"out", "Output", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 12.0f, " dB"); }},
        {"freeze", "Freeze", Kind::Bool, 0.0f},
        // DEREZ (before SPACE)
        {"derez_on", "Derez On", Kind::Bool, 0.0f},
        {"derez_interval", "Derez Interval", Kind::Choice, 1.0f, {"-24", "-12", "-7", "-1", "0", "+1", "+7", "+12", "+24"}},
        {"derez_crush", "Derez Crush", Kind::Float, 0.6f, {}, [](float t) { return "n=" + std::to_string(derezMultiple(t)); }},
        {"derez_bits", "Derez Bits", Kind::Float, 0.55f, {}, [](float t) { return fmt("%.1f", derezBits(t)); }},
        {"derez_mix", "Derez Mix", Kind::Float, 0.7f, {}, pct},
        // STRETCH
        {"stretch_on", "Stretch On", Kind::Bool, 0.0f},
        {"stretch_mode", "Stretch Mode", Kind::Choice, 0.0f, {"Smear", "Capture"}},
        {"stretch_factor", "Stretch Factor", Kind::Float, 0.3f, {}, [](float t) { return fmt("x%.0f", stretchFactor(t)); }},
        {"stretch_loop", "Stretch Loop", Kind::Float, 0.4f, {}, [](float t) { return fmt("%.1f s", captureSeconds(t)); }},
        {"stretch_mix", "Stretch Mix", Kind::Float, 0.5f, {}, pct},
        // ECHO
        {"echo_on", "Echo On", Kind::Bool, 1.0f},
        {"echo_mode", "Echo Mode", Kind::Choice, 1.0f, {"Mono", "Ping-pong"}},
        {"echo_sync", "Echo Sync", Kind::Choice, 1.0f, {"Free", "Sync"}},
        {"echo_time", "Echo Time", Kind::Float, 0.55f, {}, [](float t) { return fmt("%.0f ms", echoTimeMs(t)); }},
        {"echo_div", "Echo Division", Kind::Float, 9.0f / 13.0f, {}, [](float t) { return std::string(kDivisionNames[size_t(echoDivision(t))]); }},
        {"echo_fdbk", "Echo Feedback", Kind::Float, 0.6f, {}, pct},
        {"echo_wow", "Echo Wow", Kind::Float, 0.4f, {}, pct},
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
        {"color_mud", "Color Mud", Kind::Float, 0.5f, {}, [](float t) { return fmt("%.2f", t); }},
        {"color_air", "Color Air", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 1.0f, "", "%+.2f"); }},
        {"tonal", "Tonal / Noise", Kind::Float, 0.5f, {}, [](float t) { return bip(t, 1.0f, "", "%+.2f"); }},
        {"tonal_place", "Tonal Place", Kind::Choice, 1.0f, {"Pre space", "Post space"}},
    };
    return specs;
}

// Index of each parameter in all(), resolved once.
inline int indexOf(const char* id) {
    const auto& specs = all();
    for (size_t i = 0; i < specs.size(); ++i)
        if (std::string(specs[i].id) == id) return int(i);
    return -1;
}

}  // namespace stasis::params
