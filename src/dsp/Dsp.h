// Small DSP building blocks shared by every module.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }
inline float clampf(float x, float lo, float hi) { return std::min(hi, std::max(lo, x)); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Rational tanh approximation; exact at +-3 where it saturates.
inline float fastTanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Smooth hard-ish clipper: x / (1 + |x|^k)^(1/k). Larger k is harder.
inline float softClipK(float x, float k) {
    return x / std::pow(1.0f + std::pow(std::fabs(x), k), 1.0f / k);
}

// Diode-pair style clipper with asymmetry (a > 0 lifts the positive knee).
inline float diodeClip(float x, float asym = 0.0f) {
    return x >= 0 ? fastTanh(x / (1.0f + asym)) * (1.0f + asym) : fastTanh(x);
}

// Per-sample exponential smoothing toward a target value.
struct Smoother {
    float current = 0, target = 0, coef = 0.999f;
    void prepare(double sr, float timeMs, float initial) {
        coef = float(std::exp(-1.0 / (0.001 * timeMs * sr)));
        current = target = initial;
    }
    void setTarget(float t) { target = t; }
    float next() { current = target + (current - target) * coef; return current; }
};

// Transposed direct form II biquad (RBJ cookbook designs), double state.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }
    inline float process(float xf) {
        double x = xf;
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return float(y);
    }
    void processBlock(float* x, int n) { for (int i = 0; i < n; ++i) x[i] = process(x[i]); }

    void setNormalized(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
    static double w0(double sr, double f) { return kTwoPi * std::clamp(f, 1.0, sr * 0.49) / sr; }

    void lowpass(double sr, double f, double q = 0.7071) {
        double w = w0(sr, f), c = std::cos(w), al = std::sin(w) / (2 * q);
        setNormalized((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass(double sr, double f, double q = 0.7071) {
        double w = w0(sr, f), c = std::cos(w), al = std::sin(w) / (2 * q);
        setNormalized((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void bandpass(double sr, double f, double q) {  // 0 dB peak gain
        double w = w0(sr, f), c = std::cos(w), al = std::sin(w) / (2 * q);
        setNormalized(al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
    void notch(double sr, double f, double q) {
        double w = w0(sr, f), c = std::cos(w), al = std::sin(w) / (2 * q);
        setNormalized(1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }
    void allpass(double sr, double f, double q) {
        double w = w0(sr, f), c = std::cos(w), al = std::sin(w) / (2 * q);
        setNormalized(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
    }
    void peak(double sr, double f, double q, double db) {
        double A = std::pow(10.0, db / 40.0), w = w0(sr, f), c = std::cos(w), al = std::sin(w) / (2 * q);
        setNormalized(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf(double sr, double f, double db, double slope = 1.0) {
        double A = std::pow(10.0, db / 40.0), w = w0(sr, f), c = std::cos(w), s = std::sin(w);
        double al = s / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
        setNormalized(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                      (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    }
    void highShelf(double sr, double f, double db, double slope = 1.0) {
        double A = std::pow(10.0, db / 40.0), w = w0(sr, f), c = std::cos(w), s = std::sin(w);
        double al = s / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
        setNormalized(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                      (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
    }
    void identity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    // Magnitude response at frequency f (for level calibration).
    double magnitude(double sr, double f) const {
        double w = kTwoPi * f / sr;
        double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
        double nr = b0 + b1 * cr + b2 * c2r, ni = b1 * ci + b2 * c2i;
        double dr = 1 + a1 * cr + a2 * c2r, di = a1 * ci + a2 * c2i;
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// One-pole low/high pass.
struct OnePole {
    float a = 0, z = 0;
    void setLowpass(double sr, double f) { a = float(std::exp(-kTwoPi * std::min(f, sr * 0.49) / sr)); }
    void reset() { z = 0; }
    inline float lp(float x) { z = x + (z - x) * a; return z; }
    inline float hp(float x) { return x - lp(x); }
};

// DC blocker (first-order high-pass around 5-20 Hz).
struct DcBlocker {
    float r = 0.999f, x1 = 0, y1 = 0;
    void prepare(double sr, double f = 10.0) { r = float(std::exp(-kTwoPi * f / sr)); x1 = y1 = 0; }
    void reset() { x1 = y1 = 0; }
    inline float process(float x) { float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

// Topology-preserving state-variable filter (Zavalishin), gives LP/BP/HP at once.
struct Svf {
    double g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    float lp = 0, bp = 0, hp = 0;
    void set(double sr, double f, double q) {
        g = std::tan(kPi * std::clamp(f, 5.0, sr * 0.45) / sr);
        k = 1.0 / std::max(q, 0.05);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0; }
    inline void process(float x) {
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        lp = float(v2);
        bp = float(v1);
        hp = float(x - k * v1 - v2);
    }
};

// Circular delay line with fractional (cubic Hermite) reads.
struct DelayLine {
    std::vector<float> buf;
    int mask = 0, w = 0;
    void prepare(int maxSamples) {
        int n = 1;
        while (n < maxSamples + 4) n <<= 1;
        buf.assign(size_t(n), 0.0f);
        mask = n - 1;
        w = 0;
    }
    void reset() { std::fill(buf.begin(), buf.end(), 0.0f); w = 0; }
    inline void push(float x) { buf[size_t(w)] = x; w = (w + 1) & mask; }
    // Read the sample written `d` samples ago (d >= 1 after push).
    inline float tap(int d) const { return buf[size_t((w - d) & mask)]; }
    // Cubic interpolation needs one newer neighbour, so the minimum delay is 2.
    inline float read(float d) const {
        d = std::clamp(d, 2.0f, float(mask - 3));
        int i = int(d);
        float f = d - float(i);
        float xm1 = tap(i - 1), x0 = tap(i), x1 = tap(i + 1), x2 = tap(i + 2);
        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

// Phase accumulator LFO producing several shapes in [-1, 1].
struct Lfo {
    double phase = 0, inc = 0;
    void setRate(double sr, double hz) { inc = hz / sr; }
    inline void advance() { phase += inc; if (phase >= 1.0) phase -= 1.0; }
    static float sine(double ph) { return float(std::sin(kTwoPi * ph)); }
    static float triangle(double ph) { double p = ph - std::floor(ph); return float(p < 0.5 ? 4 * p - 1 : 3 - 4 * p); }
    static float softSquare(double ph) { return fastTanh(4.0f * sine(ph)) / fastTanh(4.0f); }
    float sineAt(double offset = 0) const { return sine(phase + offset); }
    float triAt(double offset = 0) const { return triangle(phase + offset); }
};

// Peak envelope follower with separate attack/release.
struct EnvelopeFollower {
    float att = 0, rel = 0, env = 0;
    void prepare(double sr, float attackMs, float releaseMs) {
        att = float(std::exp(-1.0 / (0.001 * std::max(attackMs, 0.01f) * sr)));
        rel = float(std::exp(-1.0 / (0.001 * std::max(releaseMs, 0.01f) * sr)));
    }
    void reset() { env = 0; }
    inline float process(float x) {
        float a = std::fabs(x);
        env = a > env ? a + (env - a) * att : a + (env - a) * rel;
        return env;
    }
};

}  // namespace dsp
