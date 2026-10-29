#pragma once

#include <stasis/air.h>
#include <stasis/capture_stretch.h>
#include <stasis/color.h>
#include <stasis/key_detector.h>
#include <stasis/lfo.h>
#include <stasis/note_set.h>
#include <stasis/pitched_derez.h>
#include <stasis/pitch.h>
#include <stasis/plate_reverb.h>
#include <stasis/resonant_focus.h>
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
            laneTarget_[i].store(params::kDefaultTargets[i][0] ? params::indexOf(params::kDefaultTargets[i]) : -1);
    }

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        shape_.prepare(sampleRate);
        pitch_.prepare(sampleRate);
        air_.prepare(sampleRate);
        limiterGain_ = 1.0f;
        limiterRelease_ = smoothingCoef(0.15f, sampleRate);
        mute_ = 0;
        needsReset_.store(false);
        derez_.prepare(sampleRate);
        derez_.setRoot(110.0f);  // A2, when no MIDI note is held
        stretch_.prepare(sampleRate);
        echo_.prepare(sampleRate);
        space_.prepare(sampleRate);
        plate_.prepare(sampleRate);
        focus_.prepare(sampleRate);
        keys_.prepare(sampleRate);
        keys_.setParams({1.5f, 0.45f, 7});
        color_.prepare(sampleRate);
        tonal_.prepare(sampleRate);
        for (auto& b : buffers_) b.assign(kMaxBlock, 0.0f);
        modulated_.assign(params::all().size(), 0.0f);
        for (size_t i = 0; i < lanes_.size(); ++i) {
            lanes_[i].prepare(sampleRate, uint32_t(i + 1) * 7919u);
            laneRate_[i] = params::indexOf(params::laneId(int(i), "rate"));
            laneShape_[i] = params::indexOf(params::laneId(int(i), "shape"));
            laneDepth_[i] = params::indexOf(params::laneId(int(i), "depth"));
        }
        rampCoef_ = smoothingCoef(0.03f, sampleRate);
        for (auto& g : moduleGain_) g = 0.0f;
        tonalGain_ = mixGain_ = outGain_ = -1.0f;  // snap on the first block
        last_.assign(params::all().size(), NAN);
        stretchMode_ = -1;
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
    void process(const float* values, float bpm, float* left, float* right, int numSamples) {
        for (int start = 0; start < numSamples; start += kMaxBlock) {
            const int n = std::min(kMaxBlock, numSamples - start);
            update(modulate(values, n), bpm);
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
        for (uint64_t i = 0; i < params::kModules.size(); ++i) order |= i << (4 * i);
        return order;
    }
    void setOrder(uint64_t order) { order_.store(order, std::memory_order_relaxed); }
    uint64_t order() const { return order_.load(std::memory_order_relaxed); }

    // The 12 MOTION lanes, -1..1 (for the UI traces).
    float laneValue(int lane) const { return laneValues_[size_t(lane)].load(std::memory_order_relaxed); }
    // Where each lane has moved its target knob right now (0..1), for the UI.
    float laneTarget(int lane) const { return laneTargets_[size_t(lane)].load(std::memory_order_relaxed); }
    // Which parameter (index in params::all(), -1 = none) a lane moves. Any thread.
    void setLaneTarget(int lane, int paramIndex) { laneTarget_[size_t(lane)].store(paramIndex, std::memory_order_relaxed); }
    int laneTargetIndex(int lane) const { return laneTarget_[size_t(lane)].load(std::memory_order_relaxed); }

    // Pitch classes FOCUS is following (MIDI, or what AUTO hears). Bit n = class n.
    uint16_t focusClasses() const { return focusClasses_.load(std::memory_order_relaxed); }

private:
    enum Module { kDerez, kStretch, kEcho, kSpace, kFocus, kColor, kShape, kPitch, kAir, kModules };

    float v(const float* values, int index) const { return values[index]; }

    // MOTION: each lane sweeps its target between the user's setting (depth 0) and the
    // knob's whole travel (depth 100 %): value = set + depth · (lfo01 − set).
    const float* modulate(const float* values, int n) {
        static const int iOn = params::indexOf("motion_on"), iAmount = params::indexOf("motion_amount"), iDrift = params::indexOf("motion_drift");
        const bool on = values[iOn] > 0.5f;
        if (!on) {  // MOTION off: nothing runs, the parameters pass as they are
            if (motionWasOn_) for (auto& v : laneValues_) v.store(0.0f, std::memory_order_relaxed);
            motionWasOn_ = false;
            return values;
        }
        motionWasOn_ = true;
        std::copy(values, values + modulated_.size(), modulated_.begin());
        const float amount = values[iAmount], drift = params::driftScale(values[iDrift]);
        for (size_t i = 0; i < lanes_.size(); ++i) {
            const float depth = values[laneDepth_[i]];
            const int targetIndex = laneTarget_[i].load(std::memory_order_relaxed);
            if (depth <= 0.0f || targetIndex < 0) {  // an empty lane, or one at 0 %, does not run
                laneValues_[i].store(0.0f, std::memory_order_relaxed);
                continue;
            }
            Lfo& lfo = lanes_[i];
            lfo.setRate(params::laneRateHz(values[laneRate_[i]]) * drift);
            lfo.setShape(values[laneShape_[i]]);
            const float x = lfo.advance(n);
            laneValues_[i].store(x, std::memory_order_relaxed);
            float& target = modulated_[size_t(targetIndex)];
            target = std::clamp(target + amount * depth * (0.5f * (x + 1.0f) - target), 0.0f, 1.0f);
        }
        for (size_t i = 0; i < lanes_.size(); ++i) {
            const int t = laneTarget_[i].load(std::memory_order_relaxed);
            laneTargets_[i].store(t >= 0 ? modulated_[size_t(t)] : 0.0f, std::memory_order_relaxed);
        }
        return modulated_.data();
    }

    // Push the parameters to the modules, only those that changed (some setParams
    // recompute filters and gains).
    void update(const float* values, float bpm) {
        using namespace params;
        static const auto& specs = all();
        static const int iMix = indexOf("mix"), iOut = indexOf("out"), iFreeze = indexOf("freeze");
        static const int iDerezOn = indexOf("derez_on"), iInterval = indexOf("derez_interval"), iCrush = indexOf("derez_crush"),
                         iBits = indexOf("derez_bits"), iDerezMix = indexOf("derez_mix");
        static const int iStretchOn = indexOf("stretch_on"), iStretchMode = indexOf("stretch_mode"), iFactor = indexOf("stretch_factor"),
                         iLoop = indexOf("stretch_loop"), iStretchMix = indexOf("stretch_mix");
        static const int iEchoOn = indexOf("echo_on"), iEchoMode = indexOf("echo_mode"), iSync = indexOf("echo_sync"), iTime = indexOf("echo_time"),
                         iDiv = indexOf("echo_div"), iFdbk = indexOf("echo_fdbk"), iEchoMix = indexOf("echo_mix");
        static const int iSpaceOn = indexOf("space_on"), iType = indexOf("space_type"), iSize = indexOf("space_size"), iDecay = indexOf("space_decay"),
                         iTone = indexOf("space_tone"), iMod = indexOf("space_mod"), iPre = indexOf("space_pre"), iSpaceMix = indexOf("space_mix"),
                         iShimmer = indexOf("shimmer_depth"), iShimmerPitch = indexOf("shimmer_pitch"), iWidth = indexOf("space_width");
        static const int iFocusOn = indexOf("focus_on"), iFocusMode = indexOf("focus_mode"), iAmount = indexOf("focus_amount"),
                         iFocusDepth = indexOf("focus_depth"), iFocusMix = indexOf("focus_mix");
        static const int iColorOn = indexOf("color_on"), iBass = indexOf("color_bass"), iBody = indexOf("color_body"), iMud = indexOf("color_mud"),
                         iHarsh = indexOf("color_harsh"), iAir = indexOf("color_air"), iHpf = indexOf("color_hpf"), iLpf = indexOf("color_lpf"),
                         iTonal = indexOf("tonal"), iPlace = indexOf("tonal_place");

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
        static const int iShapeOn = indexOf("shape_on"), iTilt = indexOf("shape_tilt"), iDrive = indexOf("shape_drive"), iHarm = indexOf("shape_harm");
        static const int iPitchOn = indexOf("pitch_on"), iSemi = indexOf("pitch_semi"), iShift = indexOf("pitch_shift"), iPitchMix = indexOf("pitch_mix");
        static const int iAirOn = indexOf("air_on"), iBreath = indexOf("air_breath"), iWidthAir = indexOf("air_width"), iLofi = indexOf("air_lofi");
        on_[kShape] = v(values, iShapeOn) > 0.5f;
        on_[kPitch] = v(values, iPitchOn) > 0.5f;
        on_[kAir] = v(values, iAirOn) > 0.5f;
        mix_[kShape] = mix_[kAir] = 1.0f;
        mix_[kPitch] = v(values, iPitchMix);
        if (changed({iTilt, iDrive, iHarm})) shape_.setParams({2.0f * v(values, iTilt) - 1.0f, v(values, iDrive), v(values, iHarm)});
        if (changed({iSemi, iShift})) pitch_.setParams({pitchSemitones(v(values, iSemi)), shiftHz(v(values, iShift))});
        if (changed({iBreath, iWidthAir, iLofi})) air_.setParams({v(values, iBreath), v(values, iWidthAir), v(values, iLofi)});
        mix_[kDerez] = v(values, iDerezMix);
        mix_[kStretch] = v(values, iStretchMix);
        mix_[kEcho] = v(values, iEchoMix);
        mix_[kSpace] = v(values, iSpaceMix);
        mix_[kFocus] = 1.0f;  // FOCUS has its own mix inside
        mix_[kColor] = 1.0f;
        mixTarget_ = v(values, iMix);
        outTarget_ = dbToGain((2.0f * v(values, iOut) - 1.0f) * 12.0f);
        tonalBalance_ = 2.0f * v(values, iTonal) - 1.0f;
        tonalPre_ = v(values, iPlace) < 0.5f;
        freeze_ = v(values, iFreeze) > 0.5f;

        if (changed({iInterval, iCrush, iBits})) {
            PitchedDerez::Params p;
            p.interval = derezSemitones(v(values, iInterval));
            p.crush = v(values, iCrush);
            p.bits = derezBits(v(values, iBits));
            p.mix = 1.0f;  // the module ramp does the mix
            derez_.setParams(p);
        }

        const int mode = int(v(values, iStretchMode));
        if (changed({iStretchMode, iFactor, iLoop})) {
            CaptureStretch::Params p;
            p.mode = mode == 1 ? CaptureStretch::Mode::Capture : CaptureStretch::Mode::Smear;
            p.stretch = stretchFactor(v(values, iFactor));
            p.captureSeconds = captureSeconds(v(values, iLoop));
            stretch_.setParams(p);
        }
        // Switching to CAPTURE, or pressing RECAPTURE, freezes the last LOOP seconds.
        static const int iGrab = indexOf("stretch_grab");
        const bool grab = v(values, iGrab) > 0.5f;
        if (mode == 1 && ((stretchMode_ != 1 && stretchMode_ != -1) || (grab && !grab_))) stretch_.capture();
        stretchMode_ = mode;
        grab_ = grab;

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

        plateSelected_ = int(v(values, iType)) == 1;
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

        if (changed({iBass, iBody, iMud, iHarsh, iAir, iHpf, iLpf})) {
            Color::Params p;
            p.bass = 2.0f * v(values, iBass) - 1.0f;
            p.body = 2.0f * v(values, iBody) - 1.0f;
            p.mud = v(values, iMud);
            p.harsh = v(values, iHarsh);
            p.air = 2.0f * v(values, iAir) - 1.0f;
            p.highpassHz = highpassHz(v(values, iHpf));
            p.lowpassHz = lowpassHz(v(values, iLpf));
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
                    runModule(kSpace, wetL, wetR, n, [&](auto... a) { plateSelected_ ? plate_.process(a...) : space_.process(a...); });
                    break;
                case 6: runModule(kFocus, wetL, wetR, n, [&](auto... a) { focus_.process(a...); }); break;
                case 8: runModule(kColor, wetL, wetR, n, [&](auto... a) { color_.process(a...); }); break;
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
            float l = outGain_ * (left[i] + mixGain_ * (wetL[i] - left[i]));
            float r = outGain_ * (right[i] + mixGain_ * (wetR[i] - right[i]));
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
    float tonalBalance_ = 0.0f, tonalGain_ = -1.0f, lastBpm_ = 0.0f;
    bool tonalPre_ = false, freeze_ = false, plateSelected_ = false, focusAuto_ = false;
    int stretchMode_ = -1;
    bool grab_ = false;

    std::array<float, kVisualSize> visual_{};
    std::atomic<int> visualWrite_{0};
    std::atomic<uint16_t> focusClasses_{0};
    std::atomic<uint64_t> order_{defaultOrder()};
    std::array<Lfo, 12> lanes_;
    std::array<int, 12> laneRate_{}, laneShape_{}, laneDepth_{};
    std::array<std::atomic<int>, 12> laneTarget_{};
    std::array<std::atomic<float>, 12> laneValues_{}, laneTargets_{};
    std::vector<float> modulated_;
    bool motionWasOn_ = false;
};

}  // namespace stasis
