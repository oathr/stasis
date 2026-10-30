# STASIS

**An ambient effects rack**: deep reverbs, tape echo, PaulXStretch-style time stretch, grains, tuned DeRez and MIDI-driven harmonic filtering, in a single chain built for pads, synths and recordings.

VST3 · Windows & macOS · JUCE 8 · v0.1 (in development)

![The STASIS interface](docs/screenshot.png)

---

## Contents
- [Principles](#principles)
- [The chain](#the-chain)
- [The modules](#the-modules)
- [Presets](#presets)
- [Installation](#installation)
- [Using it in Ableton Live](#using-it-in-ableton-live)
- [Building](#building)
- [Code layout](#code-layout)
- [Measurements and tests](#measurements-and-tests)
- [Roadmap](#roadmap)
- [Licences](#licences)

---

## Principles
- **Top DSP quality, minimal CPU.** Every algorithm is measured: echo density, resonances, tuning, tonal/noise separation, CPU cost.
- **The dry signal is never delayed.** The latency reported to the host is **0 samples**. Modules that need time (STRETCH, TONAL, PITCH) only delay the wet signal, where an offset is musically invisible.
- **Few controls, each one useful.** Three to six knobs per module; every parameter can be automated.
- **A sober design**: Swiss design, 80s print, inspired by Gattaca, *Her* and Marathon, without copying any of their visuals.

## The chain

```
IN ─┬───────────────────────────────────────────────────────────────────────────┬─ MIX ─ OUT
    └ SHAPE ► DEREZ ► PITCH ► STRETCH ► GRAIN ► ECHO ► SPACE ► FOCUS ► AIR ► COLOR ► [TONAL] ┘
                 ▲                                     ▲
MIDI IN ─────────┴─────────────────────────────────────┘ (DEREZ, FOCUS)
```

- **The order is yours**: grab a card by its black vertical spine and drop it elsewhere, or use the ◂ ▸ arrows at the top of the spine. A wire links the modules in processing order; the small numbered square where it enters each card gives its position. The order is saved with the project and with presets.
- By default, the crunch and the stretch feed the echo, which feeds the reverb.
- The visualizer at the top shows the actual spectrum of the wet signal.
- Each module has a switch (the square at the bottom of its spine) with a 30 ms fade, so it never clicks.
- `DRY / WET` (top, 100 % by default) balances wet against dry; `OUTPUT` sets the output level (±12 dB).
- `OFFSET` widens the wet signal: its right channel arrives 0 to 25 ms after the left (Haas effect, like Arturia's offset mode). The dry signal is untouched; at 0, nothing is computed.
- `⏻` (BYPASS): the output goes back to the untouched input, with a 30 ms fade, and the rack dims. The effects keep running underneath, so the tails do not freeze.
- The button left of the themes puts the visualizer **on top** or **at the side** (for short screens: the window loses 13 % of its height; in the side column, time runs downwards). The choice is saved with the project.
- **Level makeup**: the modules' losses used to add up (GRAIN, DEREZ, STRETCH… a preset could come out 10 to 18 dB quieter than its input). The wet is now brought back up to the input's loudness: slowly (1.5 s up, 150 ms down, so a reverb still building never overshoots), makeup only (0 to +12 dB: a module that boosts keeps its boost), and frozen while the input is silent so tails fade naturally. Measured: the 60 presets now sit between −3.6 and +2 dB of the input.
- **An invisible safety limiter** at the very end: nothing goes above −0.3 dBFS, and if a bug ever produced invalid values (NaN), the output mutes for 0.25 s and the engine resets. Your ears are safe.
- `FREEZE` holds the reverb: the tail lasts forever, without drifting.
- `PANIC` (red, top right): instant silence, clears every tail and buffer (reverb, echo, stretch, capture, held notes) and releases FREEZE. Settings, order and MOTION stay as they are.

## The modules

### SHAPE — the first touch
| Control | What it does |
|---|---|
| `TYPE` | How DRIVE bends the wave (see below) |
| `TILT` | ±6 dB tilt around 800 Hz: dark ◂▸ bright |
| `DRIVE` | Amount: subtle in the lower half, furious at the top (×40 gain at 100 %); the level at −12 dBFS stays the same whatever the type |
| `HARM` | Asymmetry: even harmonics, "tube" warmth |

| Type | Character |
|---|---|
| `TAPE` | Alias-free tanh (ADAA): round, warm, the classic |
| `TUBE` | Asymmetric exponential: the top of the wave softens earlier than the bottom; even harmonics even without HARM |
| `FOLD` | Sine wavefolder: past full scale the wave folds back on itself; bells, glassy overtones, "west coast" |
| `RECT` | Blends in the rectified wave: an octave up appears (fuzz octaver) |
| `SLEW` | Limits how fast the wave can move: edges turn into ramps; a soft, ghostly grit that follows the pitch (on loud high notes, a faint sub-tone remains) |
| `BLOOM` | Drive follows the level inversely: attacks stay clean, decays and reverb tails get gritty as they fade |

Every type except TAPE runs 2× oversampled; aliasing measured at 3.1 kHz, −3 dBFS: from −21 dB (RECT) to −50 dB (BLOOM).

### PITCH — transpose and shift
| Control | What it does |
|---|---|
| `SEMI` | Transposition from −24 to +24 semitones (phase vocoder, accurate to ±0.5 Hz at 440 Hz) |
| `SHIFT` | Frequency shift of ±50 Hz (fine control around the centre): every frequency moves by the same number of Hz, from slow beating (±1 Hz) to metallic. Unwanted sideband at −43 to −49 dB |
| `MIX` | Blend, truly half and half at 50 %: the transposed voice is brought back to the input level (the vocoder lost 4 dB going up, 6 dB going down) and the direct part is delayed as much as the vocoder, for a clean interval rather than a note followed by its echo. Measured: 0.100 / 0.100 at +12, −12 and in SHIFT |

### AIR — texture and width
| Control | What it does |
|---|---|
| `BREATH` | Airy noise (2–9 kHz) that follows the sound's envelope and stops with it |
| `SMOOTH` | How smoothly the breath follows the sound: 20 ms (nervous) to 1 s (a slow wave). 140 ms by default |
| `WIDTH` | Stereo width, from mono to 150 % |
| `LO-FI` | Worn medium: the top closes (16 → 3.5 kHz), the resolution drops, peaks are rounded |
| `THRESH` | BREATH only starts above this level (−60 to −6 dBFS, soft knee over ~12 dB): quiet tails stay clean |

### DEREZ — *tuned* resolution reduction
Inspired by Airwindows' DeRez, but instead of shifting the pitch chaotically, the reduced sample rate is locked to the note being played: the aliasing lands on a musical interval.

| Control | What it does |
|---|---|
| `−24 … +24` slider | Pitch of the aliasing, by semitone (a mark at each octave) |
| `RATE` | Reduction rate (n = ratio to the fundamental). As in DeRez: **top = clean**, the lower, the harder it crushes |
| `BITS` | Bit depth: 16 bits at the top (clean), 2 bits at the bottom |
| `SMOOTH` | Softness: at 0 %, the raw staircase and sharp bit steps (it crackles); towards 100 %, the steps are joined by slopes, bit steps become soft and a 12 dB/oct low-pass follows the crush rate. 70 % by default. Measured (a fading 220 Hz tone, energy above 4 kHz): −22 dB at 0 %, −60 dB at 70 %, −68 dB at 100 % (the clean sound: −71 dB) |
| `MIX` | Blend |

The fundamental comes from the lowest MIDI note held, otherwise A2 (110 Hz). Measured: 100 % of the energy stays in tune, against 9 to 77 % for a classic DeRez.

### STRETCH — a continuous pad (BLUR)
Constantly stretches **what is being played now**: 32768-point FFT frames with random phases (as in PaulStretch), with the spectrum averaged over time. Notes swell and fade smoothly; the pad never stops and never jumps.

| Control | What it does |
|---|---|
| `MEMORY` | How long the pad remembers what was played: 0.1 s to 2 min (1 s by default) |
| `MIX` | Blend |

A test checks that a click comes out as a smooth breath, never as repeats (`stasis_render --analyse`).

### GRAIN — separate notes, drawn from the sound
Replays small slices of what was just played, under an envelope, at random moments and transposed to chosen intervals: a continuous sound becomes a multitude of separate notes. Placed right after STRETCH by default (the echo and the reverb turn the notes into a cloud).

| Control | What it does |
|---|---|
| `PULSE` / `SCATTER` / `DRIFT` | Randomness: a steady clock (CHANCE skips some ticks); random gaps (Poisson); a rate that drifts slowly (×0.25 to ×4): clusters, then silences |
| `SINE` / `TRI` / `SQUARE` / `PERC` | Envelope of each grain (SQUARE has 2 % ramps, no clicks; PERC attacks fast and decays) |
| `−12` `−7` `−5` `−3` `0` `+3` `+5` `+7` `+12` | Allowed transpositions, in semitones, relative to the sound itself (no pitch detection needed): `0` = unison, `+7` = a fifth up, `−12` = an octave down. Default: 0, +7, +12 |
| `FREE` / `SYNC` | FREE: `DENSITY` in grains per second (0.5 to 40). SYNC: `RATE` as a division of the host tempo (1/32 to 1/1, dotted, triplets); in PULSE the grains land on the song's grid, in SCATTER / DRIFT the randomness stays around that rate |
| `FDBK` | Feedback, 0 to 100 %: the grains go back into the cloud (512 samples later, soft-clipped), so grains are cut from grains: cascades, then a cloud that feeds itself. At 100 % it stays bounded and fades (−56 dB after 9 s of silence) |
| `CHANCE` | Probability that a scheduled grain actually plays |
| `SIZE` | 20 ms to 2 s |
| `MIX` | Blend (at 100 %: the notes only) |

Grains are drawn **inside the spectrum**, in blue, on its dot grid: each grain lights the cell of the column where it plays, at the height of its real frequency (the dominant note of the sound, shifted by its interval), with a size that follows its volume. When two grains share a cell, only the louder is drawn. The number of grains per second is shown at the bottom right of the spectrum. The host test checks that −12, 0 and +12 land on the right octave (+18 to +24 dB). Measured before the switch to intervals, on a steady 220 Hz tone (unison, fifth and their octave): 66 % of silence between notes, energy at 220 / 330 / 440 / 660 Hz, a non-allowed note 50 dB lower.

### ECHO — tape echo
Saturation (anti-aliased tanh) and tape filters in the loop, no hiss, no wow or flutter: the repeats stay in tune. Time changes glide like a tape changing speed.

| Control | What it does |
|---|---|
| `MONO` / `PING-PONG` | Repeats in the centre, or alternating left/right |
| `FREE` / `SYNC` | Free time (20 ms – 2.5 s) or locked to the host tempo (1/32 to 1/1, dotted, triplets) |
| `TIME`, `FDBK` | Time, feedback |
| `MIX` | Level of the repeats |

### SPACE — the reverbs
| Type | Engine |
|---|---|
| `ROOM` / `HALL` / `CATHEDRAL` | 16-line FDN: 8-channel diffuser, Householder matrix + rotation (no line feeds back into itself), 3-band absorption, modulation that adapts to the decay time |
| `PLATE` | Dattorro plate |
| `BLOOM` | Two 8-line networks in series ahead of the FDN: the reverb **swells** after the attack instead of starting at full level. The larger `DECAY` and `SIZE`, the longer the swell |
| `RESONATOR` | 24 tuned combs (12 notes × 2 octaves, damped like strings) ahead of a shorter reverb: the tail **sings notes**. It follows the MIDI notes held, otherwise what FOCUS AUTO hears, and keeps the last chord (C + G at start). `SIZE` picks the register (C4, C3 or C2), `TONE` the damping. The notes played are shown in SPACE's header |

| Control | What it does |
|---|---|
| `SIZE`, `DECAY`, `TONE` | Size, decay time (0.5 s to ∞), brightness of the tail |
| `MOD`, `PRE`, `MIX` | Modulation, pre-delay (1 to 500 ms), blend |
| `SHIMMER DEPTH` / `PITCH` | Share of the feedback that is transposed by a phase vocoder, from −24 to +24 semitones |
| `WIDTH` | Stereo width |

Measured: the hall is dense after 90 ms, residual ringing 0.027; freeze drift 0.01 dB/min.
For a click with DECAY 5 s, HALL's energy peaks at 175 ms and BLOOM's at 525 ms. BLOOM reaches the same peak level, but its first 50 ms are 16 dB lower.
With white noise in and an A held over MIDI, RESONATOR puts out 23 dB more on the A's than on the neighbouring notes.

### FOCUS — a wet signal that follows MIDI
Keeps in the wet signal the frequencies of the notes being played and attenuates the rest, including the tail of the previous chord. **Zero latency** (bank of resonant filters).

| Control | What it does |
|---|---|
| `KEY` | Keeps the held notes, at every octave |
| `HARM` | Keeps the notes and their harmonics (tuned combs) |
| `AUTO` | Without MIDI: detects the notes being played by itself |
| `FOCUS`, `DEPTH`, `MIX` | Precision, strength, blend |

The notes being followed light up on the module's keyboard.

### COLOR — finishing the wet signal
| Control | What it does |
|---|---|
| `BASS` | ±24 dB shelf at 110 Hz (below 0, a high-pass is added) |
| `BODY` | ±18 dB bell at 220 Hz: the warmth of the pad |
| `MUD` | ±18 dB bell at 450 Hz: the boxy build-up of long tails (down to clear it) |
| `HARSH` | ±18 dB bell at 3.5 kHz: the glassy edge of shimmers and bright pads |
| `AIR` | ±24 dB shelf at 7 kHz |

| `TREMOLO` / `RATE` | Amplitude wobble (smooth sine, sample-accurate): at 100 % the dips reach silence. FREE: 0.5 to 16 Hz; SYNC: a division of the tempo (1/32 to 1/1, dotted, triplets), on the song's grid |

Every word is bipolar: centre = flat, up = boost, down = cut, shown in dB. Moves glide over ~20 ms and the moving bands are recomputed every sample: a knob jumping by 22 dB 47 times a second leaves no grit (−76 dB above 4 kHz, the same as the clean tone; it was −50 dB updating every 16 samples).
| `LOW CUT` / `HIGH CUT` | High-pass 20 Hz – 1 kHz, low-pass 1 – 20 kHz (12 dB/oct, *off* at the ends) |
| `NOISE ◂▸ TONAL` | Right: keeps only the notes. Left: keeps only the breath and texture. Double-click to centre |
| `PRE` / `POST` | TONAL before the reverb (it rebuilds its tail from what is left) or after it (the tail itself is filtered) |

Measured separation: a notes-to-noise ratio of +7.6 dB at the input → **+17.1 dB** at full TONAL, **−1.8 dB** at full NOISE.

### MOTION — 12 freely assignable LFOs
Each lane moves **the knob of your choice**. By default only lane 1 is assigned, to the global DRY / WET; the others are empty.

| Control | What it does |
|---|---|
| Target | Click the name: a list of knobs, grouped by module in chain order (GLOBAL, SHAPE, DEREZ… COLOR), or *Empty* |
| `◎` TOUCH | Arms the lane: every assignable knob gets outlined, and clicking one assigns it (Esc to cancel) |
| `RATE` | FREE: 0.001 to 20 Hz. SYNC: a division of the tempo, 64 bars to 1/32, locked to the song position while playing |
| Shape | A click opens the shape menu, with their drawings: sine, triangle, saws, square, S&H, slew, random sine, drunk, Perlin, chaos (Lorenz), breath |
| An LFO on an LFO | A lane can target another lane's `RATE` or `DEPTH` (MOTION column of the list) |
| One LFO per knob | Assigning a knob already moved by another lane takes it from that lane |
| `DEPTH` | How far the lane swings the knob **around where you set it**, as a share of the whole travel: a knob at 20 % with DEPTH 100 % swings from −80 % to 120 %, held inside 0–100 %. At 0 % (the default), the lane does nothing and computes nothing. MOTION never turns DECAY or FEEDBACK endless on its own |
| `FREE` / `SYNC` | Free rates in Hz, or every lane on the host tempo |
| `HARD` | Modulations in steps: the moved knobs jump by 1/32 of their travel and COLOR stops gliding, so each step lands at once and clicks, short and on the LFO's rhythm (measured: +47 dB of clicks above 4 kHz compared with the glide) |
| `MOTION` / `DRIFT` | Global depth, global speed (×0.25 to ×4, FREE only) |

A modulated knob shows an orange arc over the whole swing, always visible, and a dot that runs along it; hovering a lane outlines its target. Assignments are saved with the project.

## Presets
60 factory presets, in three families of 20. Each comes with its own MOTION modulations (2 to 4 lanes, matched to the sound), almost all use OFFSET (several move it with an LFO: the pitch wavers like tape), and many use GRAIN. None is endless: FEEDBACK and DECAY stay finite. A new instance starts on **Glacier**. Click the preset name to open the list, ‹ › to step through. They also show up as VST3 *programs* in the host. Loading a preset first clears every tail (like PANIC), so you hear the new preset on its own.

| ECHO | REVERB | AMBIENT |
|---|---|---|
| Tape Memory | Cathedral 25 | Glacier |
| Dub Wash | Endless Hall | Frozen Orchard |
| Cassette | Glass Plate | Dust & Breath |
| Ping-Pong Garden | Small Room Halo | Tuned Ruins |
| Infinite Tape | Shimmer Cascade | Pure Tones |
| Octave Repeats | Fifth Heaven | Monastery Tape |
| Slow Motion | Underworld | Sub Tide |
| Shimmer Delay | Dark Matter | Northern Lights |
| Triplet Rain | Focused Choir | Slow Bloom |
| Echoes of the Room | Bottomless | Infinite Drone |
| Grain Bells | Grain Cathedral | Music Box Fog |
| Tape Stutter | Crystal Hall | Pentatonic Rain |
| Warped Reel | Shimmer Grains | Hexagon Field |
| Dotted Haze | Breathing Room | Glass Rain |
| Slapback Tube | Velvet Plate | Minor Constellation |
| Fifth Echoes | Ghost Choir | Breathing Pad |
| Frequency Drift | Dirty Tails | Cloud Chamber |
| Granular Delay Cloud | Halo Infinity | Fourths Drift |
| Answering Machine | Metal Room | Lunar Tape |
| Long Goodbye | Sub Cavern | Distant Signals |

## Installation

### Windows (Ableton Live)
1. Open the repository's **[Actions](../../actions)** tab, pick the latest successful build and download the **STASIS-VST3-Windows** artifact.
2. Unzip it: you get a `STASIS.vst3` folder.
3. Copy it to `C:\Program Files\Common Files\VST3\`.
4. In Live: *Options › Preferences › Plug-ins*, turn on **Use VST3 Plug-in System Folders**, then **Rescan**.

The interface uses WebView2, which ships with Windows 10 and 11.

### macOS
Same steps with the **STASIS-VST3-macOS** artifact, copied to `~/Library/Audio/Plug-Ins/VST3/`. The plugin is not signed yet: on first launch, macOS may ask for permission (*System Settings › Privacy & Security*).

## Using it in Ableton Live
- **On an audio or synth track**: drop STASIS as an audio effect, after the instrument.
- **Sending MIDI to STASIS** (for FOCUS and DEREZ): create a MIDI track, set its *MIDI To* output to the track that holds STASIS, then pick **STASIS** in the second menu. Play on that track: the notes received appear at the top right (`MIDI`) and light up on FOCUS's keyboard, even with the module off; **FOCUS must be on** to filter the sound. The MIDI track must play a clip, or be armed (or monitoring *In*) for a keyboard played live.
- **Without MIDI**: FOCUS in `AUTO` detects the notes; DEREZ uses A2 as its fundamental.
- **Tempo**: ECHO in `SYNC` follows the project tempo.
- **On a send**: set `MIX` to 100 % and adjust with the return level.
- **Automation**: every knob shows up in Live's parameter list, with the same units as on screen.

## Building
Requirements: CMake ≥ 3.22, a C++20 compiler, Git. JUCE 8 is downloaded automatically by CMake.

### Windows
Visual Studio 2022 or newer (*Desktop development with C++* workload), then the WebView2 SDK:
```bash
nuget install Microsoft.Web.WebView2 -OutputDirectory C:\webview2
cmake -S . -B build -A x64 -DSTASIS_BUILD_RENDER=OFF -DJUCE_WEBVIEW2_PACKAGE_LOCATION=C:\webview2
cmake --build build --config Release --target Stasis_VST3
```
The plugin ends up in `build/plugin/Stasis_artefacts/Release/VST3/STASIS.vst3`.

### macOS
```bash
cmake -S . -B build -G Ninja
cmake --build build
```
Builds the VST3, a **Standalone** app (handy for testing without a DAW), the `stasis_hosttest` host test and the `stasis_render` render tool.

### Continuous integration
Every `push` runs [`.github/workflows/build.yml`](.github/workflows/build.yml): Windows and macOS builds, stress test, downloadable plugins.

## Code layout

| Folder | Contents |
|---|---|
| [`dsp/include/stasis/`](dsp/include/stasis) | C++20 DSP core, header-only, **no dependencies**: reverbs, echo, stretch, grains, DeRez, FOCUS, COLOR, TONAL, LFOs, FFT |
| [`plugin/Source/Params.h`](plugin/Source/Params.h) | The parameter table (including MOTION's 12 lanes), shared by the host, the interface and the engine |
| [`plugin/Source/Presets.h`](plugin/Source/Presets.h) | The 60 factory presets |
| [`plugin/Source/Engine.h`](plugin/Source/Engine.h) | The whole audio chain, independent of JUCE |
| [`plugin/Source/Plugin*.cpp`](plugin/Source) | JUCE processor (MIDI, tempo, state) and WebView editor |
| [`design/mockup.html`](design/mockup.html) | The interface: an HTML page embedded in the plugin. Opened in a browser it works on its own (interactive mockup, 5 themes) |
| [`plugin/HostTest/`](plugin/HostTest) | `stasis_hosttest`: loads the VST3 like a DAW and stresses it |
| [`tools/render/`](tools/render) | `stasis_render`: renders test sounds through each algorithm (WAV) and runs the measurements (`--analyse`) |
| [`PLAN.md`](PLAN.md) | The detailed plan (in French): algorithms, choices, measurements, decisions, roadmap |

The interface and the host talk through JUCE 8's web relays: each HTML control carries the `id` of its parameter, and the sync goes both ways (automation, undo, presets). The plugin also sends the wet spectrum to the visualizer 30 times per second, along with its status (CPU, notes being followed).

## Measurements and tests

```bash
./build/stasis_render --analyse                     # DSP measurements (density, tuning, DeRez, TONAL…)
./build/stasis_render                               # WAV renders into renders/
./build/plugin/stasis_hosttest_artefacts/Release/stasis_hosttest build/plugin/Stasis_artefacts/Release/VST3/STASIS.vst3
```

The host test plays 12 s of sound per configuration (44.1 / 48 / 96 kHz, buffers from 64 to 2048), with random automation, MIDI and a state save/reload, then runs the 60 presets (the tail must ring, stay finite and stay below 0 dBFS). Results on Apple Silicon:

| Measurement | Result |
|---|---|
| Average CPU | 1 to 3 % of one core |
| Worst block under automation (64 samples at 48 kHz) | 41 % of real time |
| Every parameter changing at once | ~2 ms in the engine |
| Output | always finite, never runs away |

Modules with long FFTs spread their work over several audio blocks: no CPU spikes, even with very short buffers.

## Roadmap
- [x] **Phase 0** — prototypes and measurements of every key algorithm
- [x] **Phase 1** — VST3 plugin (Windows first), embedded interface, tests, CI
- [x] Free module order, 60 presets, MOTION, 5-band COLOR + filters
- [x] SHAPE, PITCH, AIR; safety limiter
- [x] BLOOM and RESONATOR reverbs
- [ ] Fonts embedded in the plugin (currently loaded from Google Fonts)
- [ ] More experimental reverbs: Spectral, Modal, 80s
- [ ] AU, CLAP; signing and installers

Details in [PLAN.md](PLAN.md).

## Licences
- Personal use for now; an open-source release under **GPLv3** is planned if the result is good.
- JUCE 8 is used under its **AGPLv3** licence (or commercial).
- The moodboard that inspired the design is licensed: none of its visuals are reproduced.
