// Overdrive, distortion, fuzz and boost pedals. Nonlinear stages run at 2x.
#include <cmath>

#include "dsp/Dsp.h"
#include "dsp/Oversampler.h"
#include "engine/Module.h"

using namespace dsp;

namespace {

// Pedal skeleton: linear pre/post filtering at the base rate, the clipping
// stage in between at twice the sample rate.
class DriveBase : public Module {
public:
    using Module::Module;

protected:
    void onPrepare(double sr, int maxBlock) override {
        for (auto& o : os_) o.prepare(2, 16, maxBlock);
        osr_ = sr * 2;
        for (auto& d : dc_) d.prepare(sr, 15);
        prepareDrive(sr);
        onReset();
    }
    void onReset() override {
        for (auto& o : os_) o.reset();
        for (auto& d : dc_) d.reset();
        resetDrive();
    }
    void process(float* L, float* R, int n, bool stereo) override {
        update();
        runChannel(0, L, n);
        if (stereo) runChannel(1, R, n);
    }
    virtual void prepareDrive(double sr) = 0;
    virtual void resetDrive() {}
    virtual void update() = 0;
    virtual void shape(int c, float* x, int n) = 0;  // oversampled
    virtual void post(int c, float* x, int n) = 0;   // base rate

    double osr_ = 96000;

private:
    void runChannel(int c, float* x, int n) {
        os_[c].up(x, n);
        shape(c, os_[c].buffer(), n * 2);
        os_[c].down(x, n);
        for (int i = 0; i < n; ++i) x[i] = dc_[c].process(x[i]);
        post(c, x, n);
    }
    Oversampler os_[2];
    DcBlocker dc_[2];
};

float logLerp(float a, float b, float t) { return a * std::pow(b / a, t); }

}  // namespace

// ---------------------------------------------------------------------------
class Overdrive : public DriveBase {
public:
    enum { kDrive, kTone, kLevel };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"overdrive", "Green Overdrive",
                            "Overdrive de médios estilo 'screamer': soma o sinal limpo com o clipado.", Category::Drive,
                            {P("drive", "Drive", 0, 10, 5), P("tone", "Tone", 0, 10, 5), P("level", "Level", -20, 20, -6, "dB")}};
        return i;
    }
    Overdrive() : DriveBase(Info()) {}

protected:
    void prepareDrive(double) override {
        for (int c = 0; c < 2; ++c) {
            clipHp_[c].setLowpass(osr_, 720);
            clipLp_[c].setLowpass(osr_, 6000);
        }
    }
    void resetDrive() override {
        for (int c = 0; c < 2; ++c) { clipHp_[c].reset(); clipLp_[c].reset(); tone_[c].reset(); }
    }
    void update() override {
        gain_ = dbToGain(lerp(6, 42, get(kDrive) / 10));
        for (auto& t : tone_) t.setLowpass(sr_, logLerp(900, 7500, get(kTone) / 10));
        level_ = dbToGain(get(kLevel));
    }
    void shape(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) {
            float clip = 0.45f * fastTanh(gain_ * clipHp_[c].hp(x[i]) / 0.45f);
            x[i] = x[i] + clipLp_[c].lp(clip);
        }
    }
    void post(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) x[i] = tone_[c].lp(x[i]) * level_;
    }

private:
    OnePole clipHp_[2], clipLp_[2], tone_[2];
    float gain_ = 1, level_ = 1;
};
TF_REGISTER(Overdrive);

// ---------------------------------------------------------------------------
class Distortion : public DriveBase {
public:
    enum { kDist, kFilter, kVolume };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"distortion", "Rodent Distortion", "Distorção de op-amp com diodos e filtro passa-baixa.",
                            Category::Drive,
                            {P("distortion", "Distortion", 0, 10, 5), P("filter", "Filter", 0, 10, 4),
                             P("volume", "Volume", -20, 20, -12, "dB")}};
        return i;
    }
    Distortion() : DriveBase(Info()) {}

protected:
    void prepareDrive(double) override {}
    void resetDrive() override {
        for (int c = 0; c < 2; ++c) { hp_[c].reset(); gbw_[c].reset(); filt_[c].reset(); }
    }
    void update() override {
        float d = get(kDist) / 10;
        gain_ = dbToGain(lerp(3, 64, d));
        float gbw = std::clamp(300000.0f / gain_, 1800.0f, 20000.0f);
        for (int c = 0; c < 2; ++c) {
            hp_[c].setLowpass(osr_, lerp(60, 320, d));
            gbw_[c].setLowpass(osr_, gbw);
            filt_[c].setLowpass(sr_, logLerp(16000, 600, get(kFilter) / 10));
        }
        vol_ = dbToGain(get(kVolume));
    }
    void shape(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) {
            float v = gbw_[c].lp(gain_ * hp_[c].hp(x[i]));
            x[i] = 0.7f * softClipK(v / 0.7f, 2.5f);
        }
    }
    void post(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) x[i] = filt_[c].lp(x[i]) * vol_;
    }

private:
    OnePole hp_[2], gbw_[2], filt_[2];
    float gain_ = 1, vol_ = 1;
};
TF_REGISTER(Distortion);

// ---------------------------------------------------------------------------
class Fuzz : public DriveBase {
public:
    enum { kFuzz, kBias, kVolume };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"fuzz", "Round Fuzz", "Fuzz de dois transistores. Bias baixo deixa o som 'engasgado'.",
                            Category::Drive,
                            {P("fuzz", "Fuzz", 0, 10, 7), P("bias", "Bias", 0, 10, 8), P("volume", "Volume", -20, 20, -12, "dB")}};
        return i;
    }
    Fuzz() : DriveBase(Info()) {}

protected:
    void prepareDrive(double) override {
        for (int c = 0; c < 2; ++c) {
            inHp_[c].setLowpass(osr_, 45);
            env_[c].prepare(osr_, 1, 60);
        }
    }
    void resetDrive() override {
        for (int c = 0; c < 2; ++c) { inHp_[c].reset(); env_[c].reset(); out_[c].reset(); }
    }
    void update() override {
        gain_ = dbToGain(lerp(12, 52, get(kFuzz) / 10));
        float b = get(kBias) / 10;
        bias_ = lerp(0.45f, 0.08f, b);
        gate_ = lerp(0.03f, 0.0f, b);
        for (auto& o : out_) o.setLowpass(sr_, 5500);
        vol_ = dbToGain(get(kVolume));
    }
    void shape(int c, float* x, int n) override {
        const float off = fastTanh(bias_);
        for (int i = 0; i < n; ++i) {
            float in = inHp_[c].hp(x[i]);
            float e = env_[c].process(in);
            float g = gate_ > 0 ? std::clamp((e - gate_) / gate_, 0.0f, 1.0f) : 1.0f;
            float v = gain_ * in + bias_;
            // Soft saturation on top, harder cutoff at the bottom.
            float y = v > 0 ? fastTanh(v) : -softClipK(-v * 1.6f, 4.0f) / 1.6f;
            x[i] = (y - off) * g * 0.8f;
        }
    }
    void post(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) x[i] = out_[c].lp(x[i]) * vol_;
    }

private:
    OnePole inHp_[2], out_[2];
    EnvelopeFollower env_[2];
    float gain_ = 1, bias_ = 0, gate_ = 0, vol_ = 1;
};
TF_REGISTER(Fuzz);

// ---------------------------------------------------------------------------
class BigFuzz : public DriveBase {
public:
    enum { kSustain, kTone, kVolume };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"bigfuzz", "Big Fuzz", "Fuzz de sustain longo com controle de tom que escava os médios.",
                            Category::Drive,
                            {P("sustain", "Sustain", 0, 10, 6), P("tone", "Tone", 0, 10, 5), P("volume", "Volume", -20, 20, -6, "dB")}};
        return i;
    }
    BigFuzz() : DriveBase(Info()) {}

protected:
    void prepareDrive(double sr) override {
        for (int c = 0; c < 2; ++c) {
            inHp_[c].setLowpass(osr_, 90);
            lp1_[c].setLowpass(osr_, 4500);
            lp2_[c].setLowpass(osr_, 3200);
            toneLp_[c].setLowpass(sr, 480);
            toneHp_[c].setLowpass(sr, 1100);
        }
    }
    void resetDrive() override {
        for (int c = 0; c < 2; ++c) { inHp_[c].reset(); lp1_[c].reset(); lp2_[c].reset(); toneLp_[c].reset(); toneHp_[c].reset(); }
    }
    void update() override {
        g1_ = dbToGain(lerp(8, 42, get(kSustain) / 10));
        tone_ = get(kTone) / 10;
        vol_ = dbToGain(get(kVolume));
    }
    void shape(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) {
            float v = fastTanh(g1_ * inHp_[c].hp(x[i]));
            v = lp1_[c].lp(v);
            v = fastTanh(6.0f * v);
            x[i] = lp2_[c].lp(v) * 0.7f;
        }
    }
    void post(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) {
            float lo = toneLp_[c].lp(x[i]);
            float hi = toneHp_[c].hp(x[i]) * 1.5f;
            x[i] = lerp(lo, hi, tone_) * vol_;
        }
    }

private:
    OnePole inHp_[2], lp1_[2], lp2_[2], toneLp_[2], toneHp_[2];
    float g1_ = 1, tone_ = 0.5f, vol_ = 1;
};
TF_REGISTER(BigFuzz);

// ---------------------------------------------------------------------------
class TrebleBooster : public DriveBase {
public:
    enum { kBoost, kRange, kLevel };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"trebleboost", "Treble Booster", "Booster de germânio para empurrar o amp.", Category::Drive,
                            {P("boost", "Boost", 0, 10, 7), Choice("range", "Range", {"Treble", "Mid", "Full"}, 0),
                             P("level", "Level", -20, 20, -6, "dB")}};
        return i;
    }
    TrebleBooster() : DriveBase(Info()) {}

protected:
    void prepareDrive(double) override {}
    void resetDrive() override { for (auto& h : hp_) h.reset(); }
    void update() override {
        static const float freqs[] = {1800, 700, 120};
        for (auto& h : hp_) h.setLowpass(osr_, freqs[getInt(kRange)]);
        gain_ = dbToGain(lerp(0, 26, get(kBoost) / 10));
        level_ = dbToGain(get(kLevel));
    }
    void shape(int c, float* x, int n) override {
        for (int i = 0; i < n; ++i) {
            float v = gain_ * (hp_[c].hp(x[i]) + 0.2f * x[i]);
            x[i] = 0.6f * diodeClip(v / 0.6f, 0.35f);
        }
    }
    void post(int, float* x, int n) override {
        for (int i = 0; i < n; ++i) x[i] *= level_;
    }

private:
    OnePole hp_[2];
    float gain_ = 1, level_ = 1;
};
TF_REGISTER(TrebleBooster);

// ---------------------------------------------------------------------------
class CleanBoost : public Module {
public:
    enum { kGain, kBass, kTreble };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"cleanboost", "Clean Boost", "Ganho limpo com EQ simples de graves e agudos.", Category::Drive,
                            {P("gain", "Gain", 0, 24, 9, "dB"), P("bass", "Bass", -9, 9, 0, "dB"),
                             P("treble", "Treble", -9, 9, 0, "dB")}};
        return i;
    }
    CleanBoost() : Module(Info()) {}

protected:
    void onPrepare(double, int) override { onReset(); }
    void onReset() override { for (int c = 0; c < 2; ++c) { lo_[c].reset(); hi_[c].reset(); } }
    void process(float* L, float* R, int n, bool stereo) override {
        for (int c = 0; c < 2; ++c) {
            lo_[c].lowShelf(sr_, 120, get(kBass));
            hi_[c].highShelf(sr_, 3000, get(kTreble));
        }
        float g = dbToGain(get(kGain));
        for (int c = 0; c < (stereo ? 2 : 1); ++c) {
            float* x = c ? R : L;
            for (int i = 0; i < n; ++i) x[i] = softClipK(hi_[c].process(lo_[c].process(x[i])) * g, 6.0f);
        }
    }

private:
    Biquad lo_[2], hi_[2];
};
TF_REGISTER(CleanBoost);

// ---------------------------------------------------------------------------
class BitCrusher : public Module {
public:
    enum { kBits, kDownsample, kMix };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"crusher", "Bit Crusher", "Reduz resolução e taxa de amostragem (lo-fi).", Category::Drive,
                            {P("bits", "Bits", 2, 16, 8, "", 0), P("downsample", "Downsample", 1, 40, 4, "x", 0, 2.0f),
                             P("mix", "Mix", 0, 100, 100, "%", 0)}};
        return i;
    }
    BitCrusher() : Module(Info()) {}

protected:
    void onPrepare(double, int) override { onReset(); }
    void onReset() override { held_[0] = held_[1] = 0; count_ = 0; }
    void process(float* L, float* R, int n, bool stereo) override {
        const float steps = std::pow(2.0f, get(kBits) - 1);
        const int ds = std::max(1, int(get(kDownsample)));
        const float mix = get(kMix) * 0.01f;
        for (int i = 0; i < n; ++i) {
            if (count_ == 0) {
                held_[0] = std::round(L[i] * steps) / steps;
                if (stereo) held_[1] = std::round(R[i] * steps) / steps;
            }
            count_ = (count_ + 1) % ds;
            L[i] = lerp(L[i], held_[0], mix);
            if (stereo) R[i] = lerp(R[i], held_[1], mix);
        }
    }

private:
    float held_[2] = {0, 0};
    int count_ = 0;
};
TF_REGISTER(BitCrusher);
