// Passive bass/middle/treble tone stack, discretized with the bilinear transform.
// Transfer function after D. T. Yeh and J. O. Smith, "Discretization of the '59
// Fender Bassman Tone Stack", DAFx-06. Component values select the voicing.
#pragma once
#include <cmath>

#include "dsp/Dsp.h"

namespace dsp {

struct ToneStackComponents {
    double R1, R2, R3, R4;  // treble pot, bass pot, middle pot, slope resistor
    double C1, C2, C3;      // treble, bass, middle caps
};

class ToneStack {
public:
    void prepare(double sr) { sr_ = sr; reset(); }
    void reset() { z1_ = z2_ = z3_ = 0; }

    // t, m, l in [0, 1] (pot positions).
    void set(const ToneStackComponents& c, double t, double m, double l) {
        // Bass pot is audio taper.
        l = std::pow(std::clamp(l, 0.0, 1.0), 2.0) * 0.99 + 0.005;
        m = std::clamp(m, 0.0, 1.0) * 0.99 + 0.005;
        t = std::clamp(t, 0.0, 1.0) * 0.99 + 0.005;
        const double R1 = c.R1, R2 = c.R2, R3 = c.R3, R4 = c.R4, C1 = c.C1, C2 = c.C2, C3 = c.C3;

        double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
        double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                    m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                    l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
                    l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
        double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                    m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                    m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) + t * C1 * C2 * C3 * R1 * R3 * R4 -
                    t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4;
        double a0 = 1;
        double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
        double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                    l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                    l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
                    (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 +
                     C2 * C3 * R3 * R4);
        double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                    m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                    m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
                    l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;

        const double k = 2 * sr_, k2 = k * k, k3 = k2 * k;
        double B0 = -b1 * k - b2 * k2 - b3 * k3;
        double B1 = -b1 * k + b2 * k2 + 3 * b3 * k3;
        double B2 = b1 * k + b2 * k2 - 3 * b3 * k3;
        double B3 = b1 * k - b2 * k2 + b3 * k3;
        double A0 = -a0 - a1 * k - a2 * k2 - a3 * k3;
        double A1 = -3 * a0 - a1 * k + a2 * k2 + 3 * a3 * k3;
        double A2 = -3 * a0 + a1 * k + a2 * k2 - 3 * a3 * k3;
        double A3 = -a0 + a1 * k - a2 * k2 + a3 * k3;
        b0_ = B0 / A0; b1_ = B1 / A0; b2_ = B2 / A0; b3_ = B3 / A0;
        a1_ = A1 / A0; a2_ = A2 / A0; a3_ = A3 / A0;
    }

    inline float process(float xf) {
        double x = xf;
        double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y + z3_;
        z3_ = b3_ * x - a3_ * y;
        return float(y);
    }

    double magnitude(double f) const {
        double w = kTwoPi * f / sr_;
        auto ev = [&](double c0, double c1, double c2, double c3, double& re, double& im) {
            re = c0 + c1 * std::cos(w) + c2 * std::cos(2 * w) + c3 * std::cos(3 * w);
            im = -(c1 * std::sin(w) + c2 * std::sin(2 * w) + c3 * std::sin(3 * w));
        };
        double nr, ni, dr, di;
        ev(b0_, b1_, b2_, b3_, nr, ni);
        ev(1, a1_, a2_, a3_, dr, di);
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }

private:
    double sr_ = 48000;
    double b0_ = 1, b1_ = 0, b2_ = 0, b3_ = 0, a1_ = 0, a2_ = 0, a3_ = 0;
    double z1_ = 0, z2_ = 0, z3_ = 0;
};

}  // namespace dsp
