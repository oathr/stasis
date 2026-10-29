#pragma once

#include <array>

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
// Latency: about one window and a half (frames are computed one hop ahead), on the wet
// path only.
//
// Real-time cost: a frame (one 32768-point FFT each way, for both channels at once)
// is computed in stages, one stage per process() call, during the hop before it is
// needed. A capture is copied out of the ring buffer in chunks the same way. No call
// ever does a whole frame's work, so there are no CPU spikes.
class CaptureStretch {
public:
    enum class Mode { Smear, Capture };

    struct Params {
        Mode mode = Mode::Smear;
        float stretch = 8.0f;          // 1 .. 1000
        float captureSeconds = 4.0f;   // length of the frozen loop in CAPTURE mode
    };

    void prepare(float sampleRate, int windowSize = 32768, float bufferSeconds = 32.0f) {
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
            next_[c].assign(size_t(hop_), 0.0f);
        }
        spec_.resize(size_t(size_));
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
        // Random phases come from a table of unit vectors: no trigonometry per bin.
        for (size_t i = 0; i < kPhases; ++i) phases_[i] = std::polar(1.0f, kTwoPi * float(i) / float(kPhases));
        written_ = 0;
        readPos_ = 0.0;
        capturedLength_ = copied_ = 0;
        outRead_ = 0;  // the first hop is silent while the first frame is computed
        stage_ = 0;
    }

    void setParams(const Params& p) { params_ = p; }

    // Freeze the last captureSeconds of input (CAPTURE mode reads from this copy).
    void capture() {
        const size_t ringLength = ring_[0].size();
        const size_t maxLength = ringLength - size_t(size_);  // what the ring still holds while the copy runs
        captureFrom_ = written_;
        capturedLength_ = std::min({size_t(params_.captureSeconds * sampleRate_), maxLength, size_t(written_)});
        captureFrom_ -= capturedLength_;
        copied_ = 0;
        capturePos_ = 0.0;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        copyCapture();
        // Spread the frame's stages evenly over the hop, whatever the block size.
        const int perCall = std::max(1, (kStages * numSamples + hop_ - 1) / hop_);
        for (int i = 0; i < perCall && stage_ < kStages; ++i) runStage(stage_++);
        const size_t ringLength = ring_[0].size();
        for (int n = 0; n < numSamples; ++n) {
            const size_t w = size_t(written_ % uint64_t(ringLength));
            ring_[0][w] = inL[n];
            ring_[1][w] = inR[n];
            ++written_;
            if (outRead_ == hop_) {
                while (stage_ < kStages) runStage(stage_++);  // only if the blocks were huge
                std::swap(output_[0], next_[0]);
                std::swap(output_[1], next_[1]);
                outRead_ = 0;
                stage_ = 0;
            }
            outL[n] = output_[0][size_t(outRead_)];
            outR[n] = output_[1][size_t(outRead_)];
            ++outRead_;
        }
    }

private:
    // read, forward FFT (4 parts), phases (2), inverse FFT (4 parts), overlap-add
    static constexpr int kFftParts = 4;
    static constexpr int kStages = 1 + kFftParts + 2 + kFftParts + 1;
    static constexpr size_t kPhases = 4096;

    // Reads the window starting at `first` into spec_ as left + i·right, windowed.
    // Indices advance with a wrap counter: no division per sample.
    void readWindow(long long first) {
        const size_t ringLength = ring_[0].size();
        if (params_.mode == Mode::Capture) {
            if (capturedLength_ == 0) {
                std::fill(spec_.begin(), spec_.end(), Fft::Complex{});
                return;
            }
            const long long len = (long long)capturedLength_;
            size_t i = size_t(((first % len) + len) % len);  // the loop wraps
            size_t r = size_t((captureFrom_ + i) % ringLength);
            for (int n = 0; n < size_; ++n) {
                const bool copied = i < copied_;  // else still in the ring
                const float left = copied ? captured_[0][i] : ring_[0][r], right = copied ? captured_[1][i] : ring_[1][r];
                spec_[size_t(n)] = Fft::Complex(left, right) * analysis_[size_t(n)];
                if (++i == capturedLength_) {
                    i = 0;
                    r = size_t(captureFrom_ % ringLength);
                } else if (++r == ringLength) {
                    r = 0;
                }
            }
            return;
        }
        const long long oldest = std::max(0LL, (long long)written_ - (long long)ringLength);
        size_t r = size_t(((first % (long long)ringLength) + (long long)ringLength) % (long long)ringLength);
        for (int n = 0; n < size_; ++n) {
            const long long index = first + n;
            const bool valid = index >= oldest && index < (long long)written_;
            spec_[size_t(n)] = valid ? Fft::Complex(ring_[0][r], ring_[1][r]) * analysis_[size_t(n)] : Fft::Complex{};
            if (++r == ringLength) r = 0;
        }
    }

    void copyCapture() {
        if (copied_ >= capturedLength_) return;
        const size_t ringLength = ring_[0].size();
        const size_t end = std::min(capturedLength_, copied_ + 16384);
        for (int c = 0; c < 2; ++c)
            for (size_t i = copied_; i < end; ++i) captured_[c][i] = ring_[c][size_t((captureFrom_ + i) % ringLength)];
        copied_ = end;
    }

    // One frame, in stages. Both channels share one complex FFT: z = left + i·right.
    void runStage(int stage) {
        const int half = size_ / 2;
        const auto fftPart = [&](int part, bool inverse) {
            const int passes = fft_.passes();
            fft_.transform(spec_, inverse, passes * part / kFftParts, passes * (part + 1) / kFftParts);
        };
        // Stage → step: 0 read, 1 forward FFT, 2-3 phases, 4 inverse FFT, 5 overlap-add.
        if (stage >= 1 && stage <= kFftParts) return fftPart(stage - 1, false);
        if (stage >= kFftParts + 3 && stage <= 2 * kFftParts + 2) return fftPart(stage - kFftParts - 3, true);
        const int step = stage == 0 ? 0 : stage <= kFftParts + 2 ? stage - kFftParts + 1 : 5;
        switch (step) {
            case 0: {  // read the frame
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
                readWindow(centre - size_ / 2);
                break;
            }
            case 2:
            case 3: {  // random phases, half of the bins per stage
                const int from = stage == 2 ? 1 : half / 2, to = stage == 2 ? half / 2 : half;
                for (int k = from; k < to; ++k) {
                    // Unpack the two channels from z, keep their magnitudes and their phase
                    // difference (the stereo image), give the pair a new random phase.
                    const Fft::Complex a = spec_[size_t(k)], b = std::conj(spec_[size_t(size_ - k)]);
                    const Fft::Complex left = 0.5f * (a + b), right = Fft::Complex(0.0f, -0.5f) * (a - b);
                    const Fft::Complex u = phases_[rng_.next() & (kPhases - 1)];
                    const float magL = std::abs(left);
                    const Fft::Complex newL = magL * u;
                    const Fft::Complex newR = magL > 1.0e-9f ? u * right * std::conj(left) / magL : std::abs(right) * u;
                    // Pack again: Z[k] = L + iR, Z[N-k] = conj(L) + i·conj(R).
                    spec_[size_t(k)] = newL + Fft::Complex(0.0f, 1.0f) * newR;
                    spec_[size_t(size_ - k)] = std::conj(newL) + Fft::Complex(0.0f, 1.0f) * std::conj(newR);
                }
                if (stage == 3) spec_[0] = spec_[size_t(half)] = 0.0f;
                break;
            }
            case 5: {  // overlap-add; the first hop is the next output
                const float norm = gain_ / float(size_);
                for (int i = 0; i < size_; ++i) {
                    const float w = synthesis_[size_t(i)] * norm;
                    accum_[0][size_t(i)] += spec_[size_t(i)].real() * w;
                    accum_[1][size_t(i)] += spec_[size_t(i)].imag() * w;
                }
                for (int c = 0; c < 2; ++c) {
                    auto& acc = accum_[c];
                    std::copy(acc.begin(), acc.begin() + hop_, next_[c].begin());
                    std::move(acc.begin() + hop_, acc.end(), acc.begin());
                    std::fill(acc.end() - hop_, acc.end(), 0.0f);
                }
                break;
            }
            default: break;
        }
    }

    float sampleRate_ = 48000.0f;
    int size_ = 32768, hop_ = 16384, outRead_ = 0, stage_ = 0;
    Params params_;
    Fft fft_;
    Rng rng_{0xCA97};
    float gain_ = 1.0f;
    std::vector<float> analysis_, synthesis_;
    std::vector<float> ring_[2], captured_[2], accum_[2], output_[2], next_[2];
    std::vector<Fft::Complex> spec_;
    std::array<Fft::Complex, kPhases> phases_{};
    uint64_t written_ = 0, captureFrom_ = 0;
    double readPos_ = 0.0, capturePos_ = 0.0;
    size_t capturedLength_ = 0, copied_ = 0;
};

}  // namespace stasis
