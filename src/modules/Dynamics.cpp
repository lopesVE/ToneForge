// Noise gate, compressor and volume pedal.
#include <cmath>

#include "dsp/Dsp.h"
#include "engine/Module.h"

using namespace dsp;

class NoiseGate : public Module {
public:
    enum { kThreshold, kRelease, kRange };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"gate", "Noise Gate", "Corta o ruído quando você não está tocando.", Category::Dynamics,
                            {P("threshold", "Threshold", -90, -20, -62, "dB", 0),
                             P("release", "Release", 5, 500, 90, "ms", 0, 2.0f),
                             P("range", "Range", -90, 0, -80, "dB", 0)}};
        return i;
    }
    NoiseGate() : Module(Info()) {}

protected:
    void onPrepare(double sr, int) override {
        det_.prepare(sr, 0.3f, 40.0f);
        holdSamples_ = int(0.03 * sr);
        onReset();
    }
    void onReset() override {
        det_.reset();
        gain_ = 1;
        hold_ = 0;
        open_ = true;
    }
    void process(float* L, float* R, int n, bool stereo) override {
        const float thrOpen = dbToGain(get(kThreshold));
        const float thrClose = thrOpen * 0.5f;  // 6 dB hysteresis
        const float floorGain = dbToGain(get(kRange));
        const float attCoef = float(std::exp(-1.0 / (0.001 * sr_)));
        const float relCoef = float(std::exp(-1.0 / (0.001 * get(kRelease) * sr_)));
        for (int i = 0; i < n; ++i) {
            float in = stereo ? std::max(std::fabs(L[i]), std::fabs(R[i])) : std::fabs(L[i]);
            float env = det_.process(in);
            if (env > thrOpen) { open_ = true; hold_ = holdSamples_; }
            else if (env < thrClose) { if (hold_ > 0) --hold_; else open_ = false; }
            float target = open_ ? 1.0f : floorGain;
            float c = target > gain_ ? attCoef : relCoef;
            gain_ = target + (gain_ - target) * c;
            L[i] *= gain_;
            if (stereo) R[i] *= gain_;
        }
    }

private:
    EnvelopeFollower det_;
    float gain_ = 1;
    int hold_ = 0, holdSamples_ = 0;
    bool open_ = true;
};
TF_REGISTER(NoiseGate);

class Compressor : public Module {
public:
    enum { kThreshold, kRatio, kAttack, kRelease, kMakeup, kMix };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"compressor", "Compressor", "Compressor feed-forward com joelho suave.", Category::Dynamics,
                            {P("threshold", "Threshold", -60, 0, -24, "dB", 1),
                             P("ratio", "Ratio", 1, 20, 4, ":1", 1, 2.0f),
                             P("attack", "Attack", 0.1f, 100, 8, "ms", 1, 2.5f),
                             P("release", "Release", 10, 1000, 150, "ms", 0, 2.0f),
                             P("makeup", "Makeup", 0, 30, 8, "dB", 1),
                             P("mix", "Mix", 0, 100, 100, "%", 0)}};
        return i;
    }
    Compressor() : Module(Info()) {}

protected:
    void onPrepare(double, int) override { onReset(); }
    void onReset() override { envDb_ = 0; }
    void process(float* L, float* R, int n, bool stereo) override {
        const float thr = get(kThreshold), ratio = get(kRatio), knee = 6.0f;
        const float att = float(std::exp(-1.0 / (0.001 * get(kAttack) * sr_)));
        const float rel = float(std::exp(-1.0 / (0.001 * get(kRelease) * sr_)));
        const float makeup = dbToGain(get(kMakeup));
        const float mix = get(kMix) * 0.01f;
        for (int i = 0; i < n; ++i) {
            float in = stereo ? std::max(std::fabs(L[i]), std::fabs(R[i])) : std::fabs(L[i]);
            float xDb = gainToDb(in + 1e-9f);
            // Soft-knee static curve.
            float over = xDb - thr, gr = 0;
            if (2 * over > knee) gr = over * (1 - 1 / ratio);
            else if (2 * std::fabs(over) <= knee) gr = (1 - 1 / ratio) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            float c = gr > envDb_ ? att : rel;
            envDb_ = gr + (envDb_ - gr) * c;
            float g = dbToGain(-envDb_) * makeup;
            L[i] = L[i] * (1 - mix) + L[i] * g * mix;
            if (stereo) R[i] = R[i] * (1 - mix) + R[i] * g * mix;
        }
        reduction.store(envDb_, std::memory_order_relaxed);
    }

public:
    std::atomic<float> reduction{0};

private:
    float envDb_ = 0;
};
TF_REGISTER(Compressor);

class VolumePedal : public Module {
public:
    enum { kVolume, kTaper };
    static const ModuleInfo& Info() {
        static ModuleInfo i{"volume", "Volume Pedal", "Pedal de volume (bom para swells via MIDI).", Category::Utility,
                            {P("volume", "Volume", 0, 100, 100, "%", 0), Choice("taper", "Curva", {"Log", "Linear"}, 0)}};
        return i;
    }
    VolumePedal() : Module(Info()) {}

protected:
    void onPrepare(double sr, int) override { sm_.prepare(sr, 15, target()); }
    float target() const {
        float v = get(kVolume) * 0.01f;
        return getInt(kTaper) == 0 ? v * v * v : v;
    }
    void process(float* L, float* R, int n, bool stereo) override {
        sm_.setTarget(target());
        for (int i = 0; i < n; ++i) {
            float g = sm_.next();
            L[i] *= g;
            if (stereo) R[i] *= g;
        }
    }

private:
    Smoother sm_;
};
TF_REGISTER(VolumePedal);
