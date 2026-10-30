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
//   color_hpf   30 Hz .073 · 60 Hz .199 · 100 Hz .291 · 150 Hz .365 · 1 kHz .708 · 5 kHz 1
//   color_lpf   50 Hz 0 · 500 Hz .5 · 3 kHz .683 · 4 kHz .73 · 6 kHz .8 · 8 kHz .845 · 1 = off
//   derez_interval  -24 0 · -12 .25 · -7 .354 · 0 .5 · +7 .646 · +12 .75 · +24 1
//   derez_crush (RATE) / derez_bits  knob up = clean, down = destroyed
//   grain_density  1.2/s .2 · 1.9/s .3 · 3.6/s .45 · 6.9/s .6 · 13/s .75
//   grain_size     50 ms .2 · 80 ms .3 · 160 ms .45 · 320 ms .6 · 630 ms .75 · 1 s .85
//   grain_m12 m7 m5 m3 0 p3 p5 p7 p12  intervals from the sound, in semitones (default: 0, +7, +12)
//   color_bass … color_air  .5 = flat; BASS / AIR ±24 dB, BODY / MIDS (color_mud) / PRESENCE (color_harsh) ±18 dB
//
// MOTION swings the knob around its own position: value = set ± depth (held in 0..1), so
// .1–.2 is a gentle drift, .3–.5 a clear movement. OFFSET (.3 ≈ 2 ms) widens; an LFO on it wavers like tape.
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

// The presets were written when these knobs started elsewhere (before the defaults became
// subtle): every preset starts from these values, then its own.
inline const std::vector<std::pair<const char*, float>> kBase = {
    {"echo_on", 1.0f},  // ECHO was on in a new instance
    {"shape_drive", 0.2f}, {"shape_harm", 0.3f}, {"pitch_mix", 0.4f}, {"air_breath", 0.3f}, {"air_lofi", 0.1f}, {"grain_mix", 0.7f}, {"derez_mix", 0.7f}, {"derez_crush", 0.4f}, {"derez_bits", 0.45f}, {"stretch_mix", 0.5f}, {"echo_fdbk", 0.6f}, {"echo_mix", 0.35f}, {"space_type", 3.0f}, {"space_size", 0.9f}, {"space_decay", 0.75f}, {"space_mix", 0.6f}, {"focus_amount", 0.6f}, {"focus_depth", 0.9f}};

inline const std::vector<Preset>& all() {
    static const std::vector<Preset> list = {
        // ---------------------------------------------------------------- ECHO
        {"ECHO", "Tape Memory", {{"offset", .3f}, {"echo_div", 9 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .45f},
                                 {"space_type", 0}, {"space_size", .35f}, {"space_decay", .3f}, {"space_mix", .25f}, {"color_harsh", 0.462f}},
         {{"echo_fdbk", Perlin, .04f, 0.24f}, {"space_mix", RSine, .03f, 0.32f}, {"echo_mix", Breath, .05f, .3f}}},
        {"ECHO", "Dub Wash", {{"offset", .35f}, {"echo_mode", 0}, {"echo_div", 8 / 13.f}, {"echo_fdbk", .82f}, {"echo_mix", .55f},
                              {"space_type", 2}, {"space_decay", .43f}, {"space_mix", .3f}, {"color_bass", 0.462f}, {"color_lpf", .73f}},
         {{"color_lpf", Sine, .05f, .24f}, {"echo_fdbk", Drunk, .02f, 0.16f}, {"space_mix", Perlin, .03f, .35f}}},
        // OFFSET moved by MOTION: the right channel's delay wanders, so its pitch wavers
        // against the left (wow); lane 2 moves lane 1's RATE, so the wow never repeats: a worn cassette deck.
        {"ECHO", "Cassette", {{"offset", .45f}, {"echo_sync", 0}, {"echo_time", .61f}, {"echo_fdbk", .5f}, {"echo_mix", .4f},
                              {"shape_on", 1}, {"shape_drive", .35f}, {"air_on", 1}, {"air_lofi", .35f}, {"air_breath", .1f},
                              {"space_type", 0}, {"space_size", .3f}, {"space_decay", .25f}, {"space_mix", .2f},
                              {"color_body", .6f}, {"color_air", .4f}, {"color_lpf", .8f}},
         {{"offset", Sine, .4f, .15f}, {"m01_rate", Perlin, 3.0f, .03f}, {"air_lofi", Drunk, .05f, .3f}, {"echo_time", Perlin, .4f, .02f}}},
        {"ECHO", "Ping-Pong Garden", {{"offset", .25f}, {"echo_div", 6 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .5f},
                                      {"grain_on", 1}, {"grain_env", 3}, {"grain_density", .35f}, {"grain_size", .35f}, {"grain_mix", .35f},
                                      {"space_type", 1}, {"space_decay", .386f}, {"space_size", .5f}, {"space_mix", .35f}, {"color_air", 0.533f}},
         {{"grain_density", Perlin, .05f, 0.48f}, {"color_air", RSine, .04f, 0.32f}, {"echo_fdbk", Perlin, .03f, .2f}}},
        {"ECHO", "Infinite Tape", {{"offset", .2f}, {"echo_sync", 0}, {"echo_time", .85f}, {"echo_fdbk", .9f}, {"echo_mix", .5f},
                                   {"space_mix", .3f}, {"space_decay", .58f}, {"color_hpf", .29f}, {"color_lpf", .765f}},
         {{"color_lpf", Sine, .02f, .2f}, {"space_mix", Perlin, .03f, 0.24f}, {"offset", RSine, .05f, .3f}}},
        {"ECHO", "Octave Repeats", {{"offset", .3f}, {"echo_div", 8 / 13.f}, {"echo_fdbk", .65f}, {"echo_mix", .5f},
                                    {"derez_on", 1}, {"derez_interval", 0.75f}, {"derez_crush", 0.5f}, {"derez_mix", .5f},
                                    {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f}},
         {{"derez_mix", Tri, .06f, 0.4f}, {"echo_fdbk", Sine, .04f, .2f}},
         "shape,pitch,stretch,grain,echo,derez,space,focus,air,color"},
        {"ECHO", "Slow Motion", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .2f}, {"stretch_mix", .5f},
                                 {"echo_div", 11 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                 {"space_type", 0}, {"space_size", .4f}, {"space_decay", .3f}, {"space_mix", .3f}},
         {{"stretch_mix", Breath, .03f, 0.48f}, {"offset", Sine, .03f, .3f}, {"space_mix", Perlin, .03f, .3f}}},
        {"ECHO", "Shimmer Delay", {{"offset", .35f}, {"echo_div", 9 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                   {"space_type", 3}, {"space_decay", .58f}, {"shimmer_depth", .5f}, {"shimmer_pitch", .75f}, {"space_mix", .35f},
                                   {"color_harsh", 0.438f}},
         {{"shimmer_depth", RSine, .03f, 0.48f}, {"echo_mix", Perlin, .04f, .3f}}},
        {"ECHO", "Triplet Rain", {{"offset", .3f}, {"echo_div", 4 / 13.f}, {"echo_fdbk", .75f}, {"echo_mix", .4f},
                                  {"grain_on", 1}, {"grain_algo", 0}, {"grain_sync", 1}, {"grain_div", 2 / 13.f}, {"grain_env", 3},
                                  {"grain_chance", .45f}, {"grain_size", .25f}, {"grain_mix", .4f},
                                  {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f},
                                  {"color_harsh", 0.425f}, {"color_air", 0.45f}, {"color_lpf", .8f}},
         {{"grain_chance", Perlin, .04f, 0.48f}, {"echo_mix", Sine, .05f, 0.32f}, {"space_tone", Perlin, .03f, .25f}}},
        {"ECHO", "Echoes of the Room", {{"offset", .25f}, {"space_type", 2}, {"space_decay", .5f}, {"space_mix", .6f},
                                        {"echo_div", 11 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .5f}},
         {{"space_tone", Perlin, .03f, 0.4f}, {"echo_fdbk", Drunk, .03f, .2f}},
         "shape,derez,pitch,stretch,grain,space,echo,focus,air,color"},
        {"ECHO", "Grain Bells", {{"offset", .3f}, {"grain_on", 1}, {"grain_env", 3}, {"grain_p5", 1},
                                 {"grain_density", .4f}, {"grain_size", .3f}, {"grain_mix", .6f},
                                 {"echo_div", 5 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                 {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f}, {"color_harsh", 0.444f}},
         {{"grain_density", Drunk, .03f, 0.5f}, {"echo_fdbk", Perlin, .02f, 0.24f}, {"grain_size", RSine, .07f, 0.4f}}},
        {"ECHO", "Tape Stutter", {{"offset", .2f}, {"grain_on", 1}, {"grain_algo", 0}, {"grain_sync", 1}, {"grain_div", 2 / 13.f}, {"grain_env", 2},
                                  {"grain_p7", 0}, {"grain_p12", 0}, {"grain_size", .2f}, {"grain_chance", .55f}, {"grain_mix", .5f},
                                  {"shape_on", 1}, {"shape_drive", .4f},
                                  {"echo_mode", 0}, {"echo_div", 5 / 13.f}, {"echo_fdbk", .5f}, {"echo_mix", .4f},
                                  {"space_type", 0}, {"space_size", .35f}, {"space_decay", .25f}, {"space_mix", .2f}},
         {{"grain_chance", SampleHold, .5f, 0.5f}, {"shape_drive", Perlin, .05f, 0.32f}, {"offset", SampleHold, .5f, .25f}}},
        {"ECHO", "Warped Reel", {{"offset", .3f}, {"echo_sync", 0}, {"echo_time", .67f}, {"echo_fdbk", .65f}, {"echo_mix", .5f},
                                 {"shape_on", 1}, {"shape_drive", .35f}, {"air_on", 1}, {"air_lofi", .4f}, {"air_breath", .2f},
                                 {"space_type", 1}, {"space_decay", .431f}, {"space_mix", .25f}, {"color_lpf", .845f}},
         {{"echo_time", Perlin, .08f, 0.03f}, {"air_lofi", Drunk, .03f, 0.4f}, {"offset", Perlin, .3f, .2f}}},
        {"ECHO", "Dotted Haze", {{"offset", .4f}, {"echo_div", 9 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .45f},
                                 {"space_type", 1}, {"space_decay", .536f}, {"space_mix", .4f}, {"color_harsh", 0.438f}, {"color_mud", 0.425f}},
         {{"space_mix", Sine, .03f, 0.4f}, {"echo_mix", Tri, .05f, 0.32f}, {"color_lpf", Perlin, .03f, .15f}}},
        {"ECHO", "Slapback Tube", {{"offset", .45f}, {"color_trem", .3f}, {"color_trem_rate", .5f}, {"echo_sync", 0}, {"echo_time", .4f}, {"echo_fdbk", .3f}, {"echo_mix", .4f}, {"echo_mode", 0},
                                   {"shape_on", 1}, {"shape_type", 1}, {"shape_drive", .3f},
                                   {"space_type", 0}, {"space_size", .3f}, {"space_decay", .19f}, {"space_mix", .25f}},
         {{"shape_drive", Breath, .06f, 0.32f}, {"echo_fdbk", Perlin, .04f, 0.16f}, {"color_trem", Perlin, .05f, .2f}}},
        {"ECHO", "Fifth Echoes", {{"offset", .3f}, {"pitch_on", 1}, {"pitch_semi", .646f}, {"pitch_mix", .4f},
                                  {"echo_div", 8 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                  {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .35f}, {"color_harsh", 0.444f}},
         {{"pitch_mix", RSine, .03f, 0.48f}, {"space_tone", Perlin, .02f, 0.24f}, {"echo_fdbk", Perlin, .03f, .2f}},
         "shape,derez,stretch,grain,echo,pitch,space,focus,air,color"},
        {"ECHO", "Frequency Drift", {{"offset", .25f}, {"pitch_on", 1}, {"pitch_semi", .5f}, {"pitch_shift", .6f}, {"pitch_mix", .5f},
                                     {"echo_div", 11 / 13.f}, {"echo_fdbk", .65f}, {"echo_mix", .45f},
                                     {"space_type", 2}, {"space_decay", .58f}, {"space_mix", .4f}},
         {{"pitch_shift", Perlin, .02f, 0.16f}, {"echo_mix", Breath, .03f, 0.32f}, {"offset", Sine, .07f, .3f}},
         "shape,derez,stretch,grain,echo,pitch,space,focus,air,color"},
        {"ECHO", "Granular Delay Cloud", {{"offset", .3f}, {"grain_on", 1}, {"grain_env", 1}, {"grain_density", .6f}, {"grain_size", .45f}, {"grain_mix", .7f},
                                          {"echo_div", 6 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .5f},
                                          {"space_type", 3}, {"space_decay", .58f}, {"space_mix", .35f}, {"color_mud", 0.425f}},
         {{"grain_size", Perlin, .03f, 0.48f}, {"grain_density", Chaos, .05f, 0.4f}, {"echo_fdbk", Drunk, .02f, 0.16f}}},
        {"ECHO", "Answering Machine", {{"echo_sync", 0}, {"echo_time", .57f}, {"echo_fdbk", .5f}, {"echo_mix", .5f}, {"echo_mode", 0},
                                       {"air_on", 1}, {"air_lofi", .7f}, {"air_breath", .1f}, {"air_width", .2f},
                                       {"derez_on", 1}, {"derez_interval", .5f}, {"derez_bits", 0.4f}, {"derez_crush", 0.7f}, {"derez_mix", .3f},
                                       {"space_on", 0}, {"color_hpf", .368f}, {"color_lpf", .73f}},
         {{"air_lofi", SampleHold, .3f, 0.4f}, {"color_lpf", Perlin, .05f, .16f}, {"air_breath", Breath, .1f, .3f}}},
        {"ECHO", "Long Goodbye", {{"offset", .35f}, {"echo_sync", 0}, {"echo_time", .95f}, {"echo_fdbk", .85f}, {"echo_mix", .45f},
                                  {"space_type", 2}, {"space_decay", .78f}, {"space_mix", .45f},
                                  {"color_hpf", .198f}, {"color_lpf", .8f}, {"color_mud", 0.425f}},
         {{"color_lpf", Sine, .01f, .24f}, {"space_decay", Perlin, .02f, 0.16f}, {"echo_fdbk", Sine, .015f, .15f}}},

        // -------------------------------------------------------------- REVERB
        {"REVERB", "Cathedral 25", {{"offset", .25f}, {"echo_on", 0}, {"space_type", 3}, {"space_size", 1}, {"space_decay", .82f}, {"space_tone", .4f},
                                    {"space_mod", .6f}, {"space_pre", .59f}, {"space_mix", .55f}, {"color_mud", 0.425f}},
         {{"space_mod", Perlin, .03f, 0.32f}, {"space_tone", RSine, .02f, 0.32f}, {"space_mix", Breath, .02f, .3f}}},
        {"REVERB", "Endless Hall", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 2}, {"space_size", .85f}, {"space_decay", .92f}, {"space_mix", .55f},
                                    {"color_mud", 0.438f}, {"color_harsh", 0.45f}},
         {{"space_tone", Sine, .02f, 0.4f}, {"space_mod", Perlin, .03f, .35f}}},
        {"REVERB", "Glass Plate", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 1}, {"space_size", .6f}, {"space_decay", .645f}, {"space_tone", .7f},
                                   {"space_mix", .45f}, {"color_air", 0.55f}},
         {{"color_air", Perlin, .04f, 0.32f}, {"space_decay", Sine, .02f, .15f}}},
        {"REVERB", "Small Room Halo", {{"offset", .45f}, {"echo_on", 0}, {"space_type", 0}, {"space_size", .3f}, {"space_decay", .19f}, {"space_pre", .3f},
                                       {"space_mix", .4f}},
         {{"space_mix", Breath, .05f, 0.32f}, {"offset", RSine, .04f, .3f}}},
        {"REVERB", "Shimmer Cascade", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 3}, {"space_decay", .71f}, {"shimmer_depth", .55f}, {"shimmer_pitch", .75f},
                                       {"space_mix", .55f}, {"color_harsh", 0.438f}},
         {{"shimmer_depth", Sine, .025f, 0.48f}, {"space_tone", Perlin, .03f, 0.24f}}},
        {"REVERB", "Fifth Heaven", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 2}, {"space_decay", .66f}, {"shimmer_depth", .45f}, {"shimmer_pitch", .65f},
                                    {"space_mix", .5f}, {"color_harsh", 0.45f}},
         {{"shimmer_depth", RSine, .02f, 0.4f}, {"space_tone", Perlin, .03f, .3f}}},
        {"REVERB", "Underworld", {{"offset", .2f}, {"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_tone", .25f}, {"shimmer_depth", .5f},
                                  {"shimmer_pitch", .25f}, {"space_mix", .55f}, {"color_body", 0.567f}, {"color_hpf", .071f}},
         {{"space_tone", Perlin, .02f, 0.32f}, {"color_body", Sine, .03f, 0.32f}}},
        {"REVERB", "Dark Matter", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 2}, {"space_decay", .86f}, {"space_tone", .1f}, {"space_mod", .8f},
                                   {"space_mix", .55f}, {"color_mud", 0.412f}, {"color_lpf", .685f}},
         {{"space_mod", Chaos, .05f, 0.32f}, {"color_lpf", Perlin, .02f, .12f}}},
        {"REVERB", "Focused Choir", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f},
                                     {"focus_on", 1}, {"focus_mode", 1}, {"focus_amount", .6f}, {"focus_depth", .9f}},
         {{"focus_amount", Perlin, .03f, 0.4f}, {"space_mix", Breath, .03f, .3f}}},
        {"REVERB", "Bottomless", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 3}, {"space_decay", .97f}, {"space_mix", .55f},
                                  {"tonal", .75f}, {"color_mud", 0.425f}},
         {{"tonal", Sine, .01f, 0.32f}, {"space_mod", Perlin, .02f, .35f}}},
        {"REVERB", "Grain Cathedral", {{"offset", .3f}, {"echo_on", 0}, {"grain_on", 1}, {"grain_density", .45f}, {"grain_size", .6f}, {"grain_mix", .5f},
                                       {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"color_mud", 0.425f}},
         {{"grain_density", Perlin, .02f, 0.48f}, {"grain_mix", Breath, .03f, 0.4f}, {"offset", Perlin, .02f, .3f}}},
        {"REVERB", "Crystal Hall", {{"offset", .35f}, {"echo_on", 0}, {"grain_on", 1}, {"grain_env", 3}, {"grain_p5", 1},
                                    {"grain_density", .35f}, {"grain_size", .3f}, {"grain_mix", .45f},
                                    {"space_type", 2}, {"space_decay", .71f}, {"space_tone", .65f}, {"space_mix", .55f},
                                    {"color_air", 0.55f}, {"color_harsh", 0.438f}},
         {{"grain_chance", Drunk, .04f, 0.48f}, {"space_tone", RSine, .03f, 0.24f}}},
        {"REVERB", "Shimmer Grains", {{"offset", .3f}, {"echo_on", 0}, {"grain_on", 1}, {"grain_env", 1}, {"grain_density", .5f}, {"grain_size", .5f}, {"grain_mix", .5f},
                                      {"space_type", 1}, {"space_decay", .683f}, {"shimmer_depth", .45f}, {"shimmer_pitch", .75f}, {"space_mix", .55f},
                                      {"color_harsh", 0.431f}},
         {{"shimmer_depth", Perlin, .02f, 0.4f}, {"grain_size", Sine, .04f, 0.48f}}},
        {"REVERB", "Breathing Room", {{"offset", .4f}, {"echo_on", 0}, {"space_type", 0}, {"space_size", .45f}, {"space_decay", .3f}, {"space_mix", .45f}},
         {{"space_mix", Breath, .08f, 0.4f}, {"space_decay", Sine, .05f, 0.24f}}},
        {"REVERB", "Velvet Plate", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 1}, {"space_size", .5f}, {"space_decay", .454f}, {"space_tone", .35f},
                                    {"space_mix", .5f}, {"color_body", 0.55f}, {"color_mud", 0.425f}},
         {{"space_tone", Perlin, .03f, 0.32f}, {"space_mod", RSine, .02f, 0.32f}}},
        {"REVERB", "Ghost Choir", {{"offset", .3f}, {"echo_on", 0}, {"focus_on", 1}, {"focus_mode", 1}, {"focus_amount", .7f}, {"focus_depth", .9f},
                                   {"grain_on", 1}, {"grain_m12", 1}, {"grain_density", .4f}, {"grain_size", .7f}, {"grain_mix", .35f},
                                   {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}},
         {{"focus_amount", RSine, .02f, 0.32f}, {"grain_density", Perlin, .03f, 0.4f}, {"space_tone", Perlin, .02f, .3f}}},
        {"REVERB", "Dirty Tails", {{"offset", .25f}, {"echo_on", 0}, {"shape_on", 1}, {"shape_type", 5}, {"shape_drive", .45f},
                                   {"space_type", 2}, {"space_decay", .71f}, {"space_pre", .71f}, {"space_mix", .6f}, {"color_harsh", 0.438f}},
         {{"shape_drive", Breath, .04f, 0.4f}, {"space_mix", Perlin, .02f, 0.24f}},
         "derez,pitch,stretch,grain,echo,space,shape,focus,air,color"},
        {"REVERB", "Halo Infinity", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 3}, {"space_decay", .97f}, {"space_mix", .55f},
                                     {"shimmer_depth", .3f}, {"shimmer_pitch", .75f}, {"tonal", .7f}, {"color_harsh", 0.438f}},
         {{"shimmer_depth", Sine, .015f, 0.32f}, {"tonal", Perlin, .02f, 0.24f}}},
        // BREATH shaken by a 20 Hz sine: the breath turns into a hiss that rattles, like a snake.
        {"REVERB", "Serpent", {{"offset", .25f}, {"echo_on", 0}, {"air_on", 1}, {"air_breath", .5f}, {"air_width", .7f},
                               {"space_type", 1}, {"space_size", .5f}, {"space_decay", .5f}, {"space_tone", .6f}, {"space_mix", .4f},
                               {"color_harsh", .54f}},
         {{"air_breath", Sine, 20.0f, .5f}, {"space_tone", Perlin, .04f, .24f}, {"offset", Sine, .3f, .1f}}},
        {"REVERB", "Sub Cavern", {{"offset", .2f}, {"echo_on", 0}, {"space_type", 2}, {"space_decay", .78f}, {"space_tone", .2f}, {"space_mix", .55f},
                                  {"shimmer_depth", .35f}, {"shimmer_pitch", .25f},
                                  {"color_bass", 0.556f}, {"color_hpf", .071f}, {"color_lpf", .73f}},
         {{"space_tone", Sine, .02f, 0.32f}, {"color_lpf", Perlin, .03f, .16f}}},

        // ------------------------------------------------------------- AMBIENT
        {"AMBIENT", "Glacier", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .7f}, {"echo_on", 0},
                                {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"tonal", .8f}},
         {{"stretch_mix", Perlin, .02f, 0.32f}, {"tonal", Sine, .01f, 0.16f}, {"space_mix", Perlin, .02f, .3f}}},
        {"AMBIENT", "Frozen Orchard", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .8f},
                                       {"grain_on", 1}, {"grain_density", .3f}, {"grain_size", .65f}, {"grain_mix", .4f},
                                       {"echo_on", 0}, {"space_type", 2}, {"space_decay", .66f}, {"space_mix", .5f}, {"color_air", 0.533f}},
         {{"grain_density", Perlin, .03f, 0.48f}, {"color_air", RSine, .03f, 0.32f}, {"stretch_mix", Breath, .03f, .35f}}},
        {"AMBIENT", "Dust & Breath", {{"offset", .3f}, {"derez_on", 1}, {"derez_interval", 0.5f}, {"derez_crush", 0.3f}, {"derez_bits", 0.29f}, {"derez_mix", .3f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .71f}, {"space_mix", .6f},
                                      {"tonal", .15f}, {"tonal_place", 0}, {"color_harsh", 0.45f}},
         {{"derez_crush", Perlin, .04f, 0.4f}, {"tonal", Breath, .03f, 0.24f}}},
        {"AMBIENT", "Tuned Ruins", {{"offset", .3f}, {"derez_on", 1}, {"derez_interval", 0.25f}, {"derez_crush", 0.4f}, {"derez_mix", .7f}, {"echo_on", 0},
                                    {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f}},
         {{"derez_mix", Drunk, .02f, 0.4f}, {"space_tone", Perlin, .03f, .35f}}},
        {"AMBIENT", "Pure Tones", {{"offset", .3f}, {"echo_on", 0}, {"space_type", 5}, {"space_decay", .82f}, {"space_mix", .6f},
                                   {"focus_on", 1}, {"focus_mode", 2}, {"focus_depth", .8f}, {"tonal", .95f}},
         {{"focus_depth", Sine, .02f, 0.24f}, {"tonal", Perlin, .02f, .2f}}},
        {"AMBIENT", "Monastery Tape", {{"offset", .35f}, {"stretch_on", 1}, {"stretch_factor", .3f}, {"stretch_mix", .5f},
                                       {"echo_div", 9 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                       {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .55f}},
         {{"echo_mix", Breath, .03f, 0.32f}, {"stretch_mix", Perlin, .02f, 0.32f}, {"offset", Sine, .25f, .15f}}},
        {"AMBIENT", "Sub Tide", {{"offset", .2f}, {"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_tone", .3f},
                                 {"shimmer_depth", .6f}, {"shimmer_pitch", .25f}, {"space_mix", .6f},
                                 {"color_bass", 0.556f}, {"color_hpf", .035f}, {"color_lpf", .8f}},
         {{"color_bass", Sine, .02f, 0.24f}, {"shimmer_depth", Sine, .015f, 0.32f}}},
        {"AMBIENT", "Northern Lights", {{"offset", .35f}, {"echo_div", 6 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .3f},
                                        {"space_type", 2}, {"space_decay", .71f}, {"shimmer_depth", .4f}, {"shimmer_pitch", .9f}, {"space_mix", .55f},
                                        {"color_air", 0.55f}, {"color_harsh", 0.438f}},
         {{"shimmer_depth", Perlin, .02f, 0.48f}, {"color_air", RSine, .03f, 0.32f}, {"offset", RSine, .03f, .3f}}},
        {"AMBIENT", "Slow Bloom", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .67f}, {"stretch_mix", .6f},
                                   {"echo_sync", 0}, {"echo_time", .95f}, {"echo_fdbk", .5f}, {"echo_mix", .35f},
                                   {"space_type", 4}, {"space_decay", .92f}, {"space_mix", .7f},
                                   {"color_mud", 0.412f}, {"color_harsh", 0.438f}},
         {{"stretch_mix", Breath, .02f, 0.4f}, {"space_mix", Perlin, .02f, 0.24f}}},
        {"AMBIENT", "Infinite Drone", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .83f}, {"stretch_mix", 1},
                                       {"echo_on", 0}, {"space_type", 3}, {"space_decay", .97f}, {"space_mix", .6f},
                                       {"tonal", .7f}, {"tonal_place", 0}},
         {{"tonal", Perlin, .01f, 0.24f}, {"stretch_mix", Perlin, .02f, .3f}}},
        {"AMBIENT", "Music Box Fog", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .6f},
                                      {"grain_on", 1}, {"grain_env", 3}, {"grain_p3", 1},
                                      {"grain_density", .3f}, {"grain_size", .35f}, {"grain_chance", .6f}, {"grain_mix", .6f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f}, {"color_harsh", 0.444f}},
         {{"grain_density", Drunk, .03f, 0.5f}, {"grain_chance", Perlin, .05f, 0.4f}}},
        {"AMBIENT", "Pentatonic Rain", {{"offset", .35f}, {"grain_on", 1}, {"grain_algo", 0}, {"grain_sync", 1}, {"grain_div", 5 / 13.f}, {"grain_env", 3},
                                        {"grain_p3", 1}, {"grain_p5", 1},
                                        {"grain_chance", .5f}, {"grain_size", .3f}, {"grain_mix", .6f},
                                        {"echo_div", 8 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .35f},
                                        {"space_type", 2}, {"space_decay", .66f}, {"space_mix", .5f}},
         {{"grain_chance", SampleHold, .25f, 0.48f}, {"grain_size", Perlin, .05f, 0.4f}}},
        {"AMBIENT", "Hexagon Field", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .5f},
                                      {"grain_on", 1}, {"grain_algo", 2}, {"grain_m12", 1}, {"grain_m5", 1},
                                      {"grain_density", .5f}, {"grain_size", .55f}, {"grain_mix", .7f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"tonal", .65f}},
         {{"grain_density", Chaos, .03f, 0.48f}, {"grain_size", Perlin, .02f, 0.4f}}},
        {"AMBIENT", "Glass Rain", {{"offset", .4f}, {"grain_on", 1}, {"grain_env", 3},
                                   {"grain_density", .65f}, {"grain_size", .2f}, {"grain_chance", .6f}, {"grain_mix", .5f},
                                   {"echo_div", 5 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .35f},
                                   {"space_type", 1}, {"space_decay", .586f}, {"space_mix", .5f}, {"color_air", 0.533f}, {"color_harsh", 0.438f}},
         {{"grain_density", Perlin, .06f, 0.5f}, {"color_air", Sine, .03f, 0.24f}, {"offset", SampleHold, .2f, .25f}}},
        {"AMBIENT", "Minor Constellation", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .5f}, {"stretch_mix", .5f},
                                            {"grain_on", 1}, {"grain_env", 1}, {"grain_m5", 1}, {"grain_p3", 1},
                                            {"grain_density", .4f}, {"grain_size", .5f}, {"grain_mix", .55f},
                                            {"echo_on", 0}, {"space_type", 2}, {"space_decay", .78f}, {"space_mix", .6f},
                                            {"shimmer_depth", .3f}, {"shimmer_pitch", .75f}, {"color_harsh", 0.444f}},
         {{"grain_mix", Breath, .03f, 0.48f}, {"shimmer_depth", Perlin, .02f, 0.32f}}},
        {"AMBIENT", "Breathing Pad", {{"offset", .35f}, {"color_trem", .2f}, {"color_trem_rate", .15f}, {"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .8f},
                                      {"air_on", 1}, {"air_breath", .4f}, {"air_width", .8f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .55f}},
         {{"stretch_mix", Breath, .05f, 0.4f}, {"air_breath", Perlin, .04f, 0.48f}, {"color_trem", Breath, .03f, .2f}}},
        {"AMBIENT", "Cloud Chamber", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .67f}, {"stretch_mix", .6f},
                                      {"grain_on", 1}, {"grain_algo", 2}, {"grain_m12", 1}, {"grain_p7", 0}, {"grain_p12", 0},
                                      {"grain_density", .55f}, {"grain_size", .7f}, {"grain_mix", .8f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .92f}, {"space_mix", .65f},
                                      {"color_mud", 0.419f}, {"color_harsh", 0.438f}},
         {{"grain_size", Perlin, .02f, 0.48f}, {"space_tone", RSine, .02f, 0.32f}}},
        {"AMBIENT", "Fourths Drift", {{"offset", .3f}, {"grain_on", 1}, {"grain_m5", 1}, {"grain_p5", 1}, {"grain_p7", 0},
                                      {"grain_density", .4f}, {"grain_size", .6f}, {"grain_mix", .5f},
                                      {"echo_on", 0}, {"space_type", 2}, {"space_decay", .82f}, {"space_mix", .6f},
                                      {"shimmer_depth", .3f}, {"shimmer_pitch", .6f}},
         {{"shimmer_depth", Sine, .02f, 0.4f}, {"grain_density", Drunk, .03f, 0.48f}}},
        {"AMBIENT", "Lunar Tape", {{"offset", .3f}, {"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .5f},
                                   {"echo_sync", 0}, {"echo_time", .9f}, {"echo_fdbk", .6f}, {"echo_mix", .35f},
                                   {"air_on", 1}, {"air_lofi", .35f}, {"air_width", .9f},
                                   {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}},
         {{"air_lofi", Perlin, .02f, 0.4f}, {"echo_mix", Breath, .02f, 0.32f}, {"offset", Sine, .3f, .15f}}},
        {"AMBIENT", "Distant Signals", {{"offset", .25f}, {"grain_on", 1}, {"grain_env", 2}, {"grain_m12", 1},
                                        {"grain_density", .3f}, {"grain_size", .25f}, {"grain_chance", .5f}, {"grain_mix", .4f},
                                        {"focus_on", 1}, {"focus_mode", 2}, {"focus_depth", .7f},
                                        {"derez_on", 1}, {"derez_interval", .75f}, {"derez_crush", 0.5f}, {"derez_mix", .3f},
                                        {"echo_on", 0}, {"space_type", 2}, {"space_decay", .78f}, {"space_mix", .6f}},
         {{"derez_mix", SampleHold, .15f, 0.48f}, {"grain_chance", Chaos, .04f, 0.48f}}},
    };
    return list;
}

}  // namespace stasis::presets
