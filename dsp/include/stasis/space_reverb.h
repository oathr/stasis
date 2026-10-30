#pragma once

#include <array>

#include "color.h"
#include "common.h"
#include "lfo.h"
#include "spectral_pitch_shifter.h"

namespace stasis {

// SPACE reverb: Room → Hall → Cathedral, plus Shimmer and Freeze. Design choices:
//  - Input diffusion is an 8-channel diffuser (4 stages of stratified random delays,
//    shuffle + polarity flips + Hadamard; after Signalsmith, "Let's write a reverb").
//    Echo density builds in a few ms instead of a grainy cluster of discrete echoes.
//  - Every FDN line carries a modulated allpass (Lexicon / Dattorro practice), so the
//    density keeps growing in the tail instead of settling into a fixed pattern.
//  - Feedback matrix is Householder followed by a rotation of the lines: lossless, O(N),
//    and no line feeds back into itself (a plain Householder sends 87 % of each line back
//    to itself, and each line then rings as a comb: the "metallic" small hall).
//  - Line modulation is smooth random rather than sine: breaks metallic modes without
//    the periodic chorus wobble. Its depth is derived from the decay time so the tail's
//    total pitch drift stays a few cents, short room or endless cathedral alike.
//  - Stereo outputs use two orthogonal tap vectors: fully decorrelated, wide, mono-safe.
//  - Shimmer re-pitches half of the feedback through a phase vocoder: exact intervals
//    (-24..+24), no granular sidebands, so the cascade stays in tune.
//  - Lows decay *faster* by default (bassMultiplier < 1) and the input is high-passed:
//    the main source of mud in long ambient tails.
//  - No treble lost on the way round: a cubic interpolated read loses a little treble on
//    every pass (two per pass cost ~2 dB at 12 kHz), which capped the tail's brightness
//    whatever TONE said. The modulated read is now a first-order allpass interpolation
//    (flat in level) and the in-line allpass reads at whole samples (not modulated any
//    more: the line carries the movement).
//  - TONE (tilt) also shapes the sound going in: a shelf at 3 kHz (-9 .. +5 dB) and one at
//    250 Hz (+3 .. -3 dB), heard from the first reflection on.
class SpaceReverb {
public:
    static constexpr int kLines = 16;

    struct Params {
        float size = 0.7f;             // 0 = room .. 1 = cathedral
        float decaySeconds = 8.0f;     // mid-band RT60, INFINITY = endless
        float bassMultiplier = 0.8f;   // RT60 multiplier below lowCrossoverHz
        float lowCrossoverHz = 300.0f;
        float hfCrossoverHz = 4500.0f;
        float hfRatio = 0.5f;          // RT60 multiplier above hfCrossoverHz
        float modDepth = 0.5f;         // 0..1
        float modRate = 0.8f;          // Hz, average of the random modulators
        float diffusion = 0.8f;        // 0..1, input and in-loop allpass gains
        float earlyLevel = 0.25f;      // diffuser output mixed in as early sound
        float predelayMs = 20.0f;
        float inputHighpassHz = 60.0f;
        float width = 1.0f;            // 0 mono .. 1 natural .. 1.5 extra wide
        float tilt = 0.0f;             // TONE on the input: -1 dark and warm .. 1 bright and lean
        float shimmer = 0.0f;          // DEPTH 0..1: share of the feedback that gets re-pitched
        float shimmerSemitones = 12.0f;  // PITCH -24..+24
        bool freeze = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        Rng rng(0x5BACE);

        // Diffuser: stage s spans kStageMs[s] (scaled by size up to 1.5x); each channel
        // gets a random delay inside its own slice of that span (stratified).
        for (int s = 0; s < kStages; ++s) {
            for (int c = 0; c < kDiffChannels; ++c) {
                diffusers_[size_t(s)][size_t(c)].prepare(int(kStageMs[size_t(s)] * 1.5f * 0.001f * sampleRate) + 8);
                diffuserSlot_[size_t(s)][size_t(c)] = (float(c) + rng.uniform()) / float(kDiffChannels);
                diffuserFlip_[size_t(s)][size_t(c)] = (rng.next() & 1) ? 1.0f : -1.0f;
            }
            // Fisher-Yates shuffle of the channels for this stage.
            auto& perm = diffuserShuffle_[size_t(s)];
            for (int c = 0; c < kDiffChannels; ++c) perm[size_t(c)] = c;
            for (int c = kDiffChannels - 1; c > 0; --c) std::swap(perm[size_t(c)], perm[rng.next() % uint32_t(c + 1)]);
        }

        const int maxLine = int((kMaxLineMs + 6.0f * kMaxModMs) * 0.001f * sampleRate) + 8;
        const int maxAllpass = int(kMaxAllpassMs * 0.001f * sampleRate) + 8;
        for (int i = 0; i < kLines; ++i) {
            const size_t k = size_t(i);
            lines_[k].prepare(maxLine);
            allpasses_[k].prepare(maxAllpass);
            lengthJitter_[k] = rng.bipolar() * 0.35f;
            allpassFraction_[k] = 0.25f + 0.75f * rng.uniform();
            modulators_[k].prepare(sampleRate, 101 + uint32_t(i));
            modulators_[k].setShape(LfoShape::RandomSmooth);
            modRateJitter_[k] = 0.6f + 0.8f * rng.uniform();
        }
        for (int c = 0; c < 2; ++c) predelay_[c].prepare(int(0.5f * sampleRate) + 8);
        // The same window in time at any rate (2048 at 44.1 / 48 kHz, 4096 at 96 kHz).
        int fft = 2048;
        while (fft * 48000.0f < 0.75f * 2048.0f * sampleRate && fft < 8192) fft <<= 1;
        shifter_.prepare(sampleRate, fft);

        setParams(params_);
        lengths_ = targetLengths_;
        updateLoopGains();
        snapNextParams_ = true;
    }

    void setParams(const Params& p) {
        params_ = p;
        const float minMs = 10.0f + 50.0f * p.size;
        const float maxMs = 30.0f + 170.0f * p.size;
        for (int i = 0; i < kLines; ++i) {
            const size_t k = size_t(i);
            const float x = std::clamp((float(kOrder[k]) + lengthJitter_[k]) / float(kLines - 1), 0.0f, 1.0f);
            targetLengths_[k] = std::round(minMs * std::pow(maxMs / minMs, x) * 0.001f * sampleRate_);
            // Whole samples: the allpass reads exactly.
            allpassLengths_[k] = std::round((2.0f + 10.0f * allpassFraction_[k]) * (0.5f + p.size) * 0.001f * sampleRate_);
            allpasses_[k].delay = allpassLengths_[k];
            allpasses_[k].gain = 0.35f + 0.3f * p.diffusion;
            modulators_[k].setRate(p.modRate * modRateJitter_[k]);
            lowSplit_[k].setCutoff(p.lowCrossoverHz, sampleRate_);
            highSplit_[k].setCutoff(p.hfCrossoverHz, sampleRate_);
        }
        const float stageScale = 0.5f + p.size;
        for (int s = 0; s < kStages; ++s)
            for (int c = 0; c < kDiffChannels; ++c)
                diffuserTarget_[size_t(s)][size_t(c)] = float(std::max(
                    2, int(diffuserSlot_[size_t(s)][size_t(c)] * kStageMs[size_t(s)] * stageScale * 0.001f * sampleRate_)));
        for (auto& hp : inputHighpass_) hp.setCutoff(p.inputHighpassHz, sampleRate_);
        const float tilt = std::clamp(p.tilt, -1.0f, 1.0f);
        for (int c = 0; c < 2; ++c) {
            inputLow_[c].set(SvfEq::Type::LowShelf, 250.0f, 0.5f, -3.0f * tilt, sampleRate_);
            inputHigh_[c].set(SvfEq::Type::HighShelf, 3000.0f, 0.5f, tilt < 0.0f ? 9.0f * tilt : 5.0f * tilt, sampleRate_);
        }
        const float semitones = std::clamp(p.shimmerSemitones, -24.0f, 24.0f);
        shifter_.setSemitones(semitones);
        // Upward cascades lose their mud, downward ones keep their depth.
        // 12 dB/oct on both sides (a one-pole let the cascade turn harsh pass after pass);
        // the top follows TONE: 6 kHz upwards at the centre, 3 to 12 kHz.
        const float shimmerTop = (semitones >= 0.0f ? 6000.0f : 12000.0f) * std::pow(2.0f, std::clamp(p.tilt, -1.0f, 1.0f));
        shimmerHp_.set(SvfEq::Type::Highpass, semitones >= 0.0f ? 250.0f : 40.0f, 0.707f, 0.0f, sampleRate_);
        shimmerLp_.set(SvfEq::Type::Lowpass, std::min(shimmerTop, 0.45f * sampleRate_), 0.707f, 0.0f, sampleRate_);
        // The first settings after prepare() apply at once: gliding the line lengths
        // there would bend the pitch of the whole start of the tail (Doppler).
        if (snapNextParams_) {
            lengths_ = targetLengths_;
            allpassNow_ = allpassLengths_;
            diffuserDelay_ = diffuserTarget_;
            predelayNow_ = p.predelayMs * 0.001f * sampleRate_;
            updateLoopGains();
            mod_ = modDepth_;  // start the modulators at their centre
            snapNextParams_ = false;
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float glide = smoothingCoef(0.3f, sampleRate_, numSamples);
        // SIZE moves every length: they all glide, sample by sample (a step per block is a
        // click, and inside the loop it goes round and round), and settle on whole samples.
        const float perSample = 1.0f / float(std::max(numSamples, 1));
        const auto ramp = [&](float now, float target) {
            float end = now + (target - now) * glide;
            if (std::abs(target - end) < 1.0e-3f) end = target;
            return (end - now) * perSample;
        };
        for (int i = 0; i < kLines; ++i) {
            const size_t k = size_t(i);
            lengthStep_[k] = ramp(lengths_[k], targetLengths_[k]);
            allpassStep_[k] = ramp(allpassNow_[k], allpassLengths_[k]);
        }
        for (int s = 0; s < kStages; ++s)
            for (int c = 0; c < kDiffChannels; ++c)
                diffuserStep_[size_t(s)][size_t(c)] = ramp(diffuserDelay_[size_t(s)][size_t(c)], diffuserTarget_[size_t(s)][size_t(c)]);
        updateLoopGains();

        // PRE glides per sample (50 ms): moving it bends the pitch a little, never clicks.
        const float predelayTarget = params_.predelayMs * 0.001f * sampleRate_;
        const float predelayGlide = smoothingCoef(0.05f, sampleRate_);
        const float inputGain = params_.freeze ? 0.0f : 1.0f;
        const float earlyGain = params_.earlyLevel * 0.5f;
        const float shimmer = params_.shimmer;
        const float width = params_.width;
        std::array<float, kDiffChannels> d{}, tmp{};
        std::array<float, kLines> y{};

        for (int n = 0; n < numSamples; ++n) {
            if ((controlCounter_++ % kControlInterval) == 0) {
                for (int i = 0; i < kLines; ++i) {
                    const size_t k = size_t(i);
                    const float target = modDepth_[k] * modScale_ * (1.0f + modulators_[k].advance(kControlInterval));
                    modStep_[k] = (target - mod_[k]) / float(kControlInterval);
                }
            }

            float l = inputHigh_[0].process(inputLow_[0].process(inputHighpass_[0].highpass(inL[n] * inputGain)));
            float r = inputHigh_[1].process(inputLow_[1].process(inputHighpass_[1].highpass(inR[n] * inputGain)));
            predelayNow_ += (predelayTarget - predelayNow_) * predelayGlide;
            if (predelayNow_ >= 2.0f || predelayTarget >= 2.0f) {
                const float pl = predelay_[0].read(std::max(predelayNow_, 2.0f)), pr = predelay_[1].read(std::max(predelayNow_, 2.0f));
                predelay_[0].write(l);
                predelay_[1].write(r);
                l = pl;
                r = pr;
            }

            // 8-channel diffuser.
            for (int c = 0; c < kDiffChannels; ++c) d[size_t(c)] = ((c & 1) ? r : l) * kUpmix[size_t(c)];
            for (int s = 0; s < kStages; ++s) {
                auto& stage = diffusers_[size_t(s)];
                for (int c = 0; c < kDiffChannels; ++c) {
                    const size_t k = size_t(c);
                    diffuserDelay_[size_t(s)][k] += diffuserStep_[size_t(s)][k];
                    const float at = diffuserDelay_[size_t(s)][k];
                    const float delayed = at == diffuserTarget_[size_t(s)][k] ? stage[k].readInt(int(at)) : stage[k].read(at);
                    stage[k].write(d[k]);
                    tmp[k] = delayed * diffuserFlip_[size_t(s)][k];
                }
                for (int c = 0; c < kDiffChannels; ++c) d[size_t(c)] = tmp[size_t(diffuserShuffle_[size_t(s)][size_t(c)])];
                hadamard8(d);
            }
            const float earlyL = (d[0] + d[2] + d[4] + d[6]) * earlyGain;
            const float earlyR = (d[1] + d[3] + d[5] + d[7]) * earlyGain;

            // Late field.
            float lateL = 0.0f, lateR = 0.0f, sum = 0.0f;
            for (int i = 0; i < kLines; ++i) {
                const size_t k = size_t(i);
                mod_[k] += modStep_[k];
                lengths_[k] += lengthStep_[k];
                allpassNow_[k] += allpassStep_[k];
                // The allpass interpolation is for small slow moves (the modulation); while SIZE
                // glides across many samples it would tick at each whole sample: cubic until settled.
                float v;
                if (lengths_[k] == targetLengths_[k]) {
                    v = lines_[k].readAllpass(lengths_[k] + kLineMod * mod_[k], interpolation_[k]);
                } else {
                    v = lines_[k].read(lengths_[k] + kLineMod * mod_[k]);
                    interpolation_[k] = v;
                }
                v = allpasses_[k].processAt(v, allpassNow_[k]);  // whole samples once settled
                const float low = lowSplit_[k].lowpass(v);
                const float rest = v - low;
                const float mid = highSplit_[k].lowpass(rest);
                v = gainLow_[k] * low + gainMid_[k] * mid + gainHigh_[k] * (rest - mid);
                y[k] = v;
                sum += v;
                lateL += v * kTapL[k];
                lateR += v * kTapR[k];
            }

            // Householder (y - (2/N) * sum(y)), then rotate by 5 lines (coprime with 16):
            // each line feeds the next one instead of itself, so the main loop runs through
            // all 16 lines before coming back. No single line rings as its own comb.
            const float h = sum * (2.0f / float(kLines));
            for (auto& v : y) v -= h;
            std::rotate(y.begin(), y.begin() + 5, y.end());

            if (shimmer > 0.0f) {
                // u = (1 x8, 0 x8)/sqrt(8) is unit-norm: crossfade that component towards its
                // re-pitched copy. Half of the loop's dimensions: DEPTH 1 is a strong cascade,
                // and since the shifter's gain is <= 1 the loop stays bounded.
                const float s = (y[0] + y[1] + y[2] + y[3] + y[4] + y[5] + y[6] + y[7]) * 0.35355339f;
                const float shifted = shimmerLp_.process(shimmerHp_.process(shifter_.process(s)));
                const float delta = shimmer * (shifted - s) * 0.35355339f;
                for (size_t k = 0; k < 8; ++k) y[k] += delta;
            }

            for (int i = 0; i < kLines; ++i) {
                const size_t k = size_t(i);
                lines_[k].write(y[k] + d[k & 7] * kInputSign[k] * 0.5f);
            }

            const float wl = earlyL + lateL * 0.3f, wr = earlyR + lateR * 0.3f;
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            outL[n] = mid + side;
            outR[n] = mid - side;
        }
        // The ramps end on their targets: snap away the rounding of the sums.
        const auto land = [](float& now, float target) { if (std::abs(target - now) < 2.0e-3f) now = target; };
        for (int i = 0; i < kLines; ++i) land(lengths_[size_t(i)], targetLengths_[size_t(i)]), land(allpassNow_[size_t(i)], allpassLengths_[size_t(i)]);
        for (int s = 0; s < kStages; ++s)
            for (int c = 0; c < kDiffChannels; ++c) land(diffuserDelay_[size_t(s)][size_t(c)], diffuserTarget_[size_t(s)][size_t(c)]);
    }

private:
    static constexpr int kDiffChannels = 8;
    static constexpr int kStages = 4;
    static constexpr int kControlInterval = 32;
    static constexpr float kMaxLineMs = 210.0f;
    static constexpr float kMaxAllpassMs = 20.0f;
    static constexpr float kMaxModMs = 1.0f;
    // The whole modulation now rides the line, at the depth the line had alone: the old
    // extra 2x in the allpass only reached part of the sound. More would smear SHIMMER,
    // which recirculates every swing (+12: octaves 51 dB above the gaps at 1x, 44 at 3x).
    static constexpr float kLineMod = 1.0f;
    static constexpr float kDriftCents = 6.0f;     // total tail drift at modDepth 1
    static constexpr float kMaxDriftRt60 = 60.0f;  // freeze / very long tails count as 60 s
    // Freeze uses a 20000 s RT60 rather than exactly 1 (-0.2 dB / minute): endless in practice,
    // and the loop can never creep up through interpolation gain.
    static constexpr float kFreezeRt60 = 20000.0f;
    static constexpr std::array<float, kStages> kStageMs{48.0f, 26.0f, 14.0f, 7.0f};
    static constexpr std::array<int, kLines> kOrder{0, 9, 3, 12, 6, 15, 1, 10, 4, 13, 7, 14, 2, 11, 5, 8};
    static constexpr std::array<float, kDiffChannels> kUpmix{1, 1, -1, 1, 1, -1, -1, -1};
    static constexpr std::array<float, kLines> kInputSign{1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, -1};
    // Rows 1 and 2 of a 16x16 Hadamard matrix: orthogonal, hence decorrelated L/R.
    static constexpr std::array<float, kLines> kTapL{1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1};
    static constexpr std::array<float, kLines> kTapR{1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1};

    static void hadamard8(std::array<float, kDiffChannels>& x) {
        for (int h = 1; h < kDiffChannels; h <<= 1)
            for (int i = 0; i < kDiffChannels; i += h * 2)
                for (int j = i; j < i + h; ++j) {
                    const float a = x[size_t(j)], b = x[size_t(j + h)];
                    x[size_t(j)] = a + b;
                    x[size_t(j + h)] = a - b;
                }
        for (auto& v : x) v *= 0.35355339f;  // 1/sqrt(8)
    }

    void updateLoopGains() {
        const float rt = params_.freeze ? kFreezeRt60 : params_.decaySeconds;
        for (int i = 0; i < kLines; ++i) {
            const size_t k = size_t(i);
            // The in-line allpass adds its length to the average loop time.
            const float loop = lengths_[k] + allpassLengths_[k];
            gainLow_[k] = rt60Gain(loop, rt * params_.bassMultiplier, sampleRate_);
            gainMid_[k] = rt60Gain(loop, rt, sampleRate_);
            gainHigh_[k] = rt60Gain(loop, rt * params_.hfRatio, sampleRate_);

            // Each pass through a modulated line detunes by a few hundredths of a cent; over
            // a long tail these add up like a random walk (~ sqrt(passes)). Scaling the
            // per-pass detune by sqrt(loop / RT60) keeps the *total* drift the same for a
            // small room and an endless cathedral: about kDriftCents * modDepth.
            const float tailSeconds = std::min(params_.freeze ? kMaxDriftRt60 : params_.decaySeconds, kMaxDriftRt60);
            const float centsPerPass = kDriftCents * params_.modDepth * std::sqrt(loop / sampleRate_ / std::max(tailSeconds, 0.1f));
            const float rate = std::max(params_.modRate * modRateJitter_[k], 0.01f);
            // Doppler of a delay swinging by ±A at `rate`: ≈ 1200/ln2 * 2π * rate * A / fs cents.
            modDepth_[k] = std::min(centsPerPass * 0.6931472f / 1200.0f * sampleRate_ / (kTwoPi * rate),
                                    kMaxModMs * 0.001f * sampleRate_);
        }
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    bool snapNextParams_ = false;

    std::array<std::array<DelayLine, kDiffChannels>, kStages> diffusers_;
    std::array<std::array<int, kDiffChannels>, kStages> diffuserShuffle_{};
    std::array<std::array<float, kDiffChannels>, kStages> diffuserDelay_{}, diffuserTarget_{}, diffuserStep_{};
    std::array<std::array<float, kDiffChannels>, kStages> diffuserSlot_{}, diffuserFlip_{};

    std::array<DelayLine, kLines> lines_;
    std::array<Allpass, kLines> allpasses_;
    std::array<float, kLines> lengths_{}, targetLengths_{}, lengthJitter_{}, allpassLengths_{}, allpassFraction_{};
    std::array<float, kLines> interpolation_{};  // allpass-interpolation state, per line
    std::array<float, kLines> allpassNow_{}, allpassStep_{}, lengthStep_{};
    float predelayNow_ = 0.0f;
    std::array<float, kLines> gainLow_{}, gainMid_{}, gainHigh_{};
    std::array<OnePole, kLines> lowSplit_, highSplit_;
    std::array<Lfo, kLines> modulators_;
    std::array<float, kLines> mod_{}, modStep_{}, modRateJitter_{}, modDepth_{};
    float modScale_ = 1.0f;
    uint32_t controlCounter_ = 0;

    DelayLine predelay_[2];
    OnePole inputHighpass_[2];
    SvfEq inputLow_[2], inputHigh_[2];
    SpectralPitchShifter shifter_;
    SvfEq shimmerHp_, shimmerLp_;
};

}  // namespace stasis
