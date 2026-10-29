#pragma once

#include <array>

#include "color.h"
#include "common.h"

namespace stasis {

// 2× oversampler: zero-stuffing and decimation through the same 31-tap halfband
// (windowed sinc, Blackman). Lets the curves below fold harmonics above Nyquist far less.
class Oversampler2x {
public:
    Oversampler2x() {
        for (int i = 0; i < kTaps; ++i) {
            const double n = i - (kTaps - 1) / 2.0;
            const double sinc = n == 0.0 ? 0.5 : std::sin(0.5 * kPi * n) / (kPi * n);
            const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * i / (kTaps - 1)) + 0.08 * std::cos(4.0 * kPi * i / (kTaps - 1));
            h_[size_t(i)] = float(sinc * w);
        }
    }
    void up(float x, float& a, float& b) {
        push(upHistory_, upPos_, 2.0f * x);
        a = convolve(upHistory_, upPos_);
        push(upHistory_, upPos_, 0.0f);
        b = convolve(upHistory_, upPos_);
    }
    float down(float a, float b) {
        push(downHistory_, downPos_, a);
        push(downHistory_, downPos_, b);
        return convolve(downHistory_, downPos_);
    }

private:
    static constexpr int kTaps = 31;
    using History = std::array<float, kTaps>;
    static void push(History& h, int& pos, float x) {
        pos = (pos + 1) % kTaps;
        h[size_t(pos)] = x;
    }
    float convolve(const History& h, int pos) const {
        float sum = 0.0f;
        for (int i = 0; i < kTaps; ++i) sum += h_[size_t(i)] * h[size_t((pos - i + kTaps) % kTaps)];
        return sum;
    }
    std::array<float, kTaps> h_{};
    History upHistory_{}, downHistory_{};
    int upPos_ = 0, downPos_ = 0;
};

// SHAPE: the first touch on the sound, before the chain.
//  TILT   -1..1  ±6 dB tilt around 800 Hz (dark ◂▸ bright), two shelves
//  TYPE          how DRIVE bends the wave:
//    TAPE     tanh, antialiased (ADAA): round, warm, the classic
//    TUBE     asymmetric exponential: the positive side softens early, the negative
//             side later; even harmonics even without HARM
//    FOLD     sine wavefolder: past full scale the wave folds back on itself; bells,
//             glassy overtones, "west coast"
//    RECTIFY  blends in the rectified wave: an octave up appears (fuzz octaver)
//    SLEW     limits how fast the wave may move: sharp edges turn into ramps; a soft,
//             ghostly, slightly detuned grit that follows the pitch
//    BLOOM    drive follows the level inversely: loud notes stay clean, their decay and
//             the reverb tails get gritty as they fade
//  DRIVE   0..1  amount. Level is matched at -12 dBFS
//  HARM    0..1  asymmetry: adds even harmonics; the DC it creates is removed
// All types but TAPE run 2× oversampled.
class Shape {
public:
    enum class Type { Tape, Tube, Fold, Rectify, Slew, Bloom };

    struct Params {
        float tilt = 0.0f;
        float drive = 0.0f;
        float harm = 0.0f;
        Type type = Type::Tape;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& d : dcBlock_) d.setCutoff(8.0f, sampleRate);
        envCoef_ = smoothingCoef(0.05f, sampleRate);
        for (auto& c : channels_) c = {};
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        const float tiltDb = 6.0f * std::clamp(p.tilt, -1.0f, 1.0f);
        for (int c = 0; c < 2; ++c) {
            low_[c].set(SvfEq::Type::LowShelf, 800.0f, 0.5f, -tiltDb, sampleRate_);
            high_[c].set(SvfEq::Type::HighShelf, 800.0f, 0.5f, tiltDb, sampleRate_);
        }
        const float d = std::clamp(p.drive, 0.0f, 1.0f);
        gain_ = 1.0f + 7.0f * d * d;
        if (p.type == Type::Fold) gain_ = 1.0f + 11.0f * d * d;  // folding starts past full scale
        if (p.type == Type::Slew) gain_ = 1.0f + 2.0f * d;
        bias_ = 0.4f * std::clamp(p.harm, 0.0f, 1.0f);
        // SLEW: largest step per (oversampled) sample, from free to a crawl.
        slewStep_ = 1.0f / (1.0f + 40.0f * d * d);
        saturate_ = d > 1.0e-3f || p.harm > 1.0e-3f;
        // Level matched at -12 dBFS: the RMS of a sine at that level, through the curve
        // (DC removed, as the DC blocker will), equals the input's. BLOOM matches per
        // sample, its drive moves.
        constexpr float kReference = 0.25f;
        double sum = 0.0, sumSq = 0.0;
        constexpr int kPoints = 64;
        for (int i = 0; i < kPoints; ++i) {
            const float y = curve(gain_ * kReference * std::sin(kTwoPi * float(i) / kPoints) + bias_) - curve(bias_);
            sum += y;
            sumSq += double(y) * y;
        }
        const double variance = sumSq / kPoints - (sum / kPoints) * (sum / kPoints);
        norm_ = variance > 1.0e-12 ? float(kReference / std::sqrt(2.0 * variance)) : 1.0f;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float* in[2] = {inL, inR};
        float* out[2] = {outL, outR};
        const float biasOut = curve(bias_);
        for (int c = 0; c < 2; ++c) {
            auto& ch = channels_[size_t(c)];
            for (int n = 0; n < numSamples; ++n) {
                float v = high_[c].process(low_[c].process(in[c][n]));
                if (saturate_) {
                    if (params_.type == Type::Tape) {
                        v = (ch.adaa.process(gain_ * v + bias_) - biasOut) * norm_;
                    } else {
                        float norm = norm_, gain = gain_;
                        if (params_.type == Type::Bloom) {
                            ch.env += envCoef_ * (std::abs(v) - ch.env);
                            const float quiet = 1.0f - std::min(1.0f, ch.env / 0.3f);
                            gain = 1.0f + 20.0f * params_.drive * quiet;
                            norm = 0.25f / (std::tanh(gain * 0.25f + bias_) - biasOut + 1.0e-6f);
                        }
                        float a, b;
                        ch.oversampler.up(v, a, b);
                        a = (shapeSample(ch, gain * a + bias_) - biasOut) * norm;
                        b = (shapeSample(ch, gain * b + bias_) - biasOut) * norm;
                        v = ch.oversampler.down(a, b);
                    }
                    v = dcBlock_[c].highpass(v);
                }
                out[c][n] = v;
            }
        }
    }

private:
    struct Channel {
        TanhAdaa adaa;
        Oversampler2x oversampler;
        float slewed = 0.0f, env = 0.0f;
    };

    // The static curve of the current type (what the level matching measures).
    float curve(float x) const {
        switch (params_.type) {
            case Type::Tape:
            case Type::Slew:
            case Type::Bloom: return std::tanh(x);
            case Type::Tube: return x >= 0.0f ? 1.0f - std::exp(-x) : (std::exp(1.4f * x) - 1.0f) / 1.4f;
            case Type::Fold: return std::sin(0.5f * kPi * x);
            case Type::Rectify: {
                const float m = std::clamp(params_.drive, 0.0f, 1.0f);
                return std::tanh((1.0f - m) * x + m * std::abs(x));
            }
        }
        return x;
    }

    float shapeSample(Channel& ch, float x) {
        if (params_.type == Type::Slew) {
            // A smooth limit: small moves pass, large ones tend to slewStep_ per sample.
            // (A hard clamp beats against the sample clock and leaves sub-tones.)
            const float diff = x - ch.slewed;
            ch.slewed += diff / (1.0f + std::abs(diff) / slewStep_);
            return std::tanh(ch.slewed);
        }
        return curve(x);
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    SvfEq low_[2], high_[2];
    OnePole dcBlock_[2];
    std::array<Channel, 2> channels_{};
    float gain_ = 1.0f, bias_ = 0.0f, norm_ = 1.0f, slewStep_ = 1.0f, envCoef_ = 0.01f;
    bool saturate_ = false;
};

}  // namespace stasis
