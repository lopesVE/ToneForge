// Analog-style octaver and delay-line pitch shifter (also usable as a whammy).
#include <cmath>

#include "dsp/Dsp.h"
#include "engine/Module.h"

using namespace dsp;

class Octaver : public Module {
public:
    enum { kDirect, kSub1, kSub2, kUp };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"octaver", "Octaver", "Oitava abaixo (1 e 2) por divisor de frequência e oitava acima.",
                            Category::Pitch,
                            {P("direct", "Direct", 0, 100, 70, "%", 0), P("sub1", "-1 Oct", 0, 100, 60, "%", 0),
                             P("sub2", "-2 Oct", 0, 100, 0, "%", 0), P("up", "+1 Oct", 0, 100, 0, "%", 0)}};
        return i;
    }
    Octaver() : Module(Info()) {}

protected:
    void onPrepare(double sr, int) override {
        for (auto& c : ch_) {
            c.track1.lowpass(sr, 320, 0.7);
            c.track2.lowpass(sr, 320, 0.7);
            c.env.prepare(sr, 3, 60);
            c.subLp.lowpass(sr, 900, 0.7);
            c.upHp.highpass(sr, 120, 0.7);
            c.upLp.lowpass(sr, 3500, 0.7);
        }
        onReset();
    }
    void onReset() override {
        for (auto& c : ch_) {
            c.track1.reset(); c.track2.reset(); c.subLp.reset(); c.upHp.reset(); c.upLp.reset(); c.env.reset();
            c.last = 0; c.ff1 = c.ff2 = 1;
        }
    }
    void process(float* L, float* R, int n, bool stereo) override {
        const float direct = get(kDirect) * 0.01f, s1 = get(kSub1) * 0.01f, s2 = get(kSub2) * 0.01f, up = get(kUp) * 0.01f;
        for (int c = 0; c < (stereo ? 2 : 1); ++c) {
            float* x = c ? R : L;
            Ch& s = ch_[c];
            for (int i = 0; i < n; ++i) {
                float in = x[i];
                float t = s.track2.process(s.track1.process(in));
                // Flip-flops toggle on each rising zero crossing of the tracked fundamental.
                if (s.last <= 0 && t > 0) {
                    s.ff1 = -s.ff1;
                    if (s.ff1 > 0) s.ff2 = -s.ff2;
                }
                s.last = t;
                float e = s.env.process(in);
                float sub = s.subLp.process(s1 * s.ff1 * e + s2 * s.ff2 * e);
                float oct = s.upLp.process(s.upHp.process(std::fabs(in) * 2.0f)) * up;
                x[i] = in * direct + sub * 1.2f + oct;
            }
        }
    }

private:
    struct Ch {
        Biquad track1, track2, subLp, upHp, upLp;
        EnvelopeFollower env;
        float last = 0, ff1 = 1, ff2 = 1;
    } ch_[2];
};
TF_REGISTER(Octaver);

// ---------------------------------------------------------------------------
class PitchShifter : public Module {
public:
    enum { kShift, kFine, kMix, kVoice };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"pitch", "Pitch Shifter", "Transpõe ±24 semitons. Mapeie 'Shift' num pedal MIDI para efeito whammy.",
                            Category::Pitch,
                            {P("shift", "Shift", -24, 24, 12, "st", 1), P("fine", "Fine", -50, 50, 0, "ct", 0),
                             P("mix", "Mix", 0, 100, 50, "%", 0), Choice("voice", "Janela", {"Curta", "Média", "Longa"}, 1)}};
        return i;
    }
    PitchShifter() : Module(Info()) {}

protected:
    void onPrepare(double sr, int) override {
        for (auto& d : dl_) d.prepare(int(0.2 * sr));
        shift_.prepare(sr, 30, get(kShift));
        onReset();
    }
    void onReset() override {
        for (auto& d : dl_) d.reset();
        phase_ = 0;
    }
    void process(float* L, float* R, int n, bool stereo) override {
        static const float windows[] = {0.025f, 0.05f, 0.09f};
        const float win = windows[getInt(kVoice)] * float(sr_);
        const float mix = get(kMix) * 0.01f;
        shift_.setTarget(get(kShift) + get(kFine) * 0.01f);
        for (int i = 0; i < n; ++i) {
            float ratio = std::pow(2.0f, shift_.next() / 12.0f);
            // Two read heads half a window apart sweep the delay; a sine
            // crossfade hides the jump when each head wraps around.
            phase_ += (1.0f - ratio) / win;
            phase_ -= std::floor(phase_);
            float p2 = phase_ + 0.5f;
            p2 -= std::floor(p2);
            float g1 = std::sin(float(kPi) * phase_), g2 = std::sin(float(kPi) * p2);
            for (int c = 0; c < (stereo ? 2 : 1); ++c) {
                float* x = c ? R : L;
                dl_[c].push(x[i]);
                float y = dl_[c].read(2.0f + phase_ * win) * g1 + dl_[c].read(2.0f + p2 * win) * g2;
                x[i] = x[i] * (1 - mix) + y * mix;
            }
        }
    }

private:
    DelayLine dl_[2];
    Smoother shift_;
    float phase_ = 0;
};
TF_REGISTER(PitchShifter);
