// Chorus, flanger, phaser, vibe, tremolo/auto-pan and rotary speaker.
#include <cmath>

#include "dsp/Dsp.h"
#include "engine/Module.h"

using namespace dsp;

class Chorus : public Module {
public:
    enum { kRate, kDepth, kDelay, kMix };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"chorus", "Stereo Chorus", "Chorus estéreo com duas linhas moduladas.", Category::Modulation,
                            {P("rate", "Rate", 0.1f, 5, 0.8f, "Hz", 2, 2.0f), P("depth", "Depth", 0, 100, 50, "%", 0),
                             P("delay", "Delay", 4, 25, 11, "ms", 1), P("mix", "Mix", 0, 100, 50, "%", 0)}};
        return i;
    }
    Chorus() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double sr, int) override {
        for (auto& d : dl_) d.prepare(int(0.05 * sr));
        onReset();
    }
    void onReset() override { for (auto& d : dl_) d.reset(); }
    void process(float* L, float* R, int n, bool) override {
        lfo_.setRate(sr_, get(kRate));
        const float base = get(kDelay) * 0.001f * float(sr_);
        const float depth = get(kDepth) * 0.01f * 0.004f * float(sr_);
        const float mix = get(kMix) * 0.01f;
        for (int i = 0; i < n; ++i) {
            lfo_.advance();
            dl_[0].push(L[i]);
            dl_[1].push(R[i]);
            float wl = dl_[0].read(base + depth * lfo_.sineAt(0.0));
            float wr = dl_[1].read(base + depth * lfo_.sineAt(0.25));
            L[i] = L[i] * (1 - mix * 0.5f) + wl * mix;
            R[i] = R[i] * (1 - mix * 0.5f) + wr * mix;
        }
    }

private:
    DelayLine dl_[2];
    Lfo lfo_;
};
TF_REGISTER(Chorus);

// ---------------------------------------------------------------------------
class Flanger : public Module {
public:
    enum { kRate, kDepth, kManual, kFeedback, kMix };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"flanger", "Flanger", "Flanger estéreo com realimentação.", Category::Modulation,
                            {P("rate", "Rate", 0.05f, 5, 0.3f, "Hz", 2, 2.0f), P("depth", "Depth", 0, 100, 70, "%", 0),
                             P("manual", "Manual", 0.3f, 5, 1.6f, "ms", 2), P("feedback", "Feedback", -95, 95, 55, "%", 0),
                             P("mix", "Mix", 0, 100, 50, "%", 0)}};
        return i;
    }
    Flanger() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double sr, int) override {
        for (auto& d : dl_) d.prepare(int(0.02 * sr));
        onReset();
    }
    void onReset() override {
        for (auto& d : dl_) d.reset();
        fb_[0] = fb_[1] = 0;
    }
    void process(float* L, float* R, int n, bool) override {
        lfo_.setRate(sr_, get(kRate));
        const float manual = get(kManual) * 0.001f * float(sr_);
        const float depth = get(kDepth) * 0.01f * 0.9f;
        const float fb = get(kFeedback) * 0.01f, mix = get(kMix) * 0.01f;
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            lfo_.advance();
            for (int c = 0; c < 2; ++c) {
                float x = io[c][i];
                dl_[c].push(x + fb_[c] * fb);
                float d = manual * (1.0f + depth * lfo_.triAt(c * 0.25));
                float w = dl_[c].read(std::max(d, 2.0f));
                fb_[c] = fastTanh(w);
                io[c][i] = x * (1 - mix * 0.5f) + w * mix;
            }
        }
    }

private:
    DelayLine dl_[2];
    Lfo lfo_;
    float fb_[2] = {0, 0};
};
TF_REGISTER(Flanger);

// ---------------------------------------------------------------------------
namespace {
struct AllpassChain {
    float z[12] = {};
    float last = 0;
    void reset() { for (float& v : z) v = 0; last = 0; }
    inline float process(float x, const float* a, int stages) {
        for (int s = 0; s < stages; ++s) {
            float y = a[s] * x + z[s];
            z[s] = x - a[s] * y;
            x = y;
        }
        return x;
    }
};
inline float allpassCoef(double sr, double f) {
    double t = std::tan(kPi * std::clamp(f, 20.0, sr * 0.45) / sr);
    return float((t - 1) / (t + 1));
}
}  // namespace

class Phaser : public Module {
public:
    enum { kRate, kDepth, kCenter, kStages, kFeedback, kMix };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"phaser", "Phaser", "Phaser estéreo de 4 a 12 estágios.", Category::Modulation,
                            {P("rate", "Rate", 0.05f, 8, 0.5f, "Hz", 2, 2.0f), P("depth", "Depth", 0, 100, 80, "%", 0),
                             P("center", "Center", 200, 2000, 650, "Hz", 0, 2.0f),
                             Choice("stages", "Estágios", {"4", "6", "8", "12"}, 0),
                             P("feedback", "Feedback", 0, 90, 30, "%", 0), P("mix", "Mix", 0, 100, 50, "%", 0)}};
        return i;
    }
    Phaser() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double, int) override { onReset(); }
    void onReset() override { for (auto& c : ch_) c.reset(); }
    void process(float* L, float* R, int n, bool) override {
        static const int stageCounts[] = {4, 6, 8, 12};
        const int stages = stageCounts[getInt(kStages)];
        lfo_.setRate(sr_, get(kRate));
        const float depth = get(kDepth) * 0.01f * 2.0f;  // octaves each way
        const float center = get(kCenter), fb = get(kFeedback) * 0.01f, mix = get(kMix) * 0.01f;
        float* io[2] = {L, R};
        float a[2][12];
        for (int i = 0; i < n; ++i) {
            lfo_.advance();
            if ((i & 7) == 0) {
                for (int c = 0; c < 2; ++c) {
                    float f = center * std::pow(2.0f, depth * lfo_.sineAt(c * 0.25));
                    float co = allpassCoef(sr_, f);
                    for (int s = 0; s < stages; ++s) a[c][s] = co;
                }
            }
            for (int c = 0; c < 2; ++c) {
                float x = io[c][i];
                float y = ch_[c].process(x + ch_[c].last * fb, a[c], stages);
                ch_[c].last = y;
                io[c][i] = x * (1 - mix * 0.5f) + y * mix;
            }
        }
    }

private:
    AllpassChain ch_[2];
    Lfo lfo_;
};
TF_REGISTER(Phaser);

// ---------------------------------------------------------------------------
class Vibe : public Module {
public:
    enum { kSpeed, kIntensity, kMode, kLevel };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"vibe", "Uni-Vibe", "Phaser de 4 estágios desiguais com LFO de lâmpada (chorus/vibrato).",
                            Category::Modulation,
                            {P("speed", "Speed", 0.5f, 10, 3, "Hz", 2, 1.5f), P("intensity", "Intensity", 0, 100, 70, "%", 0),
                             Choice("mode", "Modo", {"Chorus", "Vibrato"}, 0), P("level", "Level", -12, 12, 0, "dB")}};
        return i;
    }
    Vibe() : Module(Info()) {}

protected:
    void onPrepare(double sr, int) override {
        lamp_.prepare(sr, 25, 0);
        onReset();
    }
    void onReset() override { for (auto& c : ch_) c.reset(); }
    void process(float* L, float* R, int n, bool stereo) override {
        static const float base[4] = {90, 320, 900, 2400};
        lfo_.setRate(sr_, get(kSpeed));
        const float inten = get(kIntensity) * 0.01f;
        const bool vibrato = getInt(kMode) == 1;
        const float level = dbToGain(get(kLevel));
        float a[4] = {};
        for (int i = 0; i < n; ++i) {
            lfo_.advance();
            // Lamp brightness lags the drive signal and is skewed toward dark.
            float s = 0.5f + 0.5f * lfo_.sineAt();
            lamp_.setTarget(s * s);
            float b = lamp_.next();
            if ((i & 7) == 0)
                for (int k = 0; k < 4; ++k) a[k] = allpassCoef(sr_, base[k] * std::pow(2.0f, inten * 2.5f * (b - 0.5f)));
            for (int c = 0; c < (stereo ? 2 : 1); ++c) {
                float* x = c ? R : L;
                float y = ch_[c].process(x[i], a, 4);
                x[i] = (vibrato ? y : 0.5f * (x[i] + y)) * level * (vibrato ? 1.0f : 1.3f);
            }
        }
    }

private:
    AllpassChain ch_[2];
    Lfo lfo_;
    Smoother lamp_;
};
TF_REGISTER(Vibe);

// ---------------------------------------------------------------------------
class Tremolo : public Module {
public:
    enum { kRate, kDepth, kShape, kMode };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"tremolo", "Tremolo / Pan", "Modulação de volume; no modo Pan alterna entre L e R.",
                            Category::Modulation,
                            {P("rate", "Rate", 0.5f, 15, 5, "Hz", 2, 1.5f), P("depth", "Depth", 0, 100, 60, "%", 0),
                             Choice("shape", "Forma", {"Seno", "Triângulo", "Quadrada"}, 0),
                             Choice("mode", "Modo", {"Mono", "Pan"}, 0)}};
        return i;
    }
    Tremolo() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double, int) override {}
    void process(float* L, float* R, int n, bool) override {
        lfo_.setRate(sr_, get(kRate));
        const float depth = get(kDepth) * 0.01f;
        const int shape = getInt(kShape);
        const bool pan = getInt(kMode) == 1;
        for (int i = 0; i < n; ++i) {
            lfo_.advance();
            float m = shape == 0 ? Lfo::sine(lfo_.phase) : shape == 1 ? Lfo::triangle(lfo_.phase) : Lfo::softSquare(lfo_.phase);
            float gl = 1 - depth * (0.5f - 0.5f * m);
            float gr = pan ? 1 - depth * (0.5f + 0.5f * m) : gl;
            L[i] *= gl;
            R[i] *= gr;
        }
    }

private:
    Lfo lfo_;
};
TF_REGISTER(Tremolo);

// ---------------------------------------------------------------------------
class Rotary : public Module {
public:
    enum { kSpeed, kBalance, kDrive, kWidth, kLevel };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"rotary", "Rotary Speaker", "Caixa rotativa: corneta e tambor com aceleração realista.",
                            Category::Modulation,
                            {Choice("speed", "Speed", {"Slow", "Fast"}, 0), P("balance", "Horn/Drum", 0, 100, 55, "%", 0),
                             P("drive", "Drive", 0, 10, 2), P("width", "Width", 0, 100, 80, "%", 0),
                             P("level", "Level", -12, 12, 0, "dB")}};
        return i;
    }
    Rotary() : Module(Info()) {}

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double sr, int) override {
        for (int k = 0; k < 2; ++k) {
            lp_[k].lowpass(sr, 800);
            hp_[k].highpass(sr, 800);
        }
        horn_.prepare(int(0.01 * sr));
        drum_.prepare(int(0.01 * sr));
        hornHz_ = 0.83;
        drumHz_ = 0.67;
        onReset();
    }
    void onReset() override {
        for (int k = 0; k < 2; ++k) { lp_[k].reset(); hp_[k].reset(); }
        horn_.reset();
        drum_.reset();
    }
    void process(float* L, float* R, int n, bool stereo) override {
        const bool fast = getInt(kSpeed) == 1;
        const double hornTarget = fast ? 6.7 : 0.83, drumTarget = fast ? 5.9 : 0.67;
        const double hornTau = (fast ? 0.6 : 0.9) * sr_, drumTau = (fast ? 3.0 : 4.0) * sr_;
        const float bal = get(kBalance) * 0.01f, width = get(kWidth) * 0.01f, level = dbToGain(get(kLevel));
        const float drive = 1.0f + get(kDrive) * 0.8f, driveNorm = 1.0f / fastTanh(drive * 0.5f);
        const float msHorn = 0.001f * float(sr_), base = 2.0f * msHorn;
        for (int i = 0; i < n; ++i) {
            hornHz_ += (hornTarget - hornHz_) / hornTau;
            drumHz_ += (drumTarget - drumHz_) / drumTau;
            hornPh_ += hornHz_ / sr_; if (hornPh_ >= 1) hornPh_ -= 1;
            drumPh_ += drumHz_ / sr_; if (drumPh_ >= 1) drumPh_ -= 1;

            float x = stereo ? 0.5f * (L[i] + R[i]) : L[i];
            x = fastTanh(x * drive * 0.5f) * driveNorm * 0.5f;
            float hi = hp_[1].process(hp_[0].process(x));
            float lo = lp_[1].process(lp_[0].process(x));
            horn_.push(hi);
            drum_.push(lo);
            float out[2];
            for (int m = 0; m < 2; ++m) {
                double ah = hornPh_ + m * 0.25, ad = drumPh_ + m * 0.25;
                float h = horn_.read(base + 0.45f * msHorn * Lfo::sine(ah)) * (1.0f - 0.4f * (0.5f + 0.5f * Lfo::sine(ah + 0.25)));
                float d = drum_.read(base + 0.08f * msHorn * Lfo::sine(ad)) * (1.0f - 0.25f * (0.5f + 0.5f * Lfo::sine(ad + 0.25)));
                out[m] = (h * bal + d * (1 - bal)) * 3.2f;
            }
            float mid = 0.5f * (out[0] + out[1]), side = 0.5f * (out[0] - out[1]) * width;
            L[i] = (mid + side) * level;
            R[i] = (mid - side) * level;
        }
    }

private:
    Biquad lp_[2], hp_[2];
    DelayLine horn_, drum_;
    double hornHz_ = 0.83, drumHz_ = 0.67, hornPh_ = 0, drumPh_ = 0.3;
};
TF_REGISTER(Rotary);
