#pragma once

#include "common.h"
#include "fft.h"

namespace stasis {

// STRETCH, real-time. The input is recorded continuously into a circular buffer and
// played back through PaulStretch frames (long FFT, random phases, sine-window
// overlap-add, see paul_stretch.h).
//
//  SMEAR    the read head follows the live input at 1/stretch speed. When it falls
//           too far behind, it jumps forward: PaulStretch frames are independent, so
//           a jump is inaudible.
//  CAPTURE  capture() freezes the last `captureSeconds`; that passage is then
//           stretched in an endless loop, whatever comes in afterwards.
//
// Latency: the output starts half a window late, on the wet path only.
// Note: one frame is computed per hop (every windowSize/2 samples); the plugin will
// spread that work over several blocks to avoid CPU peaks.
class CaptureStretch {
public:
    enum class Mode { Smear, Capture };

    struct Params {
        Mode mode = Mode::Smear;
        float stretch = 8.0f;          // 1 .. 1000
        float captureSeconds = 4.0f;   // length of the frozen loop in CAPTURE mode
    };

    void prepare(float sampleRate, int windowSize = 32768, float bufferSeconds = 30.0f) {
        sampleRate_ = sampleRate;
        size_ = windowSize;
        hop_ = windowSize / 2;
        fft_.prepare(windowSize);
        const size_t length = size_t(bufferSeconds * sampleRate);
        for (int c = 0; c < 2; ++c) {
            ring_[c].assign(length, 0.0f);
            captured_[c].assign(length, 0.0f);
            accum_[c].assign(size_t(size_), 0.0f);
            output_[c].assign(size_t(hop_), 0.0f);
            spec_[c].resize(size_t(size_));
        }
        analysis_.resize(size_t(size_));
        synthesis_.resize(size_t(size_));
        double sumSq = 0.0;
        for (int i = 0; i < size_; ++i) {
            const float x = float(i) / float(size_);
            analysis_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * x);
            synthesis_[size_t(i)] = std::sin(kPi * x);
            sumSq += double(analysis_[size_t(i)]) * analysis_[size_t(i)];
        }
        gain_ = float(1.0 / std::sqrt(sumSq / size_));
        written_ = 0;
        readPos_ = 0.0;
        capturedLength_ = 0;
        outRead_ = hop_;  // first frame is computed on the first sample
    }

    void setParams(const Params& p) { params_ = p; }

    // Freeze the last captureSeconds of input (CAPTURE mode reads from this copy).
    void capture() {
        const size_t ringLength = ring_[0].size();
        const size_t length = std::min({size_t(params_.captureSeconds * sampleRate_), ringLength, size_t(written_)});
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < length; ++i)
                captured_[c][i] = ring_[c][(size_t(written_) - length + i) % ringLength];
        capturedLength_ = length;
        capturePos_ = 0.0;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        const size_t ringLength = ring_[0].size();
        for (int n = 0; n < numSamples; ++n) {
            const size_t w = size_t(written_ % uint64_t(ringLength));
            ring_[0][w] = inL[n];
            ring_[1][w] = inR[n];
            ++written_;
            if (outRead_ == hop_) nextFrame();
            outL[n] = output_[0][size_t(outRead_)];
            outR[n] = output_[1][size_t(outRead_)];
            ++outRead_;
        }
    }

private:
    // Sample of the source the current mode reads from; zero outside its range.
    float source(int c, long long index) const {
        if (params_.mode == Mode::Capture) {
            if (capturedLength_ == 0) return 0.0f;
            const long long len = (long long)capturedLength_;
            return captured_[c][size_t(((index % len) + len) % len)];  // the loop wraps
        }
        const long long ringLength = (long long)ring_[0].size();
        if (index < 0 || index >= (long long)written_ || index < (long long)written_ - ringLength) return 0.0f;
        return ring_[c][size_t(index % ringLength)];
    }

    void nextFrame() {
        const double inHop = double(hop_) / std::max(1.0f, params_.stretch);
        long long centre;
        if (params_.mode == Mode::Capture) {
            centre = (long long)capturePos_;
            capturePos_ += inHop;
            if (capturedLength_ > 0 && capturePos_ >= double(capturedLength_)) capturePos_ -= double(capturedLength_);
        } else {
            // A frame needs half a window of audio after its centre.
            const double newest = double(written_) - size_ / 2;
            const double maxLag = double(ring_[0].size()) - size_;
            if (newest - readPos_ > maxLag) readPos_ = newest - size_;  // fell too far behind: jump
            readPos_ = std::min(readPos_ + inHop, std::max(newest, 0.0));
            centre = (long long)readPos_;
        }

        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < size_; ++i)
                spec_[c][size_t(i)] = source(c, centre - size_ / 2 + i) * analysis_[size_t(i)];
        fft_.forward(spec_[0]);
        fft_.forward(spec_[1]);
        spec_[0][0] = spec_[1][0] = 0.0f;
        spec_[0][size_t(size_ / 2)] = spec_[1][size_t(size_ / 2)] = 0.0f;
        for (int k = 1; k < size_ / 2; ++k) {
            const size_t ks = size_t(k);
            const float phase = kTwoPi * rng_.uniform();
            const float stereo = std::arg(spec_[1][ks]) - std::arg(spec_[0][ks]);
            spec_[0][ks] = std::polar(std::abs(spec_[0][ks]), phase);
            spec_[1][ks] = std::polar(std::abs(spec_[1][ks]), phase + stereo);
            spec_[0][size_t(size_ - k)] = std::conj(spec_[0][ks]);
            spec_[1][size_t(size_ - k)] = std::conj(spec_[1][ks]);
        }
        fft_.inverse(spec_[0]);
        fft_.inverse(spec_[1]);

        const float norm = gain_ / float(size_);
        for (int c = 0; c < 2; ++c) {
            auto& acc = accum_[c];
            for (int i = 0; i < size_; ++i) acc[size_t(i)] += spec_[c][size_t(i)].real() * synthesis_[size_t(i)] * norm;
            std::copy(acc.begin(), acc.begin() + hop_, output_[c].begin());
            std::move(acc.begin() + hop_, acc.end(), acc.begin());
            std::fill(acc.end() - hop_, acc.end(), 0.0f);
        }
        outRead_ = 0;
    }

    float sampleRate_ = 48000.0f;
    int size_ = 32768, hop_ = 16384, outRead_ = 0;
    Params params_;
    Fft fft_;
    Rng rng_{0xCA97};
    float gain_ = 1.0f;
    std::vector<float> analysis_, synthesis_;
    std::vector<float> ring_[2], captured_[2], accum_[2], output_[2];
    std::vector<Fft::Complex> spec_[2];
    uint64_t written_ = 0;
    double readPos_ = 0.0, capturePos_ = 0.0;
    size_t capturedLength_ = 0;
};

}  // namespace stasis
