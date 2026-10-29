#pragma once

#include <stasis/capture_stretch.h>
#include <stasis/color.h>
#include <stasis/key_detector.h>
#include <stasis/note_set.h>
#include <stasis/pitched_derez.h>
#include <stasis/plate_reverb.h>
#include <stasis/resonant_focus.h>
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
//  IN ─┬─────────────────────────────────────────────────────────────────────────┬─ MIX ─ OUT
//      └ DEREZ ► STRETCH ► ECHO ► [TONAL pre] ► SPACE ► FOCUS ► COLOR ► [TONAL post] ┘
//
// The dry path is never delayed. Modules with latency (STRETCH, TONAL) only delay the
// wet, where it is musically invisible, so the plugin reports zero latency.
class Engine {
public:
    static constexpr int kMaxBlock = 512;

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
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
            update(values, bpm);
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
    // Pitch classes FOCUS is following (MIDI, or what AUTO hears). Bit n = class n.
    uint16_t focusClasses() const { return focusClasses_.load(std::memory_order_relaxed); }

private:
    enum Module { kDerez, kStretch, kEcho, kSpace, kFocus, kColor, kModules };

    float v(const float* values, int index) const { return values[index]; }

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
                         iDiv = indexOf("echo_div"), iFdbk = indexOf("echo_fdbk"), iWow = indexOf("echo_wow"), iEchoMix = indexOf("echo_mix");
        static const int iSpaceOn = indexOf("space_on"), iType = indexOf("space_type"), iSize = indexOf("space_size"), iDecay = indexOf("space_decay"),
                         iTone = indexOf("space_tone"), iMod = indexOf("space_mod"), iPre = indexOf("space_pre"), iSpaceMix = indexOf("space_mix"),
                         iShimmer = indexOf("shimmer_depth"), iShimmerPitch = indexOf("shimmer_pitch"), iWidth = indexOf("space_width");
        static const int iFocusOn = indexOf("focus_on"), iFocusMode = indexOf("focus_mode"), iAmount = indexOf("focus_amount"),
                         iFocusDepth = indexOf("focus_depth"), iFocusMix = indexOf("focus_mix");
        static const int iColorOn = indexOf("color_on"), iBass = indexOf("color_bass"), iMud = indexOf("color_mud"), iAir = indexOf("color_air"),
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
            p.interval = kIntervals[size_t(std::clamp(int(v(values, iInterval)), 0, int(kIntervals.size()) - 1))];
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
        // Switching to CAPTURE freezes the last LOOP seconds.
        if (mode == 1 && stretchMode_ != 1 && stretchMode_ != -1) stretch_.capture();
        stretchMode_ = mode;

        if (changed({iEchoMode, iSync, iTime, iDiv, iFdbk, iWow}) || std::abs(bpm - lastBpm_) > 1.0e-3f) {
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
            p.wow = v(values, iWow);
            p.flutter = 0.75f * v(values, iWow);
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

        if (changed({iBass, iMud, iAir})) color_.setParams({2.0f * v(values, iBass) - 1.0f, v(values, iMud), 2.0f * v(values, iAir) - 1.0f});
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

        if (focusAuto_) {
            keys_.analyse(left, right, n);
            focus_.setScale(keys_.pitchClasses());
            focusClasses_.store(keys_.pitchClasses(), std::memory_order_relaxed);
        } else {
            focus_.setScale(0);
            focusClasses_.store(notes_.pitchClasses(), std::memory_order_relaxed);
        }

        runModule(kDerez, wetL, wetR, n, [&](auto... a) { derez_.process(a...); });
        runModule(kStretch, wetL, wetR, n, [&](auto... a) { stretch_.process(a...); });
        runModule(kEcho, wetL, wetR, n, [&](auto... a) { echo_.process(a...); });
        if (tonalPre_) runTonal(wetL, wetR, n);
        runModule(kSpace, wetL, wetR, n, [&](auto... a) { plateSelected_ ? plate_.process(a...) : space_.process(a...); });
        runModule(kFocus, wetL, wetR, n, [&](auto... a) { focus_.process(a...); });
        runModule(kColor, wetL, wetR, n, [&](auto... a) { color_.process(a...); });
        if (!tonalPre_) runTonal(wetL, wetR, n);

        if (mixGain_ < 0.0f) {
            mixGain_ = mixTarget_;
            outGain_ = outTarget_;
        }
        int w = visualWrite_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            mixGain_ += (mixTarget_ - mixGain_) * rampCoef_;
            outGain_ += (outTarget_ - outGain_) * rampCoef_;
            left[i] = outGain_ * (left[i] + mixGain_ * (wetL[i] - left[i]));
            right[i] = outGain_ * (right[i] + mixGain_ * (wetR[i] - right[i]));
            visual_[size_t(w)] = 0.5f * (wetL[i] + wetR[i]);
            w = (w + 1) % kVisualSize;
        }
        visualWrite_.store(w, std::memory_order_release);
    }

    float sampleRate_ = 48000.0f;
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

    std::array<float, kVisualSize> visual_{};
    std::atomic<int> visualWrite_{0};
    std::atomic<uint16_t> focusClasses_{0};
};

}  // namespace stasis
