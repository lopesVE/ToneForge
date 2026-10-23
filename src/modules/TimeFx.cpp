// Delay (digital / analog / tape, ping-pong, tempo sync) and reverb.
#include <cmath>

#include "dsp/Dsp.h"
#include "engine/Module.h"
#include "engine/Tempo.h"

using namespace dsp;

class Delay : public Module {
public:
    enum { kTime, kSync, kDivision, kFeedback, kMix, kMode, kPingPong, kTone, kMod };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"delay", "Delay", "Delay digital, analógico ou de fita, com ping-pong e sincronia de tempo.",
                            Category::Delay,
                            {P("time", "Time", 20, 2000, 380, "ms", 0, 2.0f), Toggle("sync", "Sync"),
                             Choice("division", "Divisão",
                                    std::vector<std::string>(std::begin(kDivisionNames), std::end(kDivisionNames)), 4),
                             P("feedback", "Feedback", 0, 100, 35, "%", 0), P("mix", "Mix", 0, 100, 30, "%", 0),
                             Choice("mode", "Modo", {"Digital", "Analógico", "Fita"}, 0), Toggle("pingpong", "Ping-Pong"),
                             P("tone", "Tone", 0, 10, 6), P("mod", "Mod", 0, 100, 10, "%", 0)}};
        return i;
    }
    Delay() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double sr, int) override {
        for (auto& d : dl_) d.prepare(int(2.2 * sr));
        time_.prepare(sr, 180, targetSamples());
        onReset();
    }
    void onReset() override {
        for (auto& d : dl_) d.reset();
        for (auto& f : tone_) f.reset();
    }
    float targetSamples() const {
        float ms = get(kTime);
        if (getBool(kSync)) {
            float bpm = std::clamp(g_tempoBpm.load(std::memory_order_relaxed), 30.0f, 300.0f);
            ms = std::min(2000.0f, 60000.0f / bpm * kDivisionBeats[getInt(kDivision)]);
        }
        return ms * 0.001f * float(sr_);
    }
    void process(float* L, float* R, int n, bool) override {
        time_.setTarget(targetSamples());
        const int mode = getInt(kMode);
        const bool ping = getBool(kPingPong);
        const float fb = get(kFeedback) * 0.01f * (mode == 0 ? 1.0f : 1.05f);
        const float mix = get(kMix) * 0.01f, mod = get(kMod) * 0.01f;
        float toneHz = 1200.0f * std::pow(14.0f, get(kTone) / 10);
        if (mode == 1) toneHz = std::min(toneHz, 3500.0f);
        if (mode == 2) toneHz = std::min(toneHz, 5000.0f);
        for (auto& f : tone_) f.setLowpass(sr_, toneHz);
        wow_.setRate(sr_, 0.7);
        flutter_.setRate(sr_, 6.3);
        const float modDepth = mod * (mode == 2 ? 0.004f : 0.0012f) * float(sr_);

        for (int i = 0; i < n; ++i) {
            wow_.advance();
            flutter_.advance();
            float d = time_.next() + modDepth * (wow_.sineAt() + 0.3f * flutter_.sineAt());
            float wl = dl_[0].read(d), wr = dl_[1].read(d);
            float fl = tone_[0].lp(wl) * fb, fr = tone_[1].lp(wr) * fb;
            if (mode != 0) { fl = fastTanh(fl * 1.2f) / 1.2f; fr = fastTanh(fr * 1.2f) / 1.2f; }
            float in = 0.5f * (L[i] + R[i]);
            if (ping) {
                dl_[0].push(in + fr);
                dl_[1].push(fl);
            } else {
                dl_[0].push(L[i] + fl);
                dl_[1].push(R[i] + fr);
            }
            L[i] = L[i] + wl * mix;
            R[i] = R[i] + wr * mix;
        }
    }

private:
    DelayLine dl_[2];
    OnePole tone_[2];
    Smoother time_;
    Lfo wow_, flutter_;
};
TF_REGISTER(Delay);

// ---------------------------------------------------------------------------
namespace {
struct Comb {
    std::vector<float> buf;
    int len = 1, pos = 0;
    float store = 0;
    void alloc(int maxLen) { buf.assign(size_t(maxLen), 0.0f); }
    void setLen(int l) { len = std::clamp(l, 1, int(buf.size())); pos %= len; }
    void reset() { std::fill(buf.begin(), buf.end(), 0.0f); store = 0; pos = 0; }
    inline float process(float x, float fb, float damp) {
        float y = buf[size_t(pos)];
        store = y * (1 - damp) + store * damp;
        buf[size_t(pos)] = x + store * fb;
        if (++pos >= len) pos = 0;
        return y;
    }
};
struct Allpass {
    std::vector<float> buf;
    int len = 1, pos = 0;
    void alloc(int maxLen) { buf.assign(size_t(maxLen), 0.0f); }
    void setLen(int l) { len = std::clamp(l, 1, int(buf.size())); pos %= len; }
    void reset() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    inline float process(float x, float g = 0.5f) {
        float b = buf[size_t(pos)];
        float y = -x + b;
        buf[size_t(pos)] = x + b * g;
        if (++pos >= len) pos = 0;
        return y;
    }
};
}  // namespace

class Reverb : public Module {
public:
    enum { kType, kDecay, kPreDelay, kDamping, kLowCut, kMix };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"reverb", "Reverb", "Reverb algorítmico: sala, hall, placa ou mola.", Category::Reverb,
                            {Choice("type", "Tipo", {"Room", "Hall", "Plate", "Spring"}, 0),
                             P("decay", "Decay", 0, 100, 50, "%", 0), P("predelay", "Pre-Delay", 0, 200, 12, "ms", 0, 1.5f),
                             P("damping", "Damping", 0, 100, 40, "%", 0), P("lowcut", "Low Cut", 20, 600, 120, "Hz", 0, 2.0f),
                             P("mix", "Mix", 0, 100, 25, "%", 0)}};
        return i;
    }
    Reverb() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double sr, int) override {
        scale_ = sr / 44100.0;
        const int maxScale = int(std::ceil(1.3 * scale_));
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 8; ++k) combs_[c][k].alloc((kComb[k] + kSpread) * maxScale + 2);
            for (int k = 0; k < 4; ++k) aps_[c][k].alloc((kAp[k] + kSpread) * maxScale + 2);
            springDl_[c].prepare(int(0.06 * sr));
        }
        pre_.prepare(int(0.21 * sr));
        lastType_ = -1;
        onReset();
    }
    void onReset() override {
        for (int c = 0; c < 2; ++c) {
            for (auto& cb : combs_[c]) cb.reset();
            for (auto& ap : aps_[c]) ap.reset();
            for (float& z : springAp_[c]) z = 0;
            springDl_[c].reset();
            springLp_[c].reset();
            lowCut_[c].reset();
        }
        pre_.reset();
    }
    void configure(int type) {
        static const double sizes[] = {0.55, 1.15, 0.8, 0.5};
        for (int c = 0; c < 2; ++c) {
            int spread = c ? kSpread : 0;
            for (int k = 0; k < 8; ++k) combs_[c][k].setLen(int((kComb[k] + spread) * scale_ * sizes[type]));
            for (int k = 0; k < 4; ++k) aps_[c][k].setLen(int((kAp[k] + spread) * scale_ * (type == 2 ? 1.3 : 1.0)));
        }
        lastType_ = type;
        onReset();
    }
    void process(float* L, float* R, int n, bool) override {
        const int type = getInt(kType);
        if (type != lastType_) configure(type);
        const float decay = get(kDecay) * 0.01f;
        const float fb = (type == 1 ? 0.80f : 0.70f) + decay * (type == 1 ? 0.185f : 0.27f);
        const float damp = get(kDamping) * 0.01f * (type == 2 ? 0.25f : 0.45f);
        const float mix = get(kMix) * 0.01f;
        const float pre = std::max(2.0f, get(kPreDelay) * 0.001f * float(sr_));
        for (int c = 0; c < 2; ++c) {
            lowCut_[c].highpass(sr_, get(kLowCut));
            springLp_[c].setLowpass(sr_, 4200);
        }
        const float springFb = 0.55f + 0.4f * decay;
        for (int i = 0; i < n; ++i) {
            pre_.push(0.5f * (L[i] + R[i]));
            float in = pre_.read(pre);
            float out[2];
            for (int c = 0; c < 2; ++c) {
                float acc = 0;
                if (type == 3) {
                    // Spring tank: dispersive allpass chain inside a feedback loop.
                    float x = in + springDl_[c].read(float((c ? 0.041 : 0.037) * sr_)) * springFb;
                    for (int s = 0; s < 12; ++s) {
                        float y = -0.62f * x + springAp_[c][s];
                        springAp_[c][s] = x + 0.62f * y;
                        x = y;
                    }
                    x = springLp_[c].lp(x);
                    springDl_[c].push(fastTanh(x));
                    acc = x * 0.6f;
                } else {
                    float x = in * 0.03f;
                    for (auto& cb : combs_[c]) acc += cb.process(x, fb, damp);
                    for (auto& ap : aps_[c]) acc = ap.process(acc);
                }
                out[c] = lowCut_[c].process(acc);
            }
            L[i] = L[i] * (1 - mix * 0.5f) + out[0] * mix * 1.5f;
            R[i] = R[i] * (1 - mix * 0.5f) + out[1] * mix * 1.5f;
        }
    }

private:
    static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int kAp[4] = {556, 441, 341, 225};
    static constexpr int kSpread = 23;
    Comb combs_[2][8];
    Allpass aps_[2][4];
    float springAp_[2][12] = {};
    DelayLine springDl_[2], pre_;
    OnePole springLp_[2];
    Biquad lowCut_[2];
    double scale_ = 1;
    int lastType_ = -1;
};
TF_REGISTER(Reverb);
