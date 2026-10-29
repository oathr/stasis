#pragma once

#include <utility>
#include <vector>

// Factory presets: 30, in three families. Each preset lists only what differs from the
// parameter defaults (Params.h): knobs as 0..1 positions, switches as 0/1, choices as
// indices. `order` is the module order (empty = default).
//
// Useful positions (see the conversions in Params.h):
//   echo_time   300 ms .57 · 500 ms .67 · 800 ms .76 · 1.2 s .85 · 2 s .95
//   echo_div    index/13: 1/8T 4 · 1/8 5 · 1/8. 6 · 1/4T 7 · 1/4 8 · 1/4. 9 · 1/2 11 · 1/1 13
//   space_decay 1.2 s .19 · 3 s .37 · 5 s .48 · 8 s .58 · 12 s .66 · 15 s .71 · 20 s .78 · 25 s .82 · 40 s .92 · ∞ 1
//   space_pre   20 ms .48 · 40 ms .59 · 80 ms .71
//   shimmer_pitch  -12 .25 · +5 .60 · +7 .65 · +12 .75 · +19 .90 · +24 1
//   stretch_factor (MEMORY) 0.5 s .2 · 1 s .3 · 2.5 s .43 · 6 s .57 · 12 s .67 · 37 s .83
//   color_hpf   30 Hz .1 · 60 Hz .28 · 100 Hz .41 · 150 Hz .52      color_lpf  3 kHz .37 · 4 kHz .46 · 6 kHz .6 · 8 kHz .69
//   derez_interval  -24 0 · -12 .25 · -7 .354 · 0 .5 · +7 .646 · +12 .75 · +24 1
namespace stasis::presets {

struct Preset {
    const char* category;
    const char* name;
    std::vector<std::pair<const char*, float>> values;
    const char* order = "";
};

inline const char* const kCategories[] = {"ECHO", "REVERB", "AMBIENT"};

inline const std::vector<Preset>& all() {
    static const std::vector<Preset> list = {
        // ---------------------------------------------------------------- ECHO
        {"ECHO", "Tape Memory", {{"echo_div", 9 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .45f},
                                 {"space_type", 0}, {"space_size", .35f}, {"space_decay", .3f}, {"space_mix", .25f}, {"color_harsh", .3f}}},
        {"ECHO", "Dub Wash", {{"echo_mode", 0}, {"echo_div", 8 / 13.f}, {"echo_fdbk", .82f}, {"echo_mix", .55f},
                              {"space_type", 2}, {"space_decay", .43f}, {"space_mix", .3f}, {"color_bass", .4f}, {"color_lpf", .46f}}},
        {"ECHO", "Broken Cassette", {{"echo_sync", 0}, {"echo_time", .61f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                     {"derez_on", 1}, {"derez_interval", 0.5f}, {"derez_crush", .3f}, {"derez_bits", .5f}, {"derez_mix", .4f},
                                     {"space_on", 0}, {"color_air", .3f}, {"color_lpf", .6f}}},
        {"ECHO", "Ping-Pong Garden", {{"echo_div", 6 / 13.f}, {"echo_fdbk", .7f}, {"echo_mix", .5f},
                                      {"space_type", 1}, {"space_decay", .45f}, {"space_size", .5f}, {"space_mix", .35f}, {"color_air", .6f}}},
        {"ECHO", "Infinite Tape", {{"echo_sync", 0}, {"echo_time", .85f}, {"echo_fdbk", 1}, {"echo_mix", .5f},
                                   {"space_mix", .3f}, {"space_decay", .58f}, {"color_hpf", .41f}, {"color_lpf", .53f}}},
        {"ECHO", "Octave Repeats", {{"echo_div", 8 / 13.f}, {"echo_fdbk", .65f}, {"echo_mix", .5f},
                                    {"derez_on", 1}, {"derez_interval", 0.75f}, {"derez_crush", .5f}, {"derez_mix", .5f},
                                    {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f}},
         "shape,pitch,stretch,grain,echo,derez,space,focus,air,color"},
        {"ECHO", "Slow Motion", {{"stretch_on", 1}, {"stretch_factor", .2f}, {"stretch_mix", .5f},
                                 {"echo_div", 11 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                 {"space_type", 0}, {"space_size", .4f}, {"space_decay", .3f}, {"space_mix", .3f}}},
        {"ECHO", "Shimmer Delay", {{"echo_div", 9 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .5f},
                                   {"space_type", 3}, {"space_decay", .58f}, {"shimmer_depth", .5f}, {"shimmer_pitch", .75f}, {"space_mix", .35f},
                                   {"color_harsh", .5f}}},
        {"ECHO", "Triplet Rain", {{"echo_div", 4 / 13.f}, {"echo_fdbk", .75f}, {"echo_mix", .4f},
                                  {"space_type", 2}, {"space_decay", .48f}, {"space_mix", .3f},
                                  {"color_harsh", .6f}, {"color_air", .35f}, {"color_lpf", .6f}}},
        {"ECHO", "Echoes of the Room", {{"space_type", 2}, {"space_decay", .5f}, {"space_mix", .6f},
                                        {"echo_div", 11 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .5f}},
         "shape,derez,pitch,stretch,grain,space,echo,focus,air,color"},

        // -------------------------------------------------------------- REVERB
        {"REVERB", "Cathedral 25", {{"echo_on", 0}, {"space_type", 3}, {"space_size", 1}, {"space_decay", .82f}, {"space_tone", .4f},
                                    {"space_mod", .6f}, {"space_pre", .59f}, {"space_mix", .55f}, {"color_mud", .6f}}},
        {"REVERB", "Endless Hall", {{"echo_on", 0}, {"space_type", 2}, {"space_size", .85f}, {"space_decay", .92f}, {"space_mix", .55f},
                                    {"color_mud", .5f}, {"color_harsh", .4f}}},
        {"REVERB", "Glass Plate", {{"echo_on", 0}, {"space_type", 1}, {"space_size", .6f}, {"space_decay", .8f}, {"space_tone", .7f},
                                   {"space_mix", .45f}, {"color_air", .65f}}},
        {"REVERB", "Small Room Halo", {{"echo_on", 0}, {"space_type", 0}, {"space_size", .3f}, {"space_decay", .19f}, {"space_pre", .3f},
                                       {"space_mix", .4f}}},
        {"REVERB", "Shimmer Cascade", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .71f}, {"shimmer_depth", .55f}, {"shimmer_pitch", .75f},
                                       {"space_mix", .55f}, {"color_harsh", .5f}}},
        {"REVERB", "Fifth Heaven", {{"echo_on", 0}, {"space_type", 2}, {"space_decay", .66f}, {"shimmer_depth", .45f}, {"shimmer_pitch", .65f},
                                    {"space_mix", .5f}, {"color_harsh", .4f}}},
        {"REVERB", "Underworld", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_tone", .25f}, {"shimmer_depth", .5f},
                                  {"shimmer_pitch", .25f}, {"space_mix", .55f}, {"color_body", .7f}, {"color_hpf", .1f}}},
        {"REVERB", "Dark Matter", {{"echo_on", 0}, {"space_type", 2}, {"space_decay", .86f}, {"space_tone", .1f}, {"space_mod", .8f},
                                   {"space_mix", .55f}, {"color_mud", .7f}, {"color_lpf", .37f}}},
        {"REVERB", "Focused Choir", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f},
                                     {"focus_on", 1}, {"focus_mode", 1}, {"focus_amount", .6f}, {"focus_depth", .9f}}},
        {"REVERB", "Bottomless", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", 1}, {"space_mix", .55f},
                                  {"tonal", .75f}, {"color_mud", .6f}}},

        // ------------------------------------------------------------- AMBIENT
        {"AMBIENT", "Glacier", {{"stretch_on", 1}, {"stretch_factor", .43f}, {"stretch_mix", .7f}, {"echo_on", 0},
                                {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f}, {"tonal", .8f}}},
        {"AMBIENT", "Frozen Orchard", {{"stretch_on", 1}, {"stretch_factor", .57f}, {"stretch_mix", .8f},
                                       {"echo_on", 0}, {"space_type", 2}, {"space_decay", .66f}, {"space_mix", .5f}, {"color_air", .6f}}},
        {"AMBIENT", "Dust & Breath", {{"derez_on", 1}, {"derez_interval", 0.5f}, {"derez_crush", .7f}, {"derez_bits", .71f}, {"derez_mix", .3f},
                                      {"echo_on", 0}, {"space_type", 3}, {"space_decay", .71f}, {"space_mix", .6f},
                                      {"tonal", .15f}, {"tonal_place", 0}, {"color_harsh", .4f}}},
        {"AMBIENT", "Tuned Ruins", {{"derez_on", 1}, {"derez_interval", 0.25f}, {"derez_crush", .6f}, {"derez_mix", .7f}, {"echo_on", 0},
                                    {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .6f}}},
        {"AMBIENT", "Pure Tones", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_mix", .6f},
                                   {"focus_on", 1}, {"focus_mode", 2}, {"focus_depth", .8f}, {"tonal", .95f}}},
        {"AMBIENT", "Monastery Tape", {{"stretch_on", 1}, {"stretch_factor", .3f}, {"stretch_mix", .5f},
                                       {"echo_div", 9 / 13.f}, {"echo_fdbk", .6f}, {"echo_mix", .45f},
                                       {"space_type", 3}, {"space_decay", .78f}, {"space_mix", .55f}}},
        {"AMBIENT", "Sub Tide", {{"echo_on", 0}, {"space_type", 3}, {"space_decay", .82f}, {"space_tone", .3f},
                                 {"shimmer_depth", .6f}, {"shimmer_pitch", .25f}, {"space_mix", .6f},
                                 {"color_bass", .65f}, {"color_hpf", .05f}, {"color_lpf", .6f}}},
        {"AMBIENT", "Northern Lights", {{"echo_div", 6 / 13.f}, {"echo_fdbk", .55f}, {"echo_mix", .3f},
                                        {"space_type", 2}, {"space_decay", .71f}, {"shimmer_depth", .4f}, {"shimmer_pitch", .9f}, {"space_mix", .55f},
                                        {"color_air", .65f}, {"color_harsh", .5f}}},
        {"AMBIENT", "Slow Bloom", {{"stretch_on", 1}, {"stretch_factor", .67f}, {"stretch_mix", .6f},
                                   {"echo_sync", 0}, {"echo_time", .95f}, {"echo_fdbk", .5f}, {"echo_mix", .35f},
                                   {"space_type", 3}, {"space_decay", .92f}, {"space_mix", .7f},
                                   {"color_mud", .7f}, {"color_harsh", .5f}}},
        {"AMBIENT", "Infinite Drone", {{"stretch_on", 1}, {"stretch_factor", .83f}, {"stretch_mix", 1},
                                       {"echo_on", 0}, {"space_type", 3}, {"space_decay", 1}, {"space_mix", .6f},
                                       {"tonal", .7f}, {"tonal_place", 0}}},
    };
    return list;
}

}  // namespace stasis::presets
