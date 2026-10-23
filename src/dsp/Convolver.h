// Uniformly partitioned overlap-save FFT convolution for cabinet impulse responses.
// Latency is one partition (kBlock samples).
#pragma once
#include <algorithm>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "dsp/Dsp.h"

namespace dsp {

using cpx = std::complex<float>;

class Fft {
public:
    explicit Fft(int n = 128) { init(n); }
    void init(int n) {
        n_ = n;
        rev_.resize(size_t(n));
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[size_t(i)] = r;
        }
        tw_.resize(size_t(n / 2));
        for (int i = 0; i < n / 2; ++i)
            tw_[size_t(i)] = cpx(float(std::cos(-kTwoPi * i / n)), float(std::sin(-kTwoPi * i / n)));
    }
    int size() const { return n_; }
    // In-place transform; inverse is unscaled (caller divides by n).
    void transform(cpx* a, bool inverse) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[size_t(i)]) std::swap(a[i], a[rev_[size_t(i)]]);
        for (int len = 2; len <= n_; len <<= 1) {
            int step = n_ / len, half = len / 2;
            for (int i = 0; i < n_; i += len) {
                for (int j = 0; j < half; ++j) {
                    cpx w = tw_[size_t(j * step)];
                    if (inverse) w = std::conj(w);
                    cpx u = a[i + j], v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

private:
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<cpx> tw_;
};

// Frequency-domain partitions of an impulse response. Immutable once built,
// so it can be handed from the UI thread to the audio thread by pointer.
struct ImpulseResponse {
    static constexpr int kBlock = 64;
    static constexpr int kFft = 2 * kBlock;
    std::vector<std::vector<cpx>> parts;
    std::string name;

    // ir: mono samples at the engine sample rate. Normalized to unit energy.
    static std::unique_ptr<ImpulseResponse> build(const std::vector<float>& irIn, int maxSamples) {
        auto r = std::make_unique<ImpulseResponse>();
        std::vector<float> ir(irIn.begin(), irIn.begin() + std::min<ptrdiff_t>(ptrdiff_t(irIn.size()), maxSamples));
        if (ir.empty()) ir.push_back(1.0f);
        double energy = 0;
        for (float s : ir) energy += double(s) * s;
        float norm = energy > 1e-12 ? float(1.0 / std::sqrt(energy)) : 1.0f;
        // Short fade-out so truncation does not click.
        int fade = std::min<int>(int(ir.size()) / 8, 256);
        for (int i = 0; i < fade; ++i) ir[ir.size() - 1 - size_t(i)] *= float(i) / float(fade);
        Fft fft(kFft);
        int numParts = (int(ir.size()) + kBlock - 1) / kBlock;
        r->parts.resize(size_t(numParts));
        for (int p = 0; p < numParts; ++p) {
            auto& part = r->parts[size_t(p)];
            part.assign(kFft, cpx(0, 0));
            for (int i = 0; i < kBlock; ++i) {
                size_t idx = size_t(p * kBlock + i);
                if (idx < ir.size()) part[size_t(i)] = cpx(ir[idx] * norm, 0);
            }
            fft.transform(part.data(), false);
        }
        return r;
    }
};

class Convolver {
public:
    void prepare(int maxParts) {
        fft_.init(ImpulseResponse::kFft);
        fdl_.assign(size_t(maxParts), std::vector<cpx>(ImpulseResponse::kFft, cpx(0, 0)));
        acc_.assign(ImpulseResponse::kFft, cpx(0, 0));
        inBuf_.assign(ImpulseResponse::kFft, 0.0f);
        outBlock_.assign(ImpulseResponse::kBlock, 0.0f);
        reset();
    }
    void reset() {
        for (auto& v : fdl_) std::fill(v.begin(), v.end(), cpx(0, 0));
        std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
        std::fill(outBlock_.begin(), outBlock_.end(), 0.0f);
        pos_ = 0;
        fdlPos_ = 0;
    }

    void process(const ImpulseResponse& ir, float* x, int n) {
        constexpr int B = ImpulseResponse::kBlock;
        for (int i = 0; i < n; ++i) {
            inBuf_[size_t(B + pos_)] = x[i];
            x[i] = outBlock_[size_t(pos_)];
            if (++pos_ == B) {
                pos_ = 0;
                computeBlock(ir);
            }
        }
    }

private:
    void computeBlock(const ImpulseResponse& ir) {
        constexpr int B = ImpulseResponse::kBlock, N = ImpulseResponse::kFft;
        const int P = std::min<int>(int(ir.parts.size()), int(fdl_.size()));
        auto& slot = fdl_[size_t(fdlPos_)];
        for (int i = 0; i < N; ++i) slot[size_t(i)] = cpx(inBuf_[size_t(i)], 0);
        fft_.transform(slot.data(), false);
        std::fill(acc_.begin(), acc_.end(), cpx(0, 0));
        const int F = int(fdl_.size());
        for (int p = 0; p < P; ++p) {
            const auto& X = fdl_[size_t((fdlPos_ - p + F) % F)];
            const auto& H = ir.parts[size_t(p)];
            for (int k = 0; k < N; ++k) acc_[size_t(k)] += X[size_t(k)] * H[size_t(k)];
        }
        fft_.transform(acc_.data(), true);
        const float scale = 1.0f / float(N);
        for (int i = 0; i < B; ++i) outBlock_[size_t(i)] = acc_[size_t(B + i)].real() * scale;
        // Slide the input window: the newest block becomes the previous one.
        std::copy(inBuf_.begin() + B, inBuf_.end(), inBuf_.begin());
        fdlPos_ = (fdlPos_ + 1) % F;
    }

    Fft fft_;
    std::vector<std::vector<cpx>> fdl_;
    std::vector<cpx> acc_;
    std::vector<float> inBuf_, outBlock_;
    int pos_ = 0, fdlPos_ = 0;
};

}  // namespace dsp
