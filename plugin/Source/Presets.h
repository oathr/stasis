#pragma once

#include <utility>
#include <vector>

// Factory presets: 60, in three families of 20. Each preset lists only what differs from
// the parameter defaults (Params.h): knobs as 0..1 positions, switches as 0/1, choices as
// indices. `motion` maps MOTION lanes 1, 2, 3… (the others go back to their defaults);
// `order` is the module order (empty = default).
//
// Useful positions (see the conversions in Params.h):
//   echo_time   140 ms .4 · 300 ms .57 · 500 ms .67 · 800 ms .76 · 1.2 s .85 · 2 s .95
//   echo_div / grain_div  index/13: 1/16 2 · 1/8T 4 · 1/8 5 · 1/8. 6 · 1/4T 7 · 1/4 8 · 1/4. 9 · 1/2 11 · 1/1 13
//   space_decay 1.2 s .19 · 3 s .37 · 5 s .48 · 8 s .58 · 12 s .66 · 15 s .71 · 20 s .78 · 25 s .82 · 40 s .92 · ∞ 1
//   space_pre   20 ms .48 · 40 ms .59 · 80 ms .71
//   shimmer_pitch  -12 .25 · +5 .60 · +7 .65 · +12 .75 · +19 .90 · +24 1
//   pitch_semi  -12 .25 · 0 .5 · +7 .646 · +12 .75
//   stretch_factor (MEMORY) 0.5 s .2 · 1 s .3 · 2.5 s .43 · 6 s .57 · 12 s .67 · 37 s .83
//   color_hpf   30 Hz .1 · 60 Hz .28 · 100 Hz .41 · 150 Hz .52      color_lpf  3 kHz .37 · 4 kHz .46 · 6 kHz .6 · 8 kHz .69
//   derez_interval  -24 0 · -12 .25 · -7 .354 · 0 .5 · +7 .646 · +12 .75 · +24 1
//   derez_crush (RATE) / derez_bits  knob up = clean, down = destroyed
//   grain_density  1.2/s .2 · 1.9/s .3 · 3.6/s .45 · 6.9/s .6 · 13/s .75
//   grain_size     50 ms .2 · 80 ms .3 · 160 ms .45 · 320 ms .6 · 630 ms .75 · 1 s .85
//   grain_m12 m7 m5 m3 0 p3 p5 p7 p12  intervals from the sound, in semitones (default: 0, +7, +12)
//
// MOTION depth pulls the knob toward the LFO: value = set + depth · (lfo − set), so .1–.25
// is a gentle drift and .3–.4 a clear movement.
namespace stasis::presets {

enum Shape { Sine, Tri, SawUp, SawDown, Square, SampleHold, Slew, RSine, Drunk, Perlin, Chaos, Breath };

struct Mod {
    const char* target;  // parameter id
    int shape;           // Shape
    float hz;
    float depth;
};

struct Preset {
    const char* category;
    const char* name;
    std::vector<std::pair<const char*, float>> values;
    std::vector<Mod> motion = {};
    const char* order = "";
};

inline const char* const kCategories[] = {"ECHO", "REVERB", "AMBIENT"};

inline const std::vector<Preset>& all() {
    static const std::vector<Preset> list = {
        // ---------------------------------------------------------------- ECHO
        {"ECHO", "Tape Memory", {{"echo_div", 9 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .45f},
                                 {"space_type", 0}, {"space_size", .35f}, {"space_decay", .3f}, {"space_mix", .25f}, {"color_harsh", 0.15f}},
         {{"echo_fdbk", Perlin, .04f, .15f}, {"space_mix", RSine, .03f, .2f}}},
        {"ECHO", "Dub Wash", {{"echo_mode", 0}, {"echo_div", 8 / 13.f}, {"echo_fdbk", .82f}, {"echo_mix", .55f},
                              {"space_type", 2}, {"space_decay", .43f}, {"space_mix", .3f}, {"color_bass", 0.45f}, {"color_lpf", .46f}},
         {{"color_lpf", Sine, .05f, .3f}, {"echo_fdbk", Drunk, .02f, .1f}}},
        {"ECHO", "Broken Cassette", {{"echo_sync", 0}, {"echo_time", .61f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                     {"derez_on", 1}, {"derez_interval", 0.5f}, {"derez_crush", 0.7f}, {"derez_bits", 0.5f}, {"derez_mix", .4f},
                                     {"space_on", 0}, {"color_air", 0.4f}, {"color_lpf", .6f}},
         {{"derez_bits", SampleHold, .5f, .3f}, {"derez_crush", Chaos, .1f, .25f}}},
        {"ECHO", "Ping-Pong Garden", {{"echo_div", 6 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .5f},
                                      {"grain_on", 1}, {"grain_env", 3}, {"grain_density", .35f}, {"grain_size", .35f}, {"grain_mix", .35f},
                                      {"space_type", 1}, {"space_decay", .45f}, {"space_size", .5f}, {"space_mix", .35f}, {"color_air", 0.55f}},
         {{"grain_density", Perlin, .05f, .3f}, {"color_air", RSine, .04f, .2f}}},
        {"ECHO", "Infinite Tape", {{"echo_sync", 0}, {"echo_time", .85f}, {"echo_fdbk", 1}, {"echo_mix", .5f},
                                   {"space_mix", .3f}, {"space_decay", .58f}, {"color_hpf", .41f}, {"color_lpf", .53f}},
         {{"color_lpf", Sine, .02f, .25f}, {"space_mix", Perlin, .03f, .15f}}},
        {"ECHO", "Octave Repeats", {{"echo_div", 8 / 13.f}, {"echo_fdbk", .65f}, {"echo_mix", .5f},
                                    {"derez_on", 1}, {"derez_interval", 0.75f}, {"derez_crush", 0.5f}, {"derez_mix", .5f},
                                    {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f}},
         {{"derez_mix", Tri, .06f, .25f}},
         "shape,pitch,stretch,grain,echo,derez,space,focus,air,color"},
        {"ECHO", "Slow Motion", {{"stretch_on", 1}, {"stretch_factor", .2f}, {"stretch_mix", .5f},
                                 {"echo_div", 11 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                 {"space_type", 0}, {"space_size", .4f}, {"space_decay", .3f}, {"space_mix", .3f}},
         {{"stretch_mix", Breath, .03f, .3f}}},
        {"ECHO", "Shimmer Delay", {{"echo_div", 9 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                   {"space_type", 3}, {"space_decay", .58f}, {"shimmer_depth", .5f}, {"shimmer_pitch", .75f}, {"space_mix", .35f},
                                   {"color_harsh", 0.25f}},
         {{"shimmer_depth", RSine, .03f, .3f}}},
        {"ECHO", "Triplet Rain", {{"echo_div", 4 / 13.f}, {"echo_fdbk", .75f}, {"echo_mix", .4f},
                                  {"grain_on", 1}, {"grain_algo", 0}, {"grain_sync", 1}, {"grain_div", 2 / 13.f}, {"grain_env", 3},
                                  {"grain_chance", .45f}, {"grain_size", .25f}, {"grain_mix", .4f},
                                  {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f},
                                  {"color_harsh", 0.3f}, {"color_air", 0.425f}, {"color_lpf", .6f}},
         {{"grain_chance", Perlin, .04f, .3f}, {"echo_mix", Sine, .05f, .2f}}},
        {"ECHO", "Echoes of the Room", {{"space_type", 2}, {"space_decay", .5f}, {"space_mix", .6f},
                                        {"echo_div", 11 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .5f}},
         {{"space_tone", Perlin, .03f, .25f}},
         "shape,derez,pitch,stretch,grain,space,echo,focus,air,color"},
        {"ECHO", "Grain Bells", {{"grain_on", 1}, {"grain_env", 3}, {"grain_p5", 1},
                                 {"grain_density", .4f}, {"grain_size", .3f}, {"grain_mix", .6f},
                                 {"echo_div", 5 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                 {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f}, {"color_harsh", 0.225f}},
         {{"grain_density", Drunk, .03f, .35f}, {"echo_fdbk", Perlin, .02f, .15f}, {"grain_size", RSine, .07f, .25f}}},
        {"ECHO", "Tape Stutter", {{"grain_on", 1}, {"grain_algo", 0}, {"grain_sync", 1}, {"grain_div", 2 / 13.f}, {"grain_env", 2},
                                  {"grain_p7", 0}, {"grain_p12", 0}, {"grain_size", .2f}, {"grain_chance", .55f}, {"grain_mix", .5f},
                                  {"shape_on", 1}, {"shape_drive", .4f},
                                  {"echo_mode", 0}, {"echo_div", 5 / 13.f}, {"echo_fdbk", .5f}, {"echo_mix", .4f},
                                  {"space_type", 0}, {"space_size", .35f}, {"space_decay", .25f}, {"space_mix", .2f}},
         {{"grain_chance", SampleHold, .5f, .35f}, {"shape_drive", Perlin, .05f, .2f}}},
        {"ECHO", "Warped Reel", {{"echo_sync", 0}, {"echo_time", .67f}, {"echo_fdbk", .65f}, {"echo_mix", .5f},
                                 {"shape_on", 1}, {"shape_drive", .35f}, {"air_on", 1}, {"air_lofi", .4f}, {"air_breath", .2f},
                                 {"space_type", 1}, {"space_decay", .37f}, {"space_mix", .25f}, {"color_lpf", .69f}},
         {{"echo_time", Perlin, .08f, .04f}, {"air_lofi", Drunk, .03f, .25f}}},
        {"ECHO", "Dotted Haze", {{"echo_div", 9 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .45f},
                                 {"space_type", 1}, {"space_decay", .58f}, {"space_mix", .4f}, {"color_harsh", 0.25f}, {"color_mud", 0.3f}},
         {{"space_mix", Sine, .03f, .25f}, {"echo_mix", Tri, .05f, .2f}}},
        {"ECHO", "Slapback Tube", {{"echo_sync", 0}, {"echo_time", .4f}, {"echo_fdbk", .3f}, {"echo_mix", .4f}, {"echo_mode", 0},
                                   {"shape_on", 1}, {"shape_type", 1}, {"shape_drive", .3f},
                                   {"space_type", 0}, {"space_size", .3f}, {"space_decay", .19f}, {"space_mix", .25f}},
         {{"shape_drive", Breath, .06f, .2f}, {"echo_fdbk", Perlin, .04f, .1f}}},
        {"ECHO", "Fifth Echoes", {{"pitch_on", 1}, {"pitch_semi", .646f}, {"pitch_mix", .4f},
                                  {"echo_div", 8 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                  {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .35f}, {"color_harsh", 0.225f}},
         {{"pitch_mix", RSine, .03f, .3f}, {"space_tone", Perlin, .02f, .15f}},
         "shape,derez,stretch,grain,echo,pitch,space,focus,air,color"},
        {"ECHO", "Frequency Drift", {{"pitch_on", 1}, {"pitch_semi", .5f}, {"pitch_shift", .6f}, {"pitch_mix", .5f},
                                     {"echo_div", 11 / 13.f}, {"echo_fdbk", .65f}, {"echo_mix", .45f},
                                     {"space_type", 2}, {"space_decay", .58f}, {"space_mix", .4f}},
         {{"pitch_shift", Perlin, .02f, .1f}, {"echo_mix", Breath, .03f, .2f}},
         "shape,derez,stretch,grain,echo,pitch,space,focus,air,color"},
        {"ECHO", "Granular Delay Cloud", {{"grain_on", 1}, {"grain_env", 1}, {"grain_density", .6f}, {"grain_size", .45f}, {"grain_mix", .7f},
                                          {"echo_div", 6 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .5f},
                                          {"space_type", 3}, {"space_decay", .58f}, {"space_mix", .35f}, {"color_mud", 0.3f}},
         {{"grain_size", Perlin, .03f, .3f}, {"grain_density", Chaos, .05f, .25f}, {"echo_fdbk", Drunk, .02f, .1f}}},
        {"ECHO", "Answering Machine", {{"echo_sync", 0}, {"echo_time", .57f}, {"echo_fdbk", .5f}, {"echo_mix", .5f}, {"echo_mode", 0},
                                       {"air_on", 1}, {"air_lofi", .7f}, {"air_breath", .1f}, {"air_width", .2f},
                                       {"derez_on", 1}, {"derez_interval", .5f}, {"derez_bits", 0.4f}, {"derez_crush", 0.7f}, {"derez_mix", .3f},
                                       {"space_on", 0}, {"color_hpf", .52f}, {"color_lpf", .46f}},
         {{"air_lofi", SampleHold, .3f, .25f}, {"color_lpf", Perlin, .05f, .2f}}},
        {"ECHO", "Long Goodbye", {{"echo_sync", 0}, {"echo_time", .95f}, {"echo_fdbk", .85f}, {"echo_mix", .45f},
                                  {"space_type", 2}, {"space_decay", .78f}, {"space_mix", .45f},
                                  {"color_hpf", .28f}, {"color_lpf", .6f}, {"color_mud", 0.3f}},
         {{"color_lpf", Sine, .01f, .3f}, {"space_decay", Perlin, .02f, .1f}}},

        // -------------------------------------------------------------- REVERB
        {"REVERB", "Cathedral 25", {{"echo_on", 0}, {"space_type", 3}, {"space_size", 1}, {"space_decay", .82f}, {"space_tone", .4f},
                                    {"space_mod", .6f}, {"space_pre", .59f}, {"space_mix", .55f}, {"color_mud", 0.3f}},
         {{"space_mod", Perlin, .03f, .2f}, {"space_tone", RSine, .02f, .2f}}},
        {"REVERB", "Endless Hall", {{"echo_on", 0}, {"space_type", 2}, {"space_size", .85f}, {"space_decay", .92f}, {"space_mix", .55f},
                                    {"color_mud", 0.25f}, {"color_harsh", 0.2f}},
         {{"space_tone", Sine, .02f, .25f}}},
        {"REVERB", "Glass Plate", {{"echo_on", 0}, {"space_type", 1}, {"space_size", .6f}, {"space_decay", .8f}, {"space_tone", .7f},
                                   {"space_mix", .45f}, {"color_air", 0.575f}},
         {{"color_air", Perlin, .04f, .2f}}},
        {"REVERB", "Small Room Halo", {{"echo_on", 0}, {"space_type", 0}, {"space_size", .3f}, {"space_decay", .19f}, {"space_pre", .3f},
                                       {"space_mix", .4f}},
         {{"space_mix", Breath, .05f, .2f}}},
        {"REVERB", "Shimmer Cascade", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .71f}, {"shimmer_depth", .55f}, {"shimmer_pitch", .75f},
                                       {"space_mix", .55f}, {"color_harsh", 0.25f}},
         {{"shimmer_depth", Sine, .025f, .3f}, {"space_tone", Perlin, .03f, .15f}}},
        {"REVERB", "Fifth Heaven", {{"echo_on", 0}, {"space_type", 2}, {"space_decay", .66f}, {"shimmer_depth", .45f}, {"shimmer_pitch", .65f},
                                    {"space_mix", .5f}, {"color_harsh", 0.2f}},
         {{"shimmer_depth", RSine, .02f, .25f}}},
        {"REVERB", "Underworld", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_tone", .25f}, {"shimmer_depth", .5f},
                                  {"shimmer_pitch", .25f}, {"space_mix", .55f}, {"color_body", 0.6f}, {"color_hpf", .1f}},
         {{"space_tone", Perlin, .02f, .2f}, {"color_body", Sine, .03f, .2f}}},
        {"REVERB", "Dark Matter", {{"echo_on", 0}, {"space_type", 2}, {"space_decay", .86f}, {"space_tone", .1f}, {"space_mod", .8f},
                                   {"space_mix", .55f}, {"color_mud", 0.35f}, {"color_lpf", .37f}},
         {{"space_mod", Chaos, .05f, .2f}, {"color_lpf", Perlin, .02f, .15f}}},
        {"REVERB", "Focused Choir", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f},
                                     {"focus_on", 1}, {"focus_mode", 1}, {"focus_amount", .6f}, {"focus_depth", .9f}},
         {{"focus_amount", Perlin, .03f, .25f}}},
        {"REVERB", "Bottomless", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", 1}, {"space_mix", .55f},
                                  {"tonal", .75f}, {"color_mud", 0.3f}},
         {{"tonal", Sine, .01f, .2f}}},
        {"REVERB", "Grain Cathedral", {{"echo_on", 0}, {"grain_on", 1}, {"grain_density", .45f}, {"grain_size", .6f}, {"grain_mix", .5f},
                                       {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"color_mud", 0.3f}},
         {{"grain_density", Perlin, .02f, .3f}, {"grain_mix", Breath, .03f, .25f}}},
        {"REVERB", "Crystal Hall", {{"echo_on", 0}, {"grain_on", 1}, {"grain_env", 3}, {"grain_p5", 1},
                                    {"grain_density", .35f}, {"grain_size", .3f}, {"grain_mix", .45f},
                                    {"space_type", 2}, {"space_decay", .71f}, {"space_tone", .65f}, {"space_mix", .55f},
                                    {"color_air", 0.575f}, {"color_harsh", 0.25f}},
         {{"grain_chance", Drunk, .04f, .3f}, {"space_tone", RSine, .03f, .15f}}},
        {"REVERB", "Shimmer Grains", {{"echo_on", 0}, {"grain_on", 1}, {"grain_env", 1}, {"grain_density", .5f}, {"grain_size", .5f}, {"grain_mix", .5f},
                                      {"space_type", 1}, {"space_decay", .78f}, {"shimmer_depth", .45f}, {"shimmer_pitch", .75f}, {"space_mix", .55f},
                                      {"color_harsh", 0.275f}},
         {{"shimmer_depth", Perlin, .02f, .25f}, {"grain_size", Sine, .04f, .3f}}},
        {"REVERB", "Breathing Room", {{"echo_on", 0}, {"space_type", 0}, {"space_size", .45f}, {"space_decay", .3f}, {"space_mix", .45f}},
         {{"space_mix", Breath, .08f, .25f}, {"space_decay", Sine, .05f, .15f}}},
        {"REVERB", "Velvet Plate", {{"echo_on", 0}, {"space_type", 1}, {"space_size", .5f}, {"space_decay", .58f}, {"space_tone", .35f},
                                    {"space_mix", .5f}, {"color_body", 0.575f}, {"color_mud", 0.3f}},
         {{"space_tone", Perlin, .03f, .2f}, {"space_mod", RSine, .02f, .2f}}},
        {"REVERB", "Ghost Choir", {{"echo_on", 0}, {"focus_on", 1}, {"focus_mode", 1}, {"focus_amount", .7f}, {"focus_depth", .9f},
                                   {"grain_on", 1}, {"grain_m12", 1}, {"grain_density", .4f}, {"grain_size", .7f}, {"grain_mix", .35f},
                                   {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}},
         {{"focus_amount", RSine, .02f, .2f}, {"grain_density", Perlin, .03f, .25f}}},
        {"REVERB", "Dirty Tails", {{"echo_on", 0}, {"shape_on", 1}, {"shape_type", 5}, {"shape_drive", .45f},
                                   {"space_type", 2}, {"space_decay", .71f}, {"space_pre", .71f}, {"space_mix", .6f}, {"color_harsh", 0.25f}},
         {{"shape_drive", Breath, .04f, .25f}, {"space_mix", Perlin, .02f, .15f}},
         "derez,pitch,stretch,grain,echo,space,shape,focus,air,color"},
        {"REVERB", "Halo Infinity", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", 1}, {"space_mix", .55f},
                                     {"shimmer_depth", .3f}, {"shimmer_pitch", .75f}, {"tonal", .7f}, {"color_harsh", 0.25f}},
         {{"shimmer_depth", Sine, .015f, .2f}, {"tonal", Perlin, .02f, .15f}}},
        {"REVERB", "Metal Room", {{"echo_on", 0}, {"shape_on", 1}, {"shape_type", 2}, {"shape_drive", .3f},
                                  {"space_type", 0}, {"space_size", .25f}, {"space_decay", .25f}, {"space_tone", .7f}, {"space_mix", .4f}},
         {{"shape_drive", SampleHold, .2f, .2f}, {"space_tone", Perlin, .04f, .15f}}},
        {"REVERB", "Sub Cavern", {{"echo_on", 0}, {"space_type", 2}, {"space_decay", .78f}, {"space_tone", .2f}, {"space_mix", .55f},
                                  {"shimmer_depth", .35f}, {"shimmer_pitch", .25f},
                                  {"color_bass", 0.575f}, {"color_hpf", .1f}, {"color_lpf", .46f}},
         {{"space_tone", Sine, .02f, .2f}, {"color_lpf", Perlin, .03f, .2f}}},

        // ------------------------------------------------------------- AMBIENT
        {"AMBIENT", "Glacier", {{"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .7f}, {"echo_on", 0},
                                {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"tonal", .8f}},
         {{"stretch_mix", Perlin, .02f, .2f}, {"tonal", Sine, .01f, .1f}}},
        {"AMBIENT", "Frozen Orchard", {{"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .8f},
                                       {"grain_on", 1}, {"grain_density", .3f}, {"grain_size", .65f}, {"grain_mix", .4f},
                                       {"echo_on", 0}, {"space_type", 2}, {"space_decay", .66f}, {"space_mix", .5f}, {"color_air", 0.55f}},
         {{"grain_density", Perlin, .03f, .3f}, {"color_air", RSine, .03f, .2f}}},
        {"AMBIENT", "Dust & Breath", {{"derez_on", 1}, {"derez_interval", 0.5f}, {"derez_crush", 0.3f}, {"derez_bits", 0.29f}, {"derez_mix", .3f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .71f}, {"space_mix", .6f},
                                      {"tonal", .15f}, {"tonal_place", 0}, {"color_harsh", 0.2f}},
         {{"derez_crush", Perlin, .04f, .25f}, {"tonal", Breath, .03f, .15f}}},
        {"AMBIENT", "Tuned Ruins", {{"derez_on", 1}, {"derez_interval", 0.25f}, {"derez_crush", 0.4f}, {"derez_mix", .7f}, {"echo_on", 0},
                                    {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f}},
         {{"derez_mix", Drunk, .02f, .25f}}},
        {"AMBIENT", "Pure Tones", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f},
                                   {"focus_on", 1}, {"focus_mode", 2}, {"focus_depth", .8f}, {"tonal", .95f}},
         {{"focus_depth", Sine, .02f, .15f}}},
        {"AMBIENT", "Monastery Tape", {{"stretch_on", 1}, {"stretch_factor", .3f}, {"stretch_mix", .5f},
                                       {"echo_div", 9 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                       {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .55f}},
         {{"echo_mix", Breath, .03f, .2f}, {"stretch_mix", Perlin, .02f, .2f}}},
        {"AMBIENT", "Sub Tide", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_tone", .3f},
                                 {"shimmer_depth", .6f}, {"shimmer_pitch", .25f}, {"space_mix", .6f},
                                 {"color_bass", 0.575f}, {"color_hpf", .05f}, {"color_lpf", .6f}},
         {{"color_bass", Sine, .02f, .15f}, {"shimmer_depth", Sine, .015f, .2f}}},
        {"AMBIENT", "Northern Lights", {{"echo_div", 6 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .3f},
                                        {"space_type", 2}, {"space_decay", .71f}, {"shimmer_depth", .4f}, {"shimmer_pitch", .9f}, {"space_mix", .55f},
                                        {"color_air", 0.575f}, {"color_harsh", 0.25f}},
         {{"shimmer_depth", Perlin, .02f, .3f}, {"color_air", RSine, .03f, .2f}}},
        {"AMBIENT", "Slow Bloom", {{"stretch_on", 1}, {"stretch_factor", .67f}, {"stretch_mix", .6f},
                                   {"echo_sync", 0}, {"echo_time", .95f}, {"echo_fdbk", .5f}, {"echo_mix", .35f},
                                   {"space_type", 3}, {"space_decay", .92f}, {"space_mix", .7f},
                                   {"color_mud", 0.35f}, {"color_harsh", 0.25f}},
         {{"stretch_mix", Breath, .02f, .25f}, {"space_mix", Perlin, .02f, .15f}}},
        {"AMBIENT", "Infinite Drone", {{"stretch_on", 1}, {"stretch_factor", .83f}, {"stretch_mix", 1},
                                       {"echo_on", 0}, {"space_type", 3}, {"space_decay", 1}, {"space_mix", .6f},
                                       {"tonal", .7f}, {"tonal_place", 0}},
         {{"tonal", Perlin, .01f, .15f}}},
        {"AMBIENT", "Music Box Fog", {{"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .6f},
                                      {"grain_on", 1}, {"grain_env", 3}, {"grain_p3", 1},
                                      {"grain_density", .3f}, {"grain_size", .35f}, {"grain_chance", .6f}, {"grain_mix", .6f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f}, {"color_harsh", 0.225f}},
         {{"grain_density", Drunk, .03f, .35f}, {"grain_chance", Perlin, .05f, .25f}}},
        {"AMBIENT", "Pentatonic Rain", {{"grain_on", 1}, {"grain_algo", 0}, {"grain_sync", 1}, {"grain_div", 5 / 13.f}, {"grain_env", 3},
                                        {"grain_p3", 1}, {"grain_p5", 1},
                                        {"grain_chance", .5f}, {"grain_size", .3f}, {"grain_mix", .6f},
                                        {"echo_div", 8 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .35f},
                                        {"space_type", 2}, {"space_decay", .66f}, {"space_mix", .5f}},
         {{"grain_chance", SampleHold, .25f, .3f}, {"grain_size", Perlin, .05f, .25f}}},
        {"AMBIENT", "Hexagon Field", {{"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .5f},
                                      {"grain_on", 1}, {"grain_algo", 2}, {"grain_m12", 1}, {"grain_m5", 1},
                                      {"grain_density", .5f}, {"grain_size", .55f}, {"grain_mix", .7f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"tonal", .65f}},
         {{"grain_density", Chaos, .03f, .3f}, {"grain_size", Perlin, .02f, .25f}}},
        {"AMBIENT", "Glass Rain", {{"grain_on", 1}, {"grain_env", 3},
                                   {"grain_density", .65f}, {"grain_size", .2f}, {"grain_chance", .6f}, {"grain_mix", .5f},
                                   {"echo_div", 5 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .35f},
                                   {"space_type", 1}, {"space_decay", .66f}, {"space_mix", .5f}, {"color_air", 0.55f}, {"color_harsh", 0.25f}},
         {{"grain_density", Perlin, .06f, .35f}, {"color_air", Sine, .03f, .15f}}},
        {"AMBIENT", "Minor Constellation", {{"stretch_on", 1}, {"stretch_factor", .5f}, {"stretch_mix", .5f},
                                            {"grain_on", 1}, {"grain_env", 1}, {"grain_m5", 1}, {"grain_p3", 1},
                                            {"grain_density", .4f}, {"grain_size", .5f}, {"grain_mix", .55f},
                                            {"echo_on", 0}, {"space_type", 2}, {"space_decay", .78f}, {"space_mix", .6f},
                                            {"shimmer_depth", .3f}, {"shimmer_pitch", .75f}, {"color_harsh", 0.225f}},
         {{"grain_mix", Breath, .03f, .3f}, {"shimmer_depth", Perlin, .02f, .2f}}},
        {"AMBIENT", "Breathing Pad", {{"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .8f},
                                      {"air_on", 1}, {"air_breath", .4f}, {"air_width", .8f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .55f}},
         {{"stretch_mix", Breath, .05f, .25f}, {"air_breath", Perlin, .04f, .3f}}},
        {"AMBIENT", "Cloud Chamber", {{"stretch_on", 1}, {"stretch_factor", .67f}, {"stretch_mix", .6f},
                                      {"grain_on", 1}, {"grain_algo", 2}, {"grain_m12", 1}, {"grain_p7", 0}, {"grain_p12", 0},
                                      {"grain_density", .55f}, {"grain_size", .7f}, {"grain_mix", .8f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .92f}, {"space_mix", .65f},
                                      {"color_mud", 0.325f}, {"color_harsh", 0.25f}},
         {{"grain_size", Perlin, .02f, .3f}, {"space_tone", RSine, .02f, .2f}}},
        {"AMBIENT", "Fourths Drift", {{"grain_on", 1}, {"grain_m5", 1}, {"grain_p5", 1}, {"grain_p7", 0},
                                      {"grain_density", .4f}, {"grain_size", .6f}, {"grain_mix", .5f},
                                      {"echo_on", 0}, {"space_type", 2}, {"space_decay", .82f}, {"space_mix", .6f},
                                      {"shimmer_depth", .3f}, {"shimmer_pitch", .6f}},
         {{"shimmer_depth", Sine, .02f, .25f}, {"grain_density", Drunk, .03f, .3f}}},
        {"AMBIENT", "Lunar Tape", {{"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .5f},
                                   {"echo_sync", 0}, {"echo_time", .9f}, {"echo_fdbk", .6f}, {"echo_mix", .35f},
                                   {"air_on", 1}, {"air_lofi", .35f}, {"air_width", .9f},
                                   {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}},
         {{"air_lofi", Perlin, .02f, .25f}, {"echo_mix", Breath, .02f, .2f}}},
        {"AMBIENT", "Distant Signals", {{"grain_on", 1}, {"grain_env", 2}, {"grain_m12", 1},
                                        {"grain_density", .3f}, {"grain_size", .25f}, {"grain_chance", .5f}, {"grain_mix", .4f},
                                        {"focus_on", 1}, {"focus_mode", 2}, {"focus_depth", .7f},
                                        {"derez_on", 1}, {"derez_interval", .75f}, {"derez_crush", 0.5f}, {"derez_mix", .3f},
                                        {"echo_on", 0}, {"space_type", 2}, {"space_decay", .78f}, {"space_mix", .6f}},
         {{"derez_mix", SampleHold, .15f, .3f}, {"grain_chance", Chaos, .04f, .3f}}},
    };
    return list;
}

}  // namespace stasis::presets
