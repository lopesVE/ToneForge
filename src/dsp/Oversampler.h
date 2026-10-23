// Polyphase FIR oversampler (Kaiser-windowed sinc), used around nonlinear stages
// so their harmonics do not fold back as aliasing.
#pragma once
#include <cmath>
#include <vector>

#include "dsp/Dsp.h"

namespace dsp {

class Oversampler {
public:
    // factor: 2 or 4. tapsPerPhase controls the anti-alias filter length.
    void prepare(int factor, int tapsPerPhase, int maxBlock) {
        factor_ = factor;
        taps_ = tapsPerPhase;
        len_ = factor * tapsPerPhase;
        design();
        upHist_.assign(size_t(2 * taps_), 0.0f);
        downHist_.assign(size_t(2 * len_), 0.0f);
        upPos_ = downPos_ = 0;
        work_.assign(size_t(maxBlock * factor), 0.0f);
    }
    void reset() {
        std::fill(upHist_.begin(), upHist_.end(), 0.0f);
        std::fill(downHist_.begin(), downHist_.end(), 0.0f);
    }
    int factor() const { return factor_; }
    float* buffer() { return work_.data(); }
    // Latency of an up + down pass, in base-rate samples.
    float latency() const { return float(len_ - 1) / float(factor_); }

    // Upsample n base-rate samples into buffer() (n * factor samples).
    void up(const float* in, int n) {
        float* out = work_.data();
        for (int i = 0; i < n; ++i) {
            upPos_ = (upPos_ == 0 ? taps_ : upPos_) - 1;
            upHist_[size_t(upPos_)] = upHist_[size_t(upPos_ + taps_)] = in[i];
            const float* h = &upHist_[size_t(upPos_)];  // h[0] newest
            for (int p = 0; p < factor_; ++p) {
                const float* c = &polyUp_[size_t(p * taps_)];
                float acc = 0;
                for (int k = 0; k < taps_; ++k) acc += c[k] * h[k];
                *out++ = acc;
            }
        }
    }
    // Downsample buffer() back into n base-rate samples.
    void down(float* out, int n) {
        const float* in = work_.data();
        for (int i = 0; i < n; ++i) {
            for (int p = 0; p < factor_; ++p) {
                downPos_ = (downPos_ == 0 ? len_ : downPos_) - 1;
                downHist_[size_t(downPos_)] = downHist_[size_t(downPos_ + len_)] = *in++;
            }
            const float* h = &downHist_[size_t(downPos_)];
            float acc = 0;
            for (int k = 0; k < len_; ++k) acc += coef_[size_t(k)] * h[k];
            out[i] = acc;
        }
    }

private:
    static double besselI0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 30; ++k) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
        }
        return sum;
    }
    void design() {
        coef_.assign(size_t(len_), 0.0f);
        const double beta = 8.0;
        const double fc = 0.5 / factor_ * 0.92;  // normalized to the high rate
        const double mid = (len_ - 1) / 2.0;
        double sum = 0;
        for (int i = 0; i < len_; ++i) {
            double t = i - mid;
            double sinc = t == 0 ? 2 * fc : std::sin(kTwoPi * fc * t) / (kPi * t);
            double r = (2.0 * i) / (len_ - 1) - 1.0;
            double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
            coef_[size_t(i)] = float(sinc * w);
            sum += sinc * w;
        }
        for (auto& c : coef_) c = float(c / sum);
        // Polyphase split for upsampling, gain = factor to keep unity level.
        polyUp_.assign(size_t(len_), 0.0f);
        for (int p = 0; p < factor_; ++p)
            for (int k = 0; k < taps_; ++k)
                polyUp_[size_t(p * taps_ + k)] = coef_[size_t(p + k * factor_)] * float(factor_);
    }

    int factor_ = 1, taps_ = 1, len_ = 1;
    std::vector<float> coef_, polyUp_, upHist_, downHist_, work_;
    int upPos_ = 0, downPos_ = 0;
};

}  // namespace dsp
