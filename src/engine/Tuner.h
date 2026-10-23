// YIN pitch detector for the tuner (runs on the UI thread).
#pragma once
#include <cmath>
#include <string>
#include <vector>

struct PitchResult {
    float freq = 0;     // Hz, 0 if no stable pitch
    float clarity = 0;  // 0..1
};

inline PitchResult detectPitch(const float* x, int n, double sr) {
    // Work at half rate: guitar and bass fundamentals sit well below 6 kHz.
    std::vector<float> d2(size_t(n / 2));
    for (size_t i = 0; i < d2.size(); ++i) d2[i] = 0.5f * (x[2 * i] + x[2 * i + 1]);
    const double rate = sr / 2;
    const int W = int(d2.size()) / 2;
    const int minTau = int(rate / 1400.0), maxTau = std::min(W - 1, int(rate / 27.0));
    PitchResult r;
    if (maxTau <= minTau + 2) return r;

    double energy = 0;
    for (int i = 0; i < W; ++i) energy += double(d2[size_t(i)]) * d2[size_t(i)];
    if (energy / W < 1e-7) return r;  // too quiet (below about -70 dBFS)

    std::vector<float> cmnd(size_t(maxTau + 1), 1.0f);
    double running = 0;
    for (int tau = 1; tau <= maxTau; ++tau) {
        double s = 0;
        for (int i = 0; i < W; ++i) {
            double d = double(d2[size_t(i)]) - d2[size_t(i + tau)];
            s += d * d;
        }
        running += s;
        cmnd[size_t(tau)] = running > 0 ? float(s * tau / running) : 1.0f;
    }
    int best = -1;
    for (int tau = minTau; tau <= maxTau; ++tau) {
        if (cmnd[size_t(tau)] < 0.15f) {
            while (tau + 1 <= maxTau && cmnd[size_t(tau + 1)] < cmnd[size_t(tau)]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) return r;
    double t = best;
    if (best > 1 && best < maxTau) {
        double a = cmnd[size_t(best - 1)], b = cmnd[size_t(best)], c = cmnd[size_t(best + 1)];
        double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) t = best + 0.5 * (a - c) / den;
    }
    r.freq = float(rate / t);
    r.clarity = 1.0f - cmnd[size_t(best)];
    return r;
}

struct NoteInfo {
    std::string name;
    int octave = 0;
    float cents = 0;
};

inline NoteInfo noteFromFreq(float f, float a4 = 440.0f) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    NoteInfo ni;
    if (f <= 0) return ni;
    float midi = 69.0f + 12.0f * std::log2(f / a4);
    int nearest = int(std::lround(midi));
    ni.cents = (midi - float(nearest)) * 100.0f;
    ni.name = names[((nearest % 12) + 12) % 12];
    ni.octave = nearest / 12 - 1;
    return ni;
}
