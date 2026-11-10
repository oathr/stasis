#pragma once

#include <array>
#include <vector>

#include "common.h"
#include "fft.h"

namespace stasis {

// SPECTRAL: a reverb built in the frequency domain. Every bin of a 4096-point STFT keeps
// its energy and lets it go at its own RT60. Each bin also keeps the frequency it last
// heard and a continuous phase that turns at that frequency, so a note's tail stays the
// note: a sustained, singing decay of the sound's own spectrum rather than a noise wash.
// (A first version drew the phases at random on every frame: every tail turned into
// breath noise.) SIZE and MOD bring the diffusion back, as much as asked.
//  DECAY  RT60 of the energy (1 kHz), INFINITY = endless; FREEZE stops the input too
//  TONE   -1..1: which end lasts: dark (lows ring on, highs fade fast) .. bright
//  SIZE   0..1: DISPERSION, what no room does: each frequency reaches the pool late in
//         proportion to its height (up to 0.85 s at the top), so every sound blooms
//         upwards, its lows first and its highs rising after them
//  MOD    0..1: each bin detunes slowly (up to ±1 %, a chorus), its phase wanders and its
//         level flickers: from a still, glassy ring to a moving shimmer
// Energy in = energy out: a steady sound comes out at its own level, whatever DECAY.
// Left and right start a quarter turn apart in a random pattern: wide, yet each in tune.
// Latency 4096 samples: it reads as pre-delay (the plugin does not report it).
class SpectralReverb {
public:
    struct Params {
        float decaySeconds = 6.0f;
        float tone = 0.0f;
        float size = 0.5f;
        float mod = 0.3f;
        float predelayMs = 0.0f;
        bool freeze = false;
    };

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        fft_.prepare(kSize);
        window_.resize(kSize);
        for (int i = 0; i < kSize; ++i) window_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(kSize));
        Rng spread(0x5EC7);
        for (size_t ch = 0; ch < channels_.size(); ++ch) {
            auto& c = channels_[ch];
            c.input.assign(kSize, 0.0f);
            c.accum.assign(kSize, 0.0f);
            c.output.assign(kHop, 0.0f);
            c.power.assign(kBins, 0.0f);
            c.flicker.assign(kBins, 1.0f);
            c.lastPhase.assign(kBins, 0.0f);
            c.freq.assign(kBins, 0.0f);
            c.phase.assign(kBins, 0.0f);
            c.vibrato.assign(kBins, 0.0f);
            for (auto& frame : c.late) frame.assign(size_t(kBins) * 2, 0.0f);
            c.latePos = 0;
            c.spectrum.resize(kSize);
            c.rng = Rng(ch == 0 ? 0x5EC7u : 0xA11CEu);
            for (int k = 0; k < kBins; ++k) {
                c.freq[size_t(k)] = float(k);
                c.phase[size_t(k)] = ch == 0 ? 0.0f : 0.5f * kPi * spread.bipolar();
            }
        }
        fill_ = kSize - kHop;
        read_ = 0;
        smeared_.assign(kBins, 0.0f);
        lag_.assign(kBins, 0);
        decay_.assign(kBins, 0.0f);
        keep_.assign(kBins, 0.0f);
        predelay_[0].prepare(int(0.5f * sampleRate) + 8);
        predelay_[1].prepare(int(0.5f * sampleRate) + 8);
        setParams(params_);
    }

    void setParams(const Params& p) {
        params_ = p;
        // Per bin: the RT60 tilted by TONE around 1 kHz (x/÷ 3 per decade at the ends).
        const float tilt = std::clamp(p.tone, -1.0f, 1.0f);  // x/÷10 per decade at the ends
        for (int k = 0; k < kBins; ++k) {
            const float hz = std::max(20.0f, float(k) * sampleRate_ / float(kSize));
            const float rt = p.freeze || !std::isfinite(p.decaySeconds) ? INFINITY
                                                                           : p.decaySeconds * std::pow(hz / 1000.0f, tilt);
            // Energy per hop, and the share of a new frame that joins the pool so that a
            // steady input settles at its own level (1 - g for a power sum).
            const float g = std::isfinite(rt) ? std::pow(10.0f, -6.0f * float(kHop) / (std::max(rt, 0.05f) * sampleRate_)) : 1.0f;
            decay_[size_t(k)] = g;
            keep_[size_t(k)] = 1.0f - g;
            // DISPERSION: frames of delay for this bin, growing with its height.
            lag_[size_t(k)] = int(std::round(std::clamp(p.size, 0.0f, 1.0f) * float(kLate - 1) * std::pow(float(k) / float(kBins - 1), 0.6f)));
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const float predelay = params_.predelayMs * 0.001f * sampleRate_;
        const float inputGain = params_.freeze ? 0.0f : 1.0f;
        for (int n = 0; n < numSamples; ++n) {
            float x[2] = {inL[n] * inputGain, inR[n] * inputGain};
            if (predelay >= 2.0f)
                for (int c = 0; c < 2; ++c) {
                    const float d = predelay_[c].read(predelay);
                    predelay_[c].write(x[c]);
                    x[c] = d;
                }
            for (int c = 0; c < 2; ++c) {
                channels_[size_t(c)].input[size_t(fill_)] = x[c];
                (c == 0 ? outL : outR)[n] = channels_[size_t(c)].output[size_t(read_)];
            }
            ++read_;
            if (++fill_ == kSize) frame();
        }
    }

private:
    static constexpr int kSize = 4096, kHop = kSize / 4, kBins = kSize / 2 + 1, kLate = 40;

    struct Channel {
        std::vector<float> input, accum, output, power, flicker, lastPhase, freq, phase, vibrato;
        std::array<std::vector<float>, 40> late;  // the last frames (energy, frequency per bin), for DISPERSION
        int latePos = 0;
        std::vector<Fft::Complex> spectrum;
        Rng rng{1};
    };

    void frame() {
        // Hann analysis and synthesis, 4x overlap: the windows sum to 1.5.
        const float norm = 1.0f / (1.5f * float(kSize));
        const float expected = kTwoPi * float(kHop) / float(kSize);  // phase per hop, per bin
        const float smear = std::clamp(params_.size, 0.0f, 1.0f);
        const float mod = std::clamp(params_.mod, 0.0f, 1.0f);
        for (auto& c : channels_) {
            for (int i = 0; i < kSize; ++i) c.spectrum[size_t(i)] = c.input[size_t(i)] * window_[size_t(i)];
            fft_.forward(c.spectrum);
            auto& now = c.late[size_t(c.latePos)];
            for (int k = 0; k < kBins; ++k) {
                const auto z = c.spectrum[size_t(k)];
                // Where this bin's energy really sits (phase vocoder): stored with it.
                const float phase = std::arg(z);
                float delta = phase - c.lastPhase[size_t(k)] - float(k) * expected;
                c.lastPhase[size_t(k)] = phase;
                delta -= kTwoPi * std::round(delta / kTwoPi);
                now[size_t(2 * k)] = std::norm(z);
                now[size_t(2 * k + 1)] = float(k) + delta / expected;
            }
            for (int k = 0; k < kBins; ++k) {
                // DISPERSION: this bin takes in the frame from lag_ frames ago.
                const auto& then = c.late[size_t((c.latePos - lag_[size_t(k)] + kLate) % kLate)];
                const float m = then[size_t(2 * k)];
                // Its frequency is kept while the frame brings a good share of the bin's
                // energy: the tail keeps the note.
                if (m > 0.25f * c.power[size_t(k)] && m > 1.0e-12f) c.freq[size_t(k)] = then[size_t(2 * k + 1)];
                // The pool: each bin's energy decays and takes in the new frame's.
                c.power[size_t(k)] = c.power[size_t(k)] * decay_[size_t(k)] + m * keep_[size_t(k)];
            }
            c.latePos = (c.latePos + 1) % kLate;
            // A light blur across neighbours with SIZE (none at 0: the note stays a note).
            const int passes = int(std::round(smear * 2.0f));
            std::copy(c.power.begin(), c.power.end(), smeared_.begin());
            for (int p = 0; p < passes; ++p) {
                float prev = smeared_[0];
                for (int k = 1; k + 1 < kBins; ++k) {
                    const float here = smeared_[size_t(k)];
                    smeared_[size_t(k)] = 0.25f * prev + 0.5f * here + 0.25f * smeared_[size_t(k + 1)];
                    prev = here;
                }
            }
            // Each bin turns at its frequency; MOD lets the phase wander and the level flicker.
            for (int k = 0; k < kBins; ++k) {
                float& ph = c.phase[size_t(k)];
                float& vib = c.vibrato[size_t(k)];
                vib += 0.05f * (c.rng.bipolar() - vib);  // a slow random per bin
                ph += c.freq[size_t(k)] * (1.0f + 0.01f * mod * vib) * expected + 0.25f * kPi * mod * c.rng.bipolar();
                ph -= kTwoPi * std::floor(ph / kTwoPi);
                float& f = c.flicker[size_t(k)];
                f += 0.15f * ((1.0f + 0.5f * mod * c.rng.bipolar()) - f);
                const float mag = std::sqrt(std::max(smeared_[size_t(k)], 0.0f)) * f;
                c.spectrum[size_t(k)] = std::polar(mag, ph);
                if (k > 0 && k < kBins - 1) c.spectrum[size_t(kSize - k)] = std::conj(c.spectrum[size_t(k)]);
            }
            c.spectrum[0] = 0.0f;
            fft_.inverse(c.spectrum);
            for (int i = 0; i < kSize; ++i) c.accum[size_t(i)] += c.spectrum[size_t(i)].real() * window_[size_t(i)] * norm;
            std::copy(c.accum.begin(), c.accum.begin() + kHop, c.output.begin());
            std::move(c.accum.begin() + kHop, c.accum.end(), c.accum.begin());
            std::fill(c.accum.end() - kHop, c.accum.end(), 0.0f);
            std::move(c.input.begin() + kHop, c.input.end(), c.input.begin());
        }
        fill_ = kSize - kHop;
        read_ = 0;
    }

    float sampleRate_ = 48000.0f;
    Params params_;
    Fft fft_;
    std::vector<float> window_, smeared_, decay_, keep_;
    std::vector<int> lag_;
    std::array<Channel, 2> channels_;
    DelayLine predelay_[2];
    int fill_ = 0, read_ = 0;
};

}  // namespace stasis
