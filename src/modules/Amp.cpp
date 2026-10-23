// Tube amp model: preamp triode stages -> passive tone stack -> power amp with
// supply sag, presence and depth. Everything nonlinear runs at 4x oversampling.
// Voicings are original approximations of classic amp families.
#include <cmath>

#include "dsp/Dsp.h"
#include "dsp/Oversampler.h"
#include "dsp/ToneStack.h"
#include "engine/Module.h"

using namespace dsp;

namespace {

struct Voicing {
    const char* name;
    float inHp;                  // input coupling high-pass (Hz)
    float gainLoDb, gainHiDb;    // range of the Gain knob before stage 1
    int stages;
    float stageGain[4], stageBias[4], stageLp[4], stageHp[4];
    int toneStackAfter;          // stage count before the tone stack
    ToneStackComponents ts;
    float brightDb;
    float powerDrive, powerAsym, sag;
    float presenceHz, depthHz;
    float outDb;
    bool solidState;
};

constexpr double pF = 1e-12, nF = 1e-9, k = 1e3, M = 1e6;
const ToneStackComponents kTsBassman{250 * k, 1 * M, 25 * k, 56 * k, 250 * pF, 20 * nF, 20 * nF};
const ToneStackComponents kTsBlackface{250 * k, 250 * k, 10 * k, 100 * k, 250 * pF, 100 * nF, 47 * nF};
const ToneStackComponents kTsBrit{220 * k, 1 * M, 22 * k, 33 * k, 470 * pF, 22 * nF, 22 * nF};
const ToneStackComponents kTsBritLead{220 * k, 1 * M, 25 * k, 33 * k, 470 * pF, 22 * nF, 22 * nF};
const ToneStackComponents kTsModern{250 * k, 1 * M, 25 * k, 47 * k, 500 * pF, 22 * nF, 22 * nF};
const ToneStackComponents kTsChime{1 * M, 1 * M, 10 * k, 100 * k, 50 * pF, 22 * nF, 22 * nF};

const Voicing kVoicings[] = {
    {"Tweed '57 (Clean/Crunch)", 60, -12, 18, 2, {2.0f, 3.0f}, {0.25f, 0.4f}, {9000, 7000}, {40, 60}, 2, kTsBassman,
     0, 2.5f, 0.15f, 0.6f, 3500, 100, -6.5f, false},
    {"US Clean '65", 70, -18, 10, 2, {1.6f, 1.8f}, {0.15f, 0.2f}, {12000, 10000}, {30, 50}, 1, kTsBlackface,
     6, 1.4f, 0.05f, 0.15f, 4000, 90, 4.2f, false},
    {"Brit Plexi '59", 80, -12, 22, 3, {2.5f, 3.0f, 2.0f}, {0.3f, 0.35f, 0.2f}, {10000, 8000, 7000}, {60, 80, 40}, 3,
     kTsBrit, 4, 3.0f, 0.1f, 0.35f, 4500, 90, -11.5f, false},
    {"Brit Lead 800", 120, -6, 30, 4, {3.5f, 4.0f, 3.0f, 1.5f}, {0.5f, 0.3f, 0.3f, 0.15f}, {8000, 7000, 6500, 7000},
     {120, 100, 60, 40}, 4, kTsBritLead, 3, 2.2f, 0.1f, 0.25f, 4000, 80, -12.5f, false},
    {"Modern Hi-Gain", 150, 0, 36, 4, {4.0f, 5.0f, 4.0f, 2.0f}, {0.45f, 0.3f, 0.4f, 0.2f}, {7000, 6500, 6000, 6500},
     {150, 120, 80, 60}, 4, kTsModern, 2, 2.8f, 0.08f, 0.4f, 4500, 75, -14, false},
    {"AC Chime (Top Boost)", 70, -12, 20, 2, {2.2f, 2.6f}, {0.2f, 0.3f}, {14000, 11000}, {50, 70}, 2, kTsChime,
     3, 3.5f, 0.25f, 0.5f, 3000, 100, -12.8f, false},
    {"Bass Tube 300", 25, -18, 16, 2, {1.8f, 2.2f}, {0.2f, 0.25f}, {9000, 8000}, {15, 20}, 2, kTsBassman,
     0, 1.8f, 0.1f, 0.3f, 3000, 60, -0.6f, false},
    {"Solid Clean 120", 50, -18, 6, 1, {1.3f}, {0.0f}, {16000}, {30}, 1, kTsBlackface,
     4, 1.0f, 0.0f, 0.0f, 5000, 100, 13.9f, true},
};
constexpr int kNumVoicings = int(sizeof(kVoicings) / sizeof(kVoicings[0]));

std::vector<std::string> voicingNames() {
    std::vector<std::string> v;
    for (auto& vc : kVoicings) v.push_back(vc.name);
    return v;
}

}  // namespace

class Amp : public Module {
public:
    enum { kModel, kGain, kBass, kMiddle, kTreble, kPresence, kDepth, kMaster, kVolume, kBright, kCount };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"amp", "Amplificador", "Pré de válvulas, tone stack passivo e power amp com sag.", Category::Amp,
                            {Choice("model", "Modelo", voicingNames(), 2), P("gain", "Gain", 0, 10, 5),
                             P("bass", "Bass", 0, 10, 5), P("middle", "Middle", 0, 10, 5), P("treble", "Treble", 0, 10, 5),
                             P("presence", "Presence", 0, 10, 5), P("depth", "Depth", 0, 10, 5),
                             P("master", "Master", 0, 10, 6), P("volume", "Volume", -24, 12, 0, "dB"),
                             Toggle("bright", "Bright")}};
        return i;
    }
    Amp() : Module(Info()) {}

protected:
    void onPrepare(double sr, int maxBlock) override {
        osr_ = sr * 4;
        for (auto& c : ch_) {
            c.os.prepare(4, 24, maxBlock);
            c.ts.prepare(osr_);
            for (auto& e : c.dyn) e.prepare(osr_, 5, 80);
            c.sagEnv.prepare(osr_, 10, 150);
            c.dc.prepare(sr, 8);
        }
        for (float& v : last_) v = -1e9f;
        update();
        onReset();
    }
    void onReset() override {
        for (auto& c : ch_) {
            c.os.reset();
            c.inHp.reset();
            c.bright.reset();
            c.ts.reset();
            for (int s = 0; s < 4; ++s) { c.lp[s].reset(); c.hp[s].reset(); c.dyn[s].reset(); }
            c.sagEnv.reset();
            c.presence.reset();
            c.depth.reset();
            c.dc.reset();
        }
    }

    void update() {
        bool modelChanged = getInt(kModel) != int(last_[kModel]);
        bool tsChanged = modelChanged;
        for (int p : {kBass, kMiddle, kTreble}) tsChanged |= get(p) != last_[p];
        bool eqChanged = modelChanged || get(kPresence) != last_[kPresence] || get(kDepth) != last_[kDepth] ||
                         get(kBright) != last_[kBright] || get(kGain) != last_[kGain];
        for (int p = 0; p < kCount; ++p) last_[p] = get(p);
        const Voicing& v = kVoicings[std::clamp(getInt(kModel), 0, kNumVoicings - 1)];
        voicing_ = &v;

        if (modelChanged) {
            for (auto& c : ch_) {
                c.inHp.setLowpass(osr_, v.inHp);
                for (int s = 0; s < v.stages; ++s) {
                    c.lp[s].setLowpass(osr_, v.stageLp[s]);
                    c.hp[s].setLowpass(osr_, v.stageHp[s]);
                }
            }
            // Normalize the tone stack so that noon settings sit near 0 dB.
            ToneStack probe;
            probe.prepare(osr_);
            probe.set(v.ts, 0.5, 0.5, 0.5);
            tsMakeup_ = float(1.0 / std::max(1e-4, probe.magnitude(700)));
            onReset();
        }
        if (tsChanged)
            for (auto& c : ch_) c.ts.set(v.ts, get(kTreble) / 10, get(kMiddle) / 10, get(kBass) / 10);
        if (eqChanged) {
            float gain01 = get(kGain) / 10;
            float brightDb = getBool(kBright) ? v.brightDb * (1 - 0.6f * gain01) : 0.0f;
            for (auto& c : ch_) {
                if (brightDb > 0.01f) c.bright.highShelf(osr_, 2500, brightDb); else c.bright.identity();
                c.presence.highShelf(sr_, v.presenceHz, lerp(-6, 8, get(kPresence) / 10));
                c.depth.peak(sr_, v.depthHz, 0.8, lerp(-3, 9, get(kDepth) / 10));
            }
        }
        preGain_ = dbToGain(lerp(v.gainLoDb, v.gainHiDb, get(kGain) / 10));
        masterGain_ = dbToGain(lerp(-30, 0, get(kMaster) / 10)) * v.powerDrive;
        outGain_ = dbToGain(get(kVolume) + v.outDb);
    }

    void process(float* L, float* R, int n, bool stereo) override {
        update();
        runChannel(ch_[0], L, n);
        if (stereo) runChannel(ch_[1], R, n);
    }

private:
    struct Ch {
        Oversampler os;
        OnePole inHp, lp[4], hp[4];
        Biquad bright, presence, depth;
        EnvelopeFollower dyn[4], sagEnv;
        ToneStack ts;
        DcBlocker dc;
    };

    void runChannel(Ch& c, float* x, int n) {
        const Voicing& v = *voicing_;
        c.os.up(x, n);
        float* b = c.os.buffer();
        const int m = n * 4;
        for (int i = 0; i < m; ++i) {
            float s = c.bright.process(c.inHp.hp(b[i]) * preGain_);
            for (int st = 0; st < v.stages; ++st) {
                if (st == v.toneStackAfter) s = c.ts.process(s) * tsMakeup_;
                float bias = v.stageBias[st] - 0.25f * c.dyn[st].env;
                float g = v.stageGain[st];
                float drive = g * s;
                c.dyn[st].process(std::max(0.0f, drive - 0.5f));
                float y = fastTanh(drive + bias) - fastTanh(bias);
                y = c.hp[st].hp(c.lp[st].lp(y));
                s = -y;  // each triode stage inverts the phase
            }
            if (v.toneStackAfter >= v.stages) s = c.ts.process(s) * tsMakeup_;

            // Power amp: sag lowers the drive when the output works hard.
            float sagMul = 1.0f / (1.0f + v.sag * 2.0f * c.sagEnv.env);
            float p = s * masterGain_ * sagMul;
            float out = v.solidState ? softClipK(p, 6.0f)
                                     : (fastTanh(p + v.powerAsym) - fastTanh(v.powerAsym));
            c.sagEnv.process(out);
            b[i] = out;
        }
        c.os.down(x, n);
        for (int i = 0; i < n; ++i) x[i] = c.dc.process(c.depth.process(c.presence.process(x[i]))) * outGain_;
    }

    Ch ch_[2];
    double osr_ = 192000;
    const Voicing* voicing_ = &kVoicings[0];
    float preGain_ = 1, masterGain_ = 1, outGain_ = 1, tsMakeup_ = 1;
    float last_[kCount] = {};
};
TF_REGISTER(Amp);
