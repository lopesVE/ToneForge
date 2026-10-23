#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "dsp/Convolver.h"
#include "dsp/Dsp.h"
#include "engine/Module.h"

class Cab : public Module {
public:
    enum { kModel, kMic, kPosition, kDistance, kRoom, kLowCut, kHighCut, kLevel, kCount };
    static constexpr double kMaxIrSeconds = 0.5;
    static const ModuleInfo& Info();
    Cab();
    ~Cab() override;

    // Loads an impulse response file (UI thread). Selects it on success.
    bool loadIr(const std::string& pathUtf8, std::string& error);
    const std::string& irName() const { return irName_; }
    bool isIrModel() const { return getInt(kModel) == numBuiltinModels(); }
    static int numBuiltinModels() { return int(Info().params[kModel].choices.size()) - 1; }

    std::string extraState() const override { return irPath_; }
    void setExtraState(const std::string& s) override;
    void idle() override;

protected:
    bool producesStereo() const override { return true; }
    void onPrepare(double sr, int maxBlock) override;
    void onReset() override;
    void process(float* L, float* R, int n, bool stereo) override;

private:
    void updateFilters();

    dsp::Biquad f_[2][12];
    dsp::Convolver conv_[2];
    dsp::DelayLine early_[2], room_;
    dsp::OnePole roomLp_[2];
    float earlyDelay_ = 0, earlyMix_ = 0;
    float last_[kCount] = {};

    // IR hand-off between the UI thread (owner) and the audio thread (user).
    std::unique_ptr<dsp::ImpulseResponse> irActive_, irPending_;
    std::atomic<dsp::ImpulseResponse*> irNext_{nullptr};
    std::atomic<bool> irSwapped_{false};
    dsp::ImpulseResponse* irCur_ = nullptr;
    std::string irPath_, irName_;
    double irSampleRate_ = 0;
};
