// Wah / auto-wah and equalizers.
#include <cmath>

#include "dsp/Dsp.h"
#include "engine/Module.h"

using namespace dsp;

class Wah : public Module {
public:
    enum { kPedal, kMode, kSens, kRate, kQ, kRange, kLevel };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"wah", "Wah / Auto-Wah",
                            "Filtro ressonante. Manual (mapeie o Pedal num controlador MIDI), Auto (segue a palhetada) ou LFO.",
                            Category::Filter,
                            {P("pedal", "Pedal", 0, 100, 50, "%", 0), Choice("mode", "Modo", {"Manual", "Auto", "LFO"}, 0),
                             P("sens", "Sensib.", 0, 10, 5), P("rate", "Rate", 0.1f, 10, 1, "Hz", 2, 2.0f),
                             P("q", "Q", 1, 15, 6, "", 1), Choice("range", "Range", {"Clássico", "Largo", "Baixo"}, 0),
                             P("level", "Level", -12, 12, 0, "dB")}};
        return i;
    }
    Wah() : Module(Info()) {}

protected:
    void onPrepare(double sr, int) override {
        env_.prepare(sr, 4, 120);
        onReset();
    }
    void onReset() override {
        for (auto& f : svf_) f.reset();
        env_.reset();
    }
    void process(float* L, float* R, int n, bool stereo) override {
        static const float lo[] = {420, 260, 160}, hi[] = {2300, 3200, 1300};
        const int mode = getInt(kMode), range = getInt(kRange);
        const float q = get(kQ), sens = get(kSens) / 10, level = dbToGain(get(kLevel));
        lfo_.setRate(sr_, get(kRate));
        float pos = get(kPedal) * 0.01f;
        for (int i = 0; i < n; ++i) {
            if (mode == 1) {
                float e = env_.process(stereo ? 0.5f * (L[i] + R[i]) : L[i]);
                pos = std::clamp(e * (2.0f + 40.0f * sens * sens), 0.0f, 1.0f);
            } else if (mode == 2) {
                lfo_.advance();
                pos = 0.5f + 0.5f * lfo_.sineAt();
            }
            if ((i & 7) == 0) {
                float f = lo[range] * std::pow(hi[range] / lo[range], pos);
                svf_[0].set(sr_, f, q);
                svf_[1].set(sr_, f, q);
            }
            for (int c = 0; c < (stereo ? 2 : 1); ++c) {
                float* x = c ? R : L;
                svf_[c].process(x[i]);
                x[i] = (svf_[c].bp * float(svf_[c].k) * 3.0f + svf_[c].lp * 0.25f) * level;
            }
        }
    }

private:
    Svf svf_[2];
    EnvelopeFollower env_;
    Lfo lfo_;
};
TF_REGISTER(Wah);

// ---------------------------------------------------------------------------
class GraphicEq : public Module {
public:
    static constexpr int kBands = 7;
    static const ModuleInfo& Info() {
        static ModuleInfo i = [] {
            ModuleInfo m{"geq7", "Graphic EQ 7", "Equalizador gráfico de 7 bandas.", Category::EQ, {}};
            const char* names[] = {"100", "200", "400", "800", "1.6k", "3.2k", "6.4k"};
            const char* ids[] = {"b100", "b200", "b400", "b800", "b1600", "b3200", "b6400"};
            for (int b = 0; b < kBands; ++b) m.params.push_back(P(ids[b], names[b], -15, 15, 0, "dB"));
            m.params.push_back(P("level", "Level", -15, 15, 0, "dB"));
            return m;
        }();
        return i;
    }
    GraphicEq() : Module(Info()) {}

protected:
    void onPrepare(double, int) override { onReset(); last_[0] = 1e9f; }
    void onReset() override { for (auto& ch : f_) for (auto& b : ch) b.reset(); }
    void process(float* L, float* R, int n, bool stereo) override {
        static const float freqs[kBands] = {100, 200, 400, 800, 1600, 3200, 6400};
        bool changed = false;
        for (int b = 0; b < kBands; ++b) changed |= get(b) != last_[b];
        if (changed) {
            for (int b = 0; b < kBands; ++b) {
                last_[b] = get(b);
                for (auto& ch : f_) ch[b].peak(sr_, freqs[b], 1.4, last_[b]);
            }
        }
        const float level = dbToGain(get(kBands));
        for (int c = 0; c < (stereo ? 2 : 1); ++c) {
            float* x = c ? R : L;
            for (int i = 0; i < n; ++i) {
                float v = x[i];
                for (auto& b : f_[c]) v = b.process(v);
                x[i] = v * level;
            }
        }
    }

private:
    Biquad f_[2][kBands];
    float last_[kBands] = {};
};
TF_REGISTER(GraphicEq);

// ---------------------------------------------------------------------------
class ParametricEq : public Module {
public:
    enum { kLowCut, kLowF, kLowG, kM1F, kM1G, kM1Q, kM2F, kM2G, kM2Q, kHighF, kHighG, kHighCut, kLevel, kCount };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"peq", "Parametric EQ", "EQ paramétrico de estúdio: cortes, 2 shelves e 2 médios.", Category::EQ,
                            {P("lowcut", "Low Cut", 20, 500, 20, "Hz", 0, 2.0f), P("lowf", "Low Freq", 40, 500, 100, "Hz", 0, 2.0f),
                             P("lowg", "Low", -15, 15, 0, "dB"), P("m1f", "Mid1 Freq", 150, 2000, 500, "Hz", 0, 2.0f),
                             P("m1g", "Mid1", -15, 15, 0, "dB"), P("m1q", "Mid1 Q", 0.3f, 6, 1, "", 2, 2.0f),
                             P("m2f", "Mid2 Freq", 800, 8000, 2500, "Hz", 0, 2.0f), P("m2g", "Mid2", -15, 15, 0, "dB"),
                             P("m2q", "Mid2 Q", 0.3f, 6, 1, "", 2, 2.0f), P("highf", "High Freq", 2000, 12000, 5000, "Hz", 0, 2.0f),
                             P("highg", "High", -15, 15, 0, "dB"), P("highcut", "High Cut", 2000, 20000, 20000, "Hz", 0, 2.0f),
                             P("level", "Level", -15, 15, 0, "dB")}};
        return i;
    }
    ParametricEq() : Module(Info()) {}

protected:
    void onPrepare(double, int) override { onReset(); for (auto& v : last_) v = 1e9f; }
    void onReset() override { for (auto& ch : f_) for (auto& b : ch) b.reset(); }
    void process(float* L, float* R, int n, bool stereo) override {
        bool changed = false;
        for (int p = 0; p < kLevel; ++p) changed |= get(p) != last_[p];
        if (changed) {
            for (int p = 0; p < kLevel; ++p) last_[p] = get(p);
            for (auto& ch : f_) {
                ch[0].highpass(sr_, last_[kLowCut]);
                ch[1].lowShelf(sr_, last_[kLowF], last_[kLowG]);
                ch[2].peak(sr_, last_[kM1F], last_[kM1Q], last_[kM1G]);
                ch[3].peak(sr_, last_[kM2F], last_[kM2Q], last_[kM2G]);
                ch[4].highShelf(sr_, last_[kHighF], last_[kHighG]);
                if (last_[kHighCut] >= 19999) ch[5].identity(); else ch[5].lowpass(sr_, last_[kHighCut]);
            }
        }
        const float level = dbToGain(get(kLevel));
        for (int c = 0; c < (stereo ? 2 : 1); ++c) {
            float* x = c ? R : L;
            for (int i = 0; i < n; ++i) {
                float v = x[i];
                for (auto& b : f_[c]) v = b.process(v);
                x[i] = v * level;
            }
        }
    }

private:
    Biquad f_[2][6];
    float last_[kCount] = {};
};
TF_REGISTER(ParametricEq);
