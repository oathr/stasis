#pragma once

#include <complex>
#include <vector>

#include "common.h"

namespace stasis {

// Iterative radix-2 complex FFT. Prototype grade: the plugin will switch to
// vDSP (macOS) / pffft (Windows) behind the same interface.
class Fft {
public:
    using Complex = std::complex<float>;

    void prepare(int size) {
        size_ = size;
        twiddles_.resize(size_t(size / 2));
        for (int k = 0; k < size / 2; ++k)
            twiddles_[size_t(k)] = std::polar(1.0f, -kTwoPi * float(k) / float(size));
        bitReverse_.resize(size_t(size));
        int bits = 0;
        while ((1 << bits) < size) ++bits;
        for (int i = 0; i < size; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitReverse_[size_t(i)] = r;
        }
    }
    int size() const { return size_; }

    void forward(std::vector<Complex>& data) const { transform(data, false); }
    // Unnormalised: divide by size() to round-trip.
    void inverse(std::vector<Complex>& data) const { transform(data, true); }

private:
    void transform(std::vector<Complex>& data, bool inverse) const {
        const int n = size_;
        for (int i = 0; i < n; ++i)
            if (i < bitReverse_[size_t(i)]) std::swap(data[size_t(i)], data[size_t(bitReverse_[size_t(i)])]);
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    Complex w = twiddles_[size_t(j * step)];
                    if (inverse) w = std::conj(w);
                    const Complex u = data[size_t(i + j)];
                    const Complex v = data[size_t(i + j + half)] * w;
                    data[size_t(i + j)] = u + v;
                    data[size_t(i + j + half)] = u - v;
                }
            }
        }
    }

    int size_ = 0;
    std::vector<Complex> twiddles_;
    std::vector<int> bitReverse_;
};

}  // namespace stasis
