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
        bits_ = bits;
        for (int i = 0; i < size; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitReverse_[size_t(i)] = r;
        }
    }
    int size() const { return size_; }

    void forward(std::vector<Complex>& data) const { transform(data, false, 0, passes()); }
    // Unnormalised: divide by size() to round-trip.
    void inverse(std::vector<Complex>& data) const { transform(data, true, 0, passes()); }

    // For spreading one transform over several audio blocks: pass 0 is the bit-reversal,
    // passes 1..log2(size) the butterfly stages. Run [first, last) in order, all of them.
    int passes() const { return bits_ + 1; }
    void transform(std::vector<Complex>& data, bool inverse, int firstPass, int lastPass) const {
        const int n = size_;
        if (firstPass == 0)
            for (int i = 0; i < n; ++i)
                if (i < bitReverse_[size_t(i)]) std::swap(data[size_t(i)], data[size_t(bitReverse_[size_t(i)])]);
        for (int len = 2 << std::max(0, firstPass - 1); len <= n && len < (2 << std::max(0, lastPass - 1)); len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    const Complex w = twiddles_[size_t(j * step)];
                    const float wr = w.real(), wi = inverse ? -w.imag() : w.imag();
                    Complex& a = data[size_t(i + j)];
                    Complex& b = data[size_t(i + j + half)];
                    // Plain multiply: std::complex's operator* checks for NaN/Inf (__mulsc3)
                    // and is several times slower without -ffast-math.
                    const float vr = b.real() * wr - b.imag() * wi, vi = b.real() * wi + b.imag() * wr;
                    const float ur = a.real(), ui = a.imag();
                    a = {ur + vr, ui + vi};
                    b = {ur - vr, ui - vi};
                }
            }
        }
    }

private:
    int size_ = 0, bits_ = 0;
    std::vector<Complex> twiddles_;
    std::vector<int> bitReverse_;
};

}  // namespace stasis
