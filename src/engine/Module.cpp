#include "engine/Module.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

const char* categoryName(Category c) {
    switch (c) {
        case Category::Dynamics: return "Dinâmica";
        case Category::Drive: return "Drive / Fuzz";
        case Category::Filter: return "Filtro / Wah";
        case Category::Modulation: return "Modulação";
        case Category::Delay: return "Delay";
        case Category::Reverb: return "Reverb";
        case Category::Pitch: return "Pitch";
        case Category::EQ: return "EQ";
        case Category::Utility: return "Utilitários";
        case Category::Amp: return "Amplificador";
        case Category::Cab: return "Gabinete";
    }
    return "?";
}

float ParamDesc::toNorm(float v) const {
    if (max <= min) return 0;
    float n = std::clamp((v - min) / (max - min), 0.0f, 1.0f);
    return skew == 1.0f ? n : std::pow(n, 1.0f / skew);
}

float ParamDesc::fromNorm(float n) const {
    n = std::clamp(n, 0.0f, 1.0f);
    if (skew != 1.0f) n = std::pow(n, skew);
    return min + (max - min) * n;
}

std::string ParamDesc::format(float v) const {
    if (isChoice()) {
        int i = std::clamp(int(v + 0.5f), 0, int(choices.size()) - 1);
        return choices[size_t(i)];
    }
    if (toggle) return v > 0.5f ? "On" : "Off";
    char buf[64];
    if (unit == "ms" && v >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f s", v / 1000.0f);
    else if (unit == "Hz" && v >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.0f);
    else if (unit.empty()) std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    else std::snprintf(buf, sizeof buf, "%.*f %s", decimals, v, unit.c_str());
    return buf;
}

ParamDesc P(const char* id, const char* name, float mn, float mx, float def, const char* unit, int decimals, float skew) {
    ParamDesc d;
    d.id = id;
    d.name = name;
    d.min = mn;
    d.max = mx;
    d.def = def;
    d.unit = unit;
    d.decimals = decimals;
    d.skew = skew;
    return d;
}

ParamDesc Choice(const char* id, const char* name, std::vector<std::string> choices, int def) {
    ParamDesc d;
    d.id = id;
    d.name = name;
    d.min = 0;
    d.max = float(choices.size() - 1);
    d.def = float(def);
    d.choices = std::move(choices);
    d.decimals = 0;
    return d;
}

ParamDesc Toggle(const char* id, const char* name, bool def) {
    ParamDesc d;
    d.id = id;
    d.name = name;
    d.min = 0;
    d.max = 1;
    d.def = def ? 1.0f : 0.0f;
    d.toggle = true;
    d.decimals = 0;
    return d;
}

int ModuleInfo::paramIndex(const std::string& id) const {
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].id == id) return int(i);
    return -1;
}

static std::atomic<uint32_t> g_nextUid{1};

Module::Module(const ModuleInfo& info) : uid(g_nextUid.fetch_add(1)), info_(info) {
    values_ = std::make_unique<std::atomic<float>[]>(info.params.size());
    for (size_t i = 0; i < info.params.size(); ++i) values_[i].store(info.params[i].def);
}

void Module::set(int i, float v) {
    const ParamDesc& d = desc(i);
    v = std::clamp(v, d.min, d.max);
    if (d.isChoice() || d.toggle) v = std::round(v);
    values_[size_t(i)].store(v, std::memory_order_relaxed);
}

void Module::prepare(double sampleRate, int maxBlock) {
    sr_ = sampleRate;
    maxBlock_ = maxBlock;
    dryL_.assign(size_t(maxBlock), 0.0f);
    dryR_.assign(size_t(maxBlock), 0.0f);
    wet_ = bypassed.load() ? 0.0f : 1.0f;
    onPrepare(sampleRate, maxBlock);
}

void Module::run(float* L, float* R, int n, bool& stereo) {
    const bool active = !bypassed.load(std::memory_order_relaxed);
    if (!active && wet_ <= 0.0f) return;
    if (active && wet_ <= 0.0f) onReset();  // coming back from full bypass: drop stale state

    if (producesStereo() && !stereo) {
        std::memcpy(R, L, sizeof(float) * size_t(n));
        stereo = true;
    }
    const float target = active ? 1.0f : 0.0f;
    if (wet_ == target) {
        process(L, R, n, stereo);
    } else {
        // 15 ms crossfade between dry and processed signal to avoid clicks.
        std::memcpy(dryL_.data(), L, sizeof(float) * size_t(n));
        if (stereo) std::memcpy(dryR_.data(), R, sizeof(float) * size_t(n));
        process(L, R, n, stereo);
        const float step = 1.0f / float(0.015 * sr_);
        float w = wet_;
        for (int i = 0; i < n; ++i) {
            w = target > w ? std::min(target, w + step) : std::max(target, w - step);
            L[i] = dryL_[size_t(i)] + (L[i] - dryL_[size_t(i)]) * w;
            if (stereo) R[i] = dryR_[size_t(i)] + (R[i] - dryR_[size_t(i)]) * w;
        }
        wet_ = w;
    }
    float peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(L[i]));
    meter.store(std::max(peak, meter.load(std::memory_order_relaxed)), std::memory_order_relaxed);
}
