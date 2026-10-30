#pragma once

#include <stasis/air.h>
#include <stasis/bloom.h>
#include <stasis/glue.h>
#include <stasis/grain.h>
#include <stasis/capture_stretch.h>
#include <stasis/color.h>
#include <stasis/key_detector.h>
#include <stasis/lfo.h>
#include <stasis/note_set.h>
#include <stasis/pitched_derez.h>
#include <stasis/pitch.h>
#include <stasis/plate_reverb.h>
#include <stasis/resonant_focus.h>
#include <stasis/resonator.h>
#include <stasis/shape.h>
#include <stasis/space_reverb.h>
#include <stasis/tape_echo.h>
#include <stasis/tonal_noise.h>

#include <atomic>
#include <bit>

#include "Params.h"

namespace stasis {

// The whole effect, without any framework: the plugin feeds it parameter values
// (as stored by the host), MIDI notes, the tempo, and audio.
//
//  IN ─┬──────────────────────────────────────────────────────────────┬─ MIX ─ OUT
//      └ modules in the order the user chose ─ [TONAL post]              ┘
//
// Default order: SHAPE DEREZ PITCH STRETCH ECHO SPACE FOCUS AIR COLOR. TONAL sits
// right before SPACE (PRE) or at the very end (POST), wherever SPACE is.
//
// The dry path is never delayed. Modules with latency (STRETCH, TONAL) only delay the
// wet, where it is musically invisible, so the plugin reports zero latency.
class Engine {
public:
    static constexpr int kMaxBlock = 512;

    Engine() {
        for (size_t i = 0; i < laneTarget_.size(); ++i)
            for (size_t s = 0; s < kLaneTargets; ++s)
                laneTarget_[i][s].store(s == 0 && params::kDefaultTargets[i][0] ? params::indexOf(params::kDefaultTargets[i]) : -1);
    }

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        shape_.prepare(sampleRate);
        grain_.prepare(sampleRate);
        pitch_.prepare(sampleRate);
        air_.prepare(sampleRate);
        limiterGain_ = 1.0f;
        limiterRelease_ = smoothingCoef(0.15f, sampleRate);
        offsetLine_.fill(0.0f);
        offsetPos_ = 0;
        mute_ = 0;
        needsReset_.store(false);
        notes_ = NoteSet{};
        derez_ = PitchedDerez{};
        focus_ = ResonantFocus{};
        derez_.prepare(sampleRate);
        derez_.setRoot(110.0f);  // A2, when no MIDI note is held
        stretch_.prepare(sampleRate);
        echo_.prepare(sampleRate);
        space_.prepare(sampleRate);
        plate_.prepare(sampleRate);
        glue_.prepare(sampleRate);
        bloom_.prepare(sampleRate);
        resonator_.prepare(sampleRate);
        focus_.prepare(sampleRate);
        keys_.prepare(sampleRate);
        keys_.setParams({1.5f, 0.45f, 7});
        color_.prepare(sampleRate);
        tonal_.prepare(sampleRate);
        for (auto& b : buffers_) b.assign(kMaxBlock, 0.0f);
        for (auto& b : preSpace_) b.assign(kMaxBlock, 0.0f);
        modulated_.assign(params::all().size(), 0.0f);
        hardPrevious_.assign(params::all().size(), -1.0f);
        hardClick_ = 0.0f;
        for (auto& line : grainFbLine_) line.fill(0.0f);
        grainFbPos_ = 0;
        rateOffset_.fill(0.0f);
        depthOffset_.fill(0.0f);
        for (size_t i = 0; i < lanes_.size(); ++i) {
            lanes_[i].prepare(sampleRate, uint32_t(i + 1) * 7919u);
            laneRate_[i] = params::indexOf(params::laneId(int(i), "rate"));
            laneShape_[i] = params::indexOf(params::laneId(int(i), "shape"));
            laneSync_[i] = params::indexOf(params::laneId(int(i), "sync"));
            laneDepth_[i] = params::indexOf(params::laneId(int(i), "depth"));
        }
        rampCoef_ = smoothingCoef(0.03f, sampleRate);
        levelCoef_ = smoothingCoef(0.6f, sampleRate);
        makeupCoef_ = smoothingCoef(1.5f, sampleRate);
        makeupDownCoef_ = smoothingCoef(0.15f, sampleRate);
        gateCoef_ = smoothingCoef(0.05f, sampleRate);
        dryLevel_ = wetLevel_ = dryFast_ = 0.0f;
        makeup_ = 1.0f;
        for (auto& g : moduleGain_) g = 0.0f;
        tonalGain_ = mixGain_ = outGain_ = -1.0f;  // snap on the first block
        last_.assign(params::all().size(), NAN);
        visual_.fill(0.0f);
        visualWrite_ = 0;
    }

    void noteOn(int note) {
        derez_.noteOn(note);
        focus_.noteOn(note);
        notes_.noteOn(note);
    }
    void noteOff(int note) {
        derez_.noteOff(note);
        focus_.noteOff(note);
        notes_.noteOff(note);
    }

    // values[i] belongs to params::all()[i]: 0..1, 0/1 or a choice index.
    // ppq: song position in quarter notes at the start of the block (if playing).
    void process(const float* values, float bpm, double ppq, bool playing, float* left, float* right, int numSamples) {
        for (int start = 0; start < numSamples; start += kMaxBlock) {
            const int n = std::min(kMaxBlock, numSamples - start);
            const double at = ppq + double(start) * bpm / 60.0 / sampleRate_;
            grain_.setTransport(bpm, at, playing);
            update(modulate(values, n, bpm, at + double(n) * bpm / 60.0 / sampleRate_, playing), bpm);
            // TREMOLO SYNC: on the song's grid while playing.
            if (tremSync_ && playing) color_.setTremoloPhase(float(std::fmod(at / double(tremBeats_), 1.0)));
            processBlock(left + start, right + start, n);
        }
    }

    // For the UI: the last samples of the output (mono), written by the audio thread.
    // Reads may tear, which only matters to the eye.
    static constexpr int kVisualSize = 4096;
    void copyVisual(float* dest) const {
        const int w = visualWrite_.load(std::memory_order_acquire);
        for (int i = 0; i < kVisualSize; ++i) dest[i] = visual_[size_t((w + i) % kVisualSize)];
    }
    // Processing order: slot k holds the index (in params::kModules) of the module
    // that runs k-th, 4 bits per slot. Any thread may set it.
    static uint64_t defaultOrder() {
        uint64_t order = 0;
        for (size_t slot = 0; slot < params::kDefaultOrder.size(); ++slot) order |= uint64_t(params::kDefaultOrder[slot]) << (4 * slot);
        return order;
    }
    void setOrder(uint64_t order) { order_.store(order, std::memory_order_relaxed); }
    uint64_t order() const { return order_.load(std::memory_order_relaxed); }

    // The 12 MOTION lanes, -1..1 (for the UI traces).
    float laneValue(int lane) const { return laneValues_[size_t(lane)].load(std::memory_order_relaxed); }
    // Where each lane has moved its target knob right now (0..1), for the UI.
    float laneTarget(int lane, int slot) const { return laneTargets_[size_t(lane)][size_t(slot)].load(std::memory_order_relaxed); }
    // Which parameter (index in params::all(), -1 = none) a lane moves. Any thread.
    // A lane moves up to kLaneTargets knobs (slots; -1 = empty), all by the same LFO and DEPTH.
    static constexpr size_t kLaneTargets = 4;
    void setLaneTarget(int lane, int slot, int paramIndex) { laneTarget_[size_t(lane)][size_t(slot)].store(paramIndex, std::memory_order_relaxed); }
    int laneTargetIndex(int lane, int slot) const { return laneTarget_[size_t(lane)][size_t(slot)].load(std::memory_order_relaxed); }

    // GRAIN events for the UI's grain view.
    int grainEvents(GrainCloud::Event* out, int max, uint32_t& cursor) const { return grain_.readEvents(out, max, cursor); }

    // Pitch classes FOCUS is following (MIDI, or what AUTO hears). Bit n = class n.
    uint16_t focusClasses() const { return focusClasses_.load(std::memory_order_relaxed); }
    // Pitch classes of the MIDI notes held right now, whether FOCUS is on or not.
    uint16_t midiClasses() const { return midiClasses_.load(std::memory_order_relaxed); }
    // Pitch classes SPACE › RESONATOR rings on (0 when another type is selected).
    uint16_t resonatorClasses() const { return resonatorClasses_.load(std::memory_order_relaxed); }

private:
    enum Module { kDerez, kStretch, kEcho, kSpace, kFocus, kColor, kShape, kPitch, kAir, kGrain, kModules };

    float v(const float* values, int index) const { return values[index]; }

    // MOTION: each lane swings its target around the user's setting (set ± depth).
    // A lane may move another lane's RATE or DEPTH: those read the previous block's
    // modulation (one block late, ~10 ms), so the lanes' order does not matter.
    // HARD (amount): the moved knobs move in steps (32 per travel, down to 6 at 100 %) and
    // COLOR stops gliding and kicks its filters, so each step lands at once and cracks.
    // ppqEnd: song position at the end of this block (tempo sync).
    const float* modulate(const float* values, int n, float bpm, double ppqEnd, bool playing) {
        static const int iOn = params::indexOf("motion_on"), iAmount = params::indexOf("motion_amount"), iDrift = params::indexOf("motion_drift"),
                         iHard = params::indexOf("motion_hard");
        const bool on = values[iOn] > 0.5f;
        if (!on) {  // MOTION off: nothing runs, the parameters pass as they are
            if (motionWasOn_) for (auto& v : laneValues_) v.store(0.0f, std::memory_order_relaxed);
            rateOffset_.fill(0.0f);
            depthOffset_.fill(0.0f);
            motionWasOn_ = false;
            return values;
        }
        motionWasOn_ = true;
        std::copy(values, values + modulated_.size(), modulated_.begin());
        const float amount = values[iAmount], drift = params::driftScale(values[iDrift]);
        // HARD: 0 = off; the steps get coarser as it rises (32 per travel at the bottom, 6 at the top).
        const float hard = values[iHard], hardSteps = hard > 0.0f ? std::round(32.0f - 26.0f * hard) : 0.0f;
        const float beatsPerSecond = std::clamp(bpm, 20.0f, 999.0f) / 60.0f;
        for (size_t i = 0; i < lanes_.size(); ++i) {
            const float depth = std::clamp(values[laneDepth_[i]] + depthOffset_[i], 0.0f, 1.0f);
            bool any = false;
            for (const auto& slot : laneTarget_[i]) any = any || slot.load(std::memory_order_relaxed) >= 0;
            if (depth <= 0.0f || !any) {  // an empty lane, or one at 0 %, does not run
                laneValues_[i].store(0.0f, std::memory_order_relaxed);
                continue;
            }
            Lfo& lfo = lanes_[i];
            const float rate = std::clamp(values[laneRate_[i]] + rateOffset_[i], 0.0f, 1.0f);
            lfo.setShape(values[laneShape_[i]]);
            float x;
            if (values[laneSync_[i]] > 0.5f) {
                // Locked to the song position while playing, at the tempo otherwise. DRIFT
                // does not apply: the grid decides.
                const float beats = params::laneSyncBeats(rate);
                lfo.setRate(beatsPerSecond / beats);
                x = playing ? lfo.advanceTo(float(std::fmod(ppqEnd / double(beats), 1.0))) : lfo.advance(n);
            } else {
                lfo.setRate(params::laneRateHz(rate) * drift);
                x = lfo.advance(n);
            }
            laneValues_[i].store(x, std::memory_order_relaxed);
            // Around the knob's own position: DEPTH 100 % swings it by the whole travel
            // each way (a knob at 20 % goes from -80 % to 120 %, held inside 0 .. 100 %).
            for (const auto& slot : laneTarget_[i]) {
                const int targetIndex = slot.load(std::memory_order_relaxed);
                if (targetIndex < 0) continue;
                float& target = modulated_[size_t(targetIndex)];
                target = std::clamp(target + amount * depth * x, 0.0f, 1.0f);
                if (hardSteps > 0.0f) {
                    target = std::round(target * hardSteps) / hardSteps;
                    // Each step taken is a crack at the output, sized by the step and by HARD.
                    float& previous = hardPrevious_[size_t(targetIndex)];
                    if (previous >= 0.0f && target != previous)
                        hardClick_ += hard * std::min(1.0f, std::abs(target - previous) * 8.0f) * (target > previous ? 1.0f : -1.0f);
                    previous = target;
                }
            }
        }
        for (size_t i = 0; i < lanes_.size(); ++i) {
            rateOffset_[i] = modulated_[size_t(laneRate_[i])] - values[laneRate_[i]];
            depthOffset_[i] = modulated_[size_t(laneDepth_[i])] - values[laneDepth_[i]];
        }
        // MOTION never makes a tail endless on its own: DECAY stops short of ∞ and
        // FEEDBACK short of self-sustain, unless the knob itself is already there.
        static const int iDecay = params::indexOf("space_decay"), iFdbk = params::indexOf("echo_fdbk");
        modulated_[size_t(iDecay)] = std::min(modulated_[size_t(iDecay)], std::max(values[iDecay], 0.97f));
        modulated_[size_t(iFdbk)] = std::min(modulated_[size_t(iFdbk)], std::max(values[iFdbk], 0.95f));
        for (size_t i = 0; i < lanes_.size(); ++i) {
            for (size_t s = 0; s < kLaneTargets; ++s) {
                const int t = laneTarget_[i][s].load(std::memory_order_relaxed);
                laneTargets_[i][s].store(t >= 0 ? modulated_[size_t(t)] : 0.0f, std::memory_order_relaxed);
            }
        }
        return modulated_.data();
    }

    // Push the parameters to the modules, only those that changed (some setParams
    // recompute filters and gains).
    void update(const float* values, float bpm) {
        using namespace params;
        static const auto& specs = all();
        static const int iMix = indexOf("mix"), iOut = indexOf("out"), iFreeze = indexOf("freeze"), iBypass = indexOf("bypass"),
                         iOffset = indexOf("offset"), iGlue = indexOf("glue"), iStaging = indexOf("gain_staging");
        static const int iDerezOn = indexOf("derez_on"), iInterval = indexOf("derez_interval"), iCrush = indexOf("derez_crush"),
                         iBits = indexOf("derez_bits"), iDerezMix = indexOf("derez_mix"), iDerezSmooth = indexOf("derez_smooth");
        static const int iStretchOn = indexOf("stretch_on"), iFactor = indexOf("stretch_factor"), iStretchMix = indexOf("stretch_mix");
        static const int iEchoOn = indexOf("echo_on"), iEchoMode = indexOf("echo_mode"), iSync = indexOf("echo_sync"), iTime = indexOf("echo_time"),
                         iDiv = indexOf("echo_div"), iFdbk = indexOf("echo_fdbk"), iEchoMix = indexOf("echo_mix");
        static const int iSpaceOn = indexOf("space_on"), iType = indexOf("space_type"), iSize = indexOf("space_size"), iDecay = indexOf("space_decay"),
                         iTone = indexOf("space_tone"), iMod = indexOf("space_mod"), iPre = indexOf("space_pre"), iSpaceMix = indexOf("space_mix"),
                         iShimmer = indexOf("shimmer_depth"), iShimmerPitch = indexOf("shimmer_pitch"), iWidth = indexOf("space_width");
        static const int iFocusOn = indexOf("focus_on"), iFocusMode = indexOf("focus_mode"), iAmount = indexOf("focus_amount"),
                         iFocusDepth = indexOf("focus_depth"), iFocusMix = indexOf("focus_mix");
        static const int iColorOn = indexOf("color_on"), iBass = indexOf("color_bass"), iBody = indexOf("color_body"), iMud = indexOf("color_mud"),
                         iHarsh = indexOf("color_harsh"), iAir = indexOf("color_air"), iHpf = indexOf("color_hpf"), iLpf = indexOf("color_lpf"),
                         iTonal = indexOf("tonal"), iPlace = indexOf("tonal_place"), iTrem = indexOf("color_trem"),
                         iTremRate = indexOf("color_trem_rate"), iTremSync = indexOf("color_trem_sync"), iHard = indexOf("motion_hard"),
                         iMotionOn = indexOf("motion_on");

        const auto changed = [&](std::initializer_list<int> ids) {
            bool any = false;
            for (const int i : ids) any = any || std::bit_cast<uint32_t>(values[i]) != std::bit_cast<uint32_t>(last_[size_t(i)]);
            return any;
        };

        on_[kDerez] = v(values, iDerezOn) > 0.5f;
        on_[kStretch] = v(values, iStretchOn) > 0.5f;
        on_[kEcho] = v(values, iEchoOn) > 0.5f;
        on_[kSpace] = v(values, iSpaceOn) > 0.5f;
        on_[kFocus] = v(values, iFocusOn) > 0.5f;
        on_[kColor] = v(values, iColorOn) > 0.5f;
        static const int iShapeOn = indexOf("shape_on"), iTilt = indexOf("shape_tilt"), iDrive = indexOf("shape_drive"), iHarm = indexOf("shape_harm"),
                         iShapeType = indexOf("shape_type");
        static const int iPitchOn = indexOf("pitch_on"), iSemi = indexOf("pitch_semi"), iShift = indexOf("pitch_shift"), iPitchMix = indexOf("pitch_mix");
        static const int iAirOn = indexOf("air_on"), iBreath = indexOf("air_breath"), iWidthAir = indexOf("air_width"), iLofi = indexOf("air_lofi"),
                         iAirSmooth = indexOf("air_smooth"), iAirThresh = indexOf("air_thresh");
        on_[kShape] = v(values, iShapeOn) > 0.5f;
        on_[kPitch] = v(values, iPitchOn) > 0.5f;
        on_[kAir] = v(values, iAirOn) > 0.5f;
        mix_[kShape] = mix_[kAir] = 1.0f;
        mix_[kPitch] = 1.0f;  // PITCH blends inside (aligned with the vocoder's latency)
        if (changed({iTilt, iDrive, iHarm, iShapeType}))
            shape_.setParams({2.0f * v(values, iTilt) - 1.0f, v(values, iDrive), v(values, iHarm), Shape::Type(std::clamp(int(v(values, iShapeType)), 0, 5))});
        if (changed({iSemi, iShift, iPitchMix})) pitch_.setParams({pitchSemitones(v(values, iSemi)), shiftHz(v(values, iShift)), v(values, iPitchMix)});
        if (changed({iBreath, iWidthAir, iLofi, iAirSmooth, iAirThresh}))
            air_.setParams({v(values, iBreath), v(values, iWidthAir), v(values, iLofi), airSmoothMs(v(values, iAirSmooth)) / 1000.0f,
                            airThresholdDb(v(values, iAirThresh))});
        static const int iGrainOn = indexOf("grain_on"), iAlgo = indexOf("grain_algo"), iEnv = indexOf("grain_env"), iDensity = indexOf("grain_density"),
                         iChance = indexOf("grain_chance"), iGrainSize = indexOf("grain_size"), iGrainMix = indexOf("grain_mix"),
                         iInterval0 = indexOf("grain_m12"), iGrainSync = indexOf("grain_sync"), iGrainDiv = indexOf("grain_div"),
                         iGrainFdbk = indexOf("grain_fdbk"), iGrainReverse = indexOf("grain_reverse");
        on_[kGrain] = v(values, iGrainOn) > 0.5f;
        mix_[kGrain] = v(values, iGrainMix);
        grainFeedback_ = 0.95f * v(values, iGrainFdbk);  // 100 % on the knob: just short of runaway
        if (changed({iAlgo, iEnv, iDensity, iChance, iGrainSize, iGrainSync, iGrainDiv, iInterval0, iInterval0 + 1, iInterval0 + 2, iInterval0 + 3,
                     iInterval0 + 4, iInterval0 + 5, iInterval0 + 6, iInterval0 + 7, iInterval0 + 8, iGrainReverse})) {
            GrainCloud::Params p;
            p.algo = GrainCloud::Algo(std::clamp(int(v(values, iAlgo)), 0, 2));
            p.envelope = GrainCloud::Envelope(std::clamp(int(v(values, iEnv)), 0, 3));
            p.density = grainDensity(v(values, iDensity));
            p.chance = v(values, iChance);
            p.sizeMs = grainSizeMs(v(values, iGrainSize));
            p.sync = v(values, iGrainSync) > 0.5f;
            p.reverse = v(values, iGrainReverse);
            p.divisionBeats = kDivisionBeats[size_t(std::clamp(echoDivision(v(values, iGrainDiv)), 0, 13))];
            p.intervals = 0;
            for (int i = 0; i < int(GrainCloud::kIntervals.size()); ++i)
                if (v(values, iInterval0 + i) > 0.5f) p.intervals = uint16_t(p.intervals | (1u << i));  // grain_m12..p12 follow each other
            grain_.setParams(p);
        }
        mix_[kDerez] = v(values, iDerezMix);
        mix_[kStretch] = v(values, iStretchMix);
        mix_[kEcho] = v(values, iEchoMix);
        mix_[kSpace] = v(values, iSpaceMix);
        mix_[kFocus] = 1.0f;  // FOCUS has its own mix inside
        mix_[kColor] = 1.0f;
        mixTarget_ = v(values, iMix);
        outTarget_ = dbToGain((2.0f * v(values, iOut) - 1.0f) * 12.0f);
        bypassTarget_ = v(values, iBypass) > 0.5f ? 1.0f : 0.0f;
        offsetTarget_ = offsetMs(v(values, iOffset)) * 0.001f * sampleRate_;
        if (changed({iGlue})) glue_.setAmount(v(values, iGlue));
        // GAIN STAGING: the wet follows the input's loudness both ways (-12 .. +12 dB);
        // off, it is only lifted back when the chain lost level (0 .. +12 dB).
        makeupFloor_ = v(values, iStaging) > 0.5f ? 0.25f : 1.0f;
        tonalBalance_ = 2.0f * v(values, iTonal) - 1.0f;
        tonalPre_ = v(values, iPlace) < 0.5f;
        freeze_ = v(values, iFreeze) > 0.5f;

        if (changed({iInterval, iCrush, iBits, iDerezSmooth})) {
            PitchedDerez::Params p;
            p.interval = derezSemitones(v(values, iInterval));
            p.crush = derezCrush(v(values, iCrush));
            p.bits = derezBits(v(values, iBits));
            p.smooth = v(values, iDerezSmooth);
            p.mix = 1.0f;  // the module ramp does the mix
            derez_.setParams(p);
        }

        if (changed({iFactor})) {
            CaptureStretch::Params p;
            p.mode = CaptureStretch::Mode::Blur;
            p.stretch = stretchFactor(v(values, iFactor));
            stretch_.setParams(p);
        }

        if (changed({iEchoMode, iSync, iTime, iDiv, iFdbk}) || std::abs(bpm - lastBpm_) > 1.0e-3f) {
            static constexpr TapeEcho::Division kDivisions[] = {
                TapeEcho::Division::ThirtySecond, TapeEcho::Division::SixteenthTriplet, TapeEcho::Division::Sixteenth,
                TapeEcho::Division::SixteenthDotted, TapeEcho::Division::EighthTriplet, TapeEcho::Division::Eighth,
                TapeEcho::Division::EighthDotted, TapeEcho::Division::QuarterTriplet, TapeEcho::Division::Quarter,
                TapeEcho::Division::QuarterDotted, TapeEcho::Division::HalfTriplet, TapeEcho::Division::Half,
                TapeEcho::Division::HalfDotted, TapeEcho::Division::Whole};
            TapeEcho::Params p;
            p.pingPong = v(values, iEchoMode) > 0.5f;
            p.sync = v(values, iSync) > 0.5f;
            p.bpm = bpm;
            p.timeMs = echoTimeMs(v(values, iTime));
            p.division = kDivisions[std::clamp(echoDivision(v(values, iDiv)), 0, 13)];
            p.feedback = v(values, iFdbk);
            p.wow = 0.0f;  // a steady tape: no wow or flutter, the repeats stay in tune
            p.flutter = 0.0f;
            echo_.setParams(p);
            lastBpm_ = bpm;
        }

        spaceType_ = int(v(values, iType));
        plateSelected_ = spaceType_ == 1;
        if (changed({iType, iSize, iDecay, iTone, iMod, iPre, iShimmer, iShimmerPitch, iWidth, iFreeze})) {
            const int type = int(v(values, iType));
            const float tone = v(values, iTone);
            if (plateSelected_) {
                PlateReverb::Params p;
                p.decay = 0.3f + 0.69f * v(values, iDecay);
                p.damping = 1.0f - tone;
                p.size = 0.5f + 1.5f * v(values, iSize);
                p.modDepth = v(values, iMod);
                p.predelayMs = predelayMs(v(values, iPre));
                p.freeze = freeze_;
                plate_.setParams(p);
            } else {
                // Room, Hall, Cathedral: the type sets the character, SIZE stays free.
                SpaceReverb::Params p;
                float hfRatio = 0.5f;
                if (type == 0) {
                    p.earlyLevel = 0.45f;
                    hfRatio = 0.6f;
                } else if (type == 4) {
                    p.earlyLevel = 0.0f;  // BLOOM: nothing at the attack, the feeder brings it in
                } else if (type == 3) {
                    hfRatio = 0.35f;
                    p.hfCrossoverHz = 3000.0f;
                }
                p.size = v(values, iSize);
                p.decaySeconds = decaySeconds(v(values, iDecay));
                p.hfRatio = std::clamp(hfRatio * (0.4f + 1.2f * tone), 0.05f, 1.0f);
                p.hfCrossoverHz *= 0.6f + 0.8f * tone;
                p.modDepth = v(values, iMod);
                p.predelayMs = predelayMs(v(values, iPre));
                p.shimmer = v(values, iShimmer);
                p.shimmerSemitones = std::round((2.0f * v(values, iShimmerPitch) - 1.0f) * 24.0f);
                p.width = 1.5f * v(values, iWidth);
                p.freeze = freeze_;
                if (type == 4) {
                    // The swell grows with DECAY and SIZE.
                    BloomFeeder::Params b;
                    b.size = p.size;
                    b.swellSeconds = std::clamp(0.3f * p.decaySeconds, 1.0f, 4.0f) * (0.6f + 0.8f * p.size);
                    bloom_.setParams(b);
                } else if (type == 5) {
                    // RESONATOR: the combs ring for DECAY, the reverb behind them is shorter.
                    resonatorParams_.size = p.size;
                    resonatorParams_.decaySeconds = std::min(p.decaySeconds, 30.0f);
                    resonatorParams_.tone = tone;
                    resonatorParams_.freeze = freeze_;
                    resonator_.setParams(resonatorParams_);
                    p.decaySeconds = std::min(p.decaySeconds * 0.5f, 12.0f);
                    p.earlyLevel = 0.1f;
                }
                space_.setParams(p);
            }
        }

        focusAuto_ = int(v(values, iFocusMode)) == 2;
        if (changed({iFocusMode, iAmount, iFocusDepth, iFocusMix})) {
            ResonantFocus::Params p;
            p.mode = int(v(values, iFocusMode)) == 1 ? ResonantFocus::Mode::Harmonic : ResonantFocus::Mode::Key;
            p.focus = v(values, iAmount);
            p.depth = v(values, iFocusDepth);
            p.mix = v(values, iFocusMix);
            focus_.setParams(p);
        }

        tremSync_ = v(values, iTremSync) > 0.5f;
        tremBeats_ = kDivisionBeats[size_t(std::clamp(echoDivision(v(values, iTremRate)), 0, 13))];
        const float hard = v(values, iMotionOn) > 0.5f ? v(values, iHard) : 0.0f;
        if (changed({iBass, iBody, iMud, iHarsh, iAir, iHpf, iLpf, iTrem, iTremRate, iTremSync, iHard, iMotionOn}) || (tremSync_ && std::abs(bpm - tremBpm_) > 1.0e-3f)) {
            tremBpm_ = bpm;
            Color::Params p;
            p.bass = 2.0f * v(values, iBass) - 1.0f;
            p.body = 2.0f * v(values, iBody) - 1.0f;
            p.mud = 2.0f * v(values, iMud) - 1.0f;
            p.harsh = 2.0f * v(values, iHarsh) - 1.0f;
            p.air = 2.0f * v(values, iAir) - 1.0f;
            p.highpassHz = highpassHz(v(values, iHpf));
            p.lowpassHz = lowpassHz(v(values, iLpf));
            p.tremolo = v(values, iTrem);
            // SYNC: RATE is a division of the tempo (1/32 .. 1/1, dotted, triplets).
            p.tremoloHz = tremSync_ ? std::max(bpm, 20.0f) / 60.0f / tremBeats_ : tremRateHz(v(values, iTremRate));
            p.hard = hard;
            color_.setParams(p);
        }
        tonal_.setParams({tonalBalance_, 0.5f, 1.0f});

        std::copy(values, values + specs.size(), last_.begin());
    }

    // Runs one module on the wet signal with a short crossfade when it is switched on
    // or off: wet = wet + gain * mix * (module(wet) - wet). ECHO adds instead of
    // crossfading (its MIX is the level of the repeats).
    template <typename F>
    void runModule(int m, float* wetL, float* wetR, int n, F&& processModule) {
        const float target = on_[size_t(m)] ? 1.0f : 0.0f;
        float& g = moduleGain_[size_t(m)];
        if (target == 0.0f && g < 1.0e-4f) {
            g = 0.0f;
            return;
        }
        float* outL = buffers_[2].data();
        float* outR = buffers_[3].data();
        processModule(wetL, wetR, outL, outR, n);
        const float mix = mix_[size_t(m)];
        const bool additive = m == kEcho;
        for (int i = 0; i < n; ++i) {
            g += (target - g) * rampCoef_;
            const float k = g * mix;
            if (additive) {
                wetL[i] += k * outL[i];
                wetR[i] += k * outR[i];
            } else {
                wetL[i] += k * (outL[i] - wetL[i]);
                wetR[i] += k * (outR[i] - wetR[i]);
            }
        }
    }

    // TONAL ◂▸ NOISE costs nothing and adds no delay at the centre; away from it, it
    // fades in over ~30 ms.
    void runTonal(float* wetL, float* wetR, int n) {
        const bool active = on_[kColor] && std::abs(tonalBalance_) > 1.0e-3f;
        if (tonalGain_ < 0.0f) tonalGain_ = active ? 1.0f : 0.0f;
        if (!active && tonalGain_ < 1.0e-4f) {
            tonalGain_ = 0.0f;
            return;
        }
        float* outL = buffers_[2].data();
        float* outR = buffers_[3].data();
        tonal_.process(wetL, wetR, outL, outR, n);
        const float target = active ? 1.0f : 0.0f;
        for (int i = 0; i < n; ++i) {
            tonalGain_ += (target - tonalGain_) * rampCoef_;
            wetL[i] += tonalGain_ * (outL[i] - wetL[i]);
            wetR[i] += tonalGain_ * (outR[i] - wetR[i]);
        }
    }

    void processBlock(float* left, float* right, int n) {
        float* wetL = buffers_[0].data();
        float* wetR = buffers_[1].data();
        std::copy(left, left + n, wetL);
        std::copy(right, right + n, wetR);

        midiClasses_.store(notes_.pitchClasses(), std::memory_order_relaxed);
        // FOCUS listens only while it is on (the key detector is an FFT every 1024 samples).
        if (!on_[kFocus]) {
            focusClasses_.store(0, std::memory_order_relaxed);
        } else if (focusAuto_) {
            keys_.analyse(left, right, n);
            focus_.setScale(keys_.pitchClasses());
            focusClasses_.store(keys_.pitchClasses(), std::memory_order_relaxed);
        } else {
            focus_.setScale(0);
            focusClasses_.store(notes_.pitchClasses(), std::memory_order_relaxed);
        }
        // RESONATOR rings on the MIDI notes held, else on what FOCUS AUTO hears, and keeps
        // the last chord when they stop.
        if (spaceType_ == 5 && on_[kSpace]) {
            uint16_t classes = notes_.pitchClasses();
            if (classes == 0 && on_[kFocus] && focusAuto_) classes = keys_.pitchClasses();
            if (classes != 0 && classes != resonatorParams_.classes) {
                resonatorParams_.classes = classes;
                resonator_.setParams(resonatorParams_);
            }
        }
        resonatorClasses_.store(spaceType_ == 5 ? resonatorParams_.classes : 0, std::memory_order_relaxed);

        const uint64_t order = order_.load(std::memory_order_relaxed);
        for (size_t slot = 0; slot < params::kModules.size(); ++slot) {
            switch (int((order >> (4 * slot)) & 15)) {
                case 0: runModule(kShape, wetL, wetR, n, [&](auto... a) { shape_.process(a...); }); break;
                case 2: runModule(kPitch, wetL, wetR, n, [&](auto... a) { pitch_.process(a...); }); break;
                case 7: runModule(kAir, wetL, wetR, n, [&](auto... a) { air_.process(a...); }); break;
                case 1: runModule(kDerez, wetL, wetR, n, [&](auto... a) { derez_.process(a...); }); break;
                case 3: runModule(kStretch, wetL, wetR, n, [&](auto... a) { stretch_.process(a...); }); break;
                case 4: runModule(kEcho, wetL, wetR, n, [&](auto... a) { echo_.process(a...); }); break;
                case 5:
                    if (tonalPre_) runTonal(wetL, wetR, n);
                    runModule(kSpace, wetL, wetR, n, [&](const float* inL, const float* inR, float* outL, float* outR, int count) {
                        if (plateSelected_) return plate_.process(inL, inR, outL, outR, count);
                        if (spaceType_ != 4 && spaceType_ != 5) return space_.process(inL, inR, outL, outR, count);
                        float* preL = preSpace_[0].data();
                        float* preR = preSpace_[1].data();
                        if (spaceType_ == 4) {
                            bloom_.process(inL, inR, preL, preR, count);
                            space_.process(preL, preR, outL, outR, count);
                        } else {
                            // The combs into the reverb, plus a little of them dry for definition.
                            resonator_.process(inL, inR, preL, preR, count);
                            space_.process(preL, preR, outL, outR, count);
                            for (int i = 0; i < count; ++i) outL[i] += 0.35f * preL[i], outR[i] += 0.35f * preR[i];
                        }
                    });
                    break;
                case 6: runModule(kFocus, wetL, wetR, n, [&](auto... a) { focus_.process(a...); }); break;
                case 8: runModule(kColor, wetL, wetR, n, [&](auto... a) { color_.process(a...); }); break;
                case 9: runModule(kGrain, wetL, wetR, n, [&](const float* inL, const float* inR, float* outL, float* outR, int count) {
                        if (grainFeedback_ <= 0.0f) return grain_.process(inL, inR, outL, outR, count);
                        // FEEDBACK: the grains go back in (512 samples later, soft-clipped), so
                        // grains are cut from grains: cascades, then a self-feeding cloud.
                        float* fbL = preSpace_[0].data();
                        float* fbR = preSpace_[1].data();
                        for (int i = 0; i < count; ++i) {
                            const int at = (grainFbPos_ + i - kGrainFbDelay) & kGrainFbMask;
                            fbL[i] = inL[i] + grainFeedback_ * std::tanh(grainFbLine_[0][size_t(at)]);
                            fbR[i] = inR[i] + grainFeedback_ * std::tanh(grainFbLine_[1][size_t(at)]);
                        }
                        grain_.process(fbL, fbR, outL, outR, count);
                        for (int i = 0; i < count; ++i) {
                            grainFbLine_[0][size_t((grainFbPos_ + i) & kGrainFbMask)] = outL[i];
                            grainFbLine_[1][size_t((grainFbPos_ + i) & kGrainFbMask)] = outR[i];
                        }
                        grainFbPos_ = (grainFbPos_ + count) & kGrainFbMask;
                    });
                    break;
            }
        }
        if (!tonalPre_) runTonal(wetL, wetR, n);

        if (mixGain_ < 0.0f) {
            mixGain_ = mixTarget_;
            outGain_ = outTarget_;
        }
        int w = visualWrite_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            mixGain_ += (mixTarget_ - mixGain_) * rampCoef_;
            outGain_ += (outTarget_ - outGain_) * rampCoef_;
            // OFFSET: the wet's right channel slightly late (fractional, glides when moved).
            float wr = wetR[i];
            offsetSamples_ += (offsetTarget_ - offsetSamples_) * rampCoef_;
            offsetLine_[size_t(offsetPos_)] = wr;
            if (offsetSamples_ > 0.01f) {
                const float at = float(offsetPos_) - offsetSamples_ + float(kOffsetSize);
                const int i0 = int(at) % kOffsetSize, i1 = (i0 + 1) % kOffsetSize;
                const float frac = at - std::floor(at);
                wr = offsetLine_[size_t(i0)] + frac * (offsetLine_[size_t(i1)] - offsetLine_[size_t(i0)]);
            }
            offsetPos_ = (offsetPos_ + 1) % kOffsetSize;
            // Level makeup: the modules' losses add up (GRAIN, DEREZ, COLOR's cuts… a preset
            // could lose 10 dB or more). The wet is brought back to the input's loudness,
            // slowly, makeup only (0 .. +12 dB), and the gain holds while the input is
            // silent so tails fade naturally.
            float wl = wetL[i];
            const float dryPower = 0.5f * (left[i] * left[i] + right[i] * right[i]);
            const float wetPower = 0.5f * (wl * wl + wr * wr);
            dryLevel_ += (dryPower - dryLevel_) * levelCoef_;
            wetLevel_ += (wetPower - wetLevel_) * levelCoef_;
            dryFast_ += (dryPower - dryFast_) * gateCoef_;
            if (dryFast_ > 1.0e-6f) {  // -60 dBFS, over 50 ms
                const float wanted = std::clamp(std::sqrt(dryLevel_ / std::max(wetLevel_, 1.0e-12f)), makeupFloor_, 4.0f);
                // Up slowly (1.5 s), down fast (150 ms): a reverb still building never overshoots.
                makeup_ += (wanted - makeup_) * (wanted > makeup_ ? makeupCoef_ : makeupDownCoef_);
            }
            wl *= makeup_;
            wr *= makeup_;
            // HARD's cracks: up to 4x the wet's level, gone in ~16 samples, a little apart in stereo.
            if (hardClick_ != 0.0f) {
                const float c = 4.0f * std::sqrt(wetLevel_) * makeup_ * hardClick_;
                wl += c;
                wr -= 0.7f * c;
                hardClick_ *= -0.62f;
                if (std::abs(hardClick_) < 1.0e-3f) hardClick_ = 0.0f;
            }
            float l = outGain_ * (left[i] + mixGain_ * (wl - left[i]));
            float r = outGain_ * (right[i] + mixGain_ * (wr - right[i]));
            // GLUE: the soft, slow compressor on the whole output.
            if (glue_.active()) glue_.process(l, r);
            // BYPASS: back to the untouched input (30 ms ramp); the chain keeps running.
            bypassGain_ += (bypassTarget_ - bypassGain_) * rampCoef_;
            l += bypassGain_ * (left[i] - l);
            r += bypassGain_ * (right[i] - r);
            safety(l, r);
            left[i] = l;
            right[i] = r;
            visual_[size_t(w)] = 0.5f * (wetL[i] + wetR[i]);
            w = (w + 1) % kVisualSize;
        }
        visualWrite_.store(w, std::memory_order_release);
    }

    // Safety, invisible: a peak limiter at -0.3 dBFS (instant attack, 150 ms release)
    // and a guard against NaN/Inf. A non-finite sample mutes the output for 0.25 s and
    // asks the plugin to reset the engine, so a bug can never reach the ears.
    void safety(float& l, float& r) {
        if (!std::isfinite(l) || !std::isfinite(r)) {
            mute_ = int(0.25f * sampleRate_);
            needsReset_.store(true, std::memory_order_relaxed);
        }
        if (mute_ > 0) {
            --mute_;
            l = r = 0.0f;
            return;
        }
        constexpr float kCeiling = 0.966f;  // -0.3 dBFS
        const float peak = std::max(std::abs(l), std::abs(r));
        const float needed = peak > kCeiling ? kCeiling / peak : 1.0f;
        limiterGain_ = needed < limiterGain_ ? needed : limiterGain_ + (1.0f - limiterGain_) * limiterRelease_;
        l = std::clamp(l * limiterGain_, -kCeiling, kCeiling);
        r = std::clamp(r * limiterGain_, -kCeiling, kCeiling);
    }

public:
    // Set when the safety guard saw a non-finite sample; the plugin then calls prepare().
    bool needsReset() const { return needsReset_.load(std::memory_order_relaxed); }

private:
    float sampleRate_ = 48000.0f;
    Shape shape_;
    GrainCloud grain_;
    Pitch pitch_;
    Air air_;
    float limiterGain_ = 1.0f, limiterRelease_ = 0.001f;
    int mute_ = 0;
    std::atomic<bool> needsReset_{false};
    PitchedDerez derez_;
    CaptureStretch stretch_;
    TapeEcho echo_;
    SpaceReverb space_;
    PlateReverb plate_;
    Glue glue_;
    float makeupFloor_ = 1.0f;
    BloomFeeder bloom_;
    Resonator resonator_;
    Resonator::Params resonatorParams_;
    std::array<std::vector<float>, 2> preSpace_;  // BLOOM / RESONATOR output, into the reverb
    int spaceType_ = 3;
    ResonantFocus focus_;
    KeyDetector keys_;
    NoteSet notes_;
    Color color_;
    TonalNoise tonal_;

    std::array<std::vector<float>, 4> buffers_;
    std::vector<float> last_;
    std::array<bool, kModules> on_{};
    std::array<float, kModules> mix_{}, moduleGain_{};
    float rampCoef_ = 0.001f, mixTarget_ = 0.5f, mixGain_ = -1.0f, outTarget_ = 1.0f, outGain_ = 1.0f;
    float bypassTarget_ = 0.0f, bypassGain_ = 0.0f, offsetTarget_ = 0.0f, offsetSamples_ = 0.0f;
    float dryLevel_ = 0.0f, wetLevel_ = 0.0f, dryFast_ = 0.0f, makeup_ = 1.0f, levelCoef_ = 0.0f, makeupCoef_ = 0.0f, makeupDownCoef_ = 0.0f, gateCoef_ = 0.0f;
    static constexpr int kOffsetSize = 8192;  // > 25 ms at 192 kHz
    std::array<float, kOffsetSize> offsetLine_{};
    int offsetPos_ = 0;
    float tonalBalance_ = 0.0f, tonalGain_ = -1.0f, lastBpm_ = 0.0f;
    bool tonalPre_ = false, freeze_ = false, plateSelected_ = false, focusAuto_ = false;


    std::array<float, kVisualSize> visual_{};
    std::atomic<int> visualWrite_{0};
    std::atomic<uint16_t> focusClasses_{0}, midiClasses_{0}, resonatorClasses_{0};
    std::atomic<uint64_t> order_{defaultOrder()};
    std::array<Lfo, 12> lanes_;
    std::array<float, 12> rateOffset_{}, depthOffset_{};
    std::vector<float> hardPrevious_;  // each knob's last HARD step (-1 = none yet)
    float hardClick_ = 0.0f;
    static constexpr int kGrainFbDelay = kMaxBlock, kGrainFbMask = 2 * kMaxBlock - 1;
    std::array<std::array<float, 2 * kMaxBlock>, 2> grainFbLine_{};
    int grainFbPos_ = 0;
    float grainFeedback_ = 0.0f;
    bool tremSync_ = false;
    float tremBeats_ = 1.0f, tremBpm_ = 0.0f;  // what other lanes did to each lane's RATE / DEPTH
    std::array<int, 12> laneRate_{}, laneShape_{}, laneDepth_{}, laneSync_{};
    std::array<std::array<std::atomic<int>, kLaneTargets>, 12> laneTarget_{};
    std::array<std::atomic<float>, 12> laneValues_{};
    std::array<std::array<std::atomic<float>, kLaneTargets>, 12> laneTargets_{};
    std::vector<float> modulated_;
    bool motionWasOn_ = false;
};

}  // namespace stasis
