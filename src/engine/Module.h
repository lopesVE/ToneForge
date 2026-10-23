// Base class for every block in the signal chain (pedals, amps, cabs, rack units).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class Category { Dynamics, Drive, Filter, Modulation, Delay, Reverb, Pitch, EQ, Utility, Amp, Cab };
const char* categoryName(Category c);

struct ParamDesc {
    std::string id, name;
    float min = 0, max = 1, def = 0;
    std::string unit;
    int decimals = 1;
    float skew = 1.0f;                 // >1 gives the knob more travel near min
    std::vector<std::string> choices;  // non-empty: value is a choice index
    bool toggle = false;

    bool isChoice() const { return !choices.empty(); }
    float toNorm(float v) const;
    float fromNorm(float n) const;
    std::string format(float v) const;
};

ParamDesc P(const char* id, const char* name, float mn, float mx, float def, const char* unit = "", int decimals = 1,
            float skew = 1.0f);
ParamDesc Choice(const char* id, const char* name, std::vector<std::string> choices, int def = 0);
ParamDesc Toggle(const char* id, const char* name, bool def = false);

struct ModuleInfo {
    std::string typeId, name, description;
    Category category = Category::Utility;
    std::vector<ParamDesc> params;
    int paramIndex(const std::string& id) const;
};

class Module {
public:
    explicit Module(const ModuleInfo& info);
    virtual ~Module() = default;

    const ModuleInfo& info() const { return info_; }
    int numParams() const { return int(info_.params.size()); }
    const ParamDesc& desc(int i) const { return info_.params[size_t(i)]; }
    float get(int i) const { return values_[size_t(i)].load(std::memory_order_relaxed); }
    void set(int i, float v);
    float getNorm(int i) const { return desc(i).toNorm(get(i)); }
    void setNorm(int i, float n) { set(i, desc(i).fromNorm(n)); }
    int getInt(int i) const { return int(get(i) + 0.5f); }
    bool getBool(int i) const { return get(i) > 0.5f; }

    std::atomic<bool> bypassed{false};
    const uint32_t uid;

    // Extra string state such as a loaded IR path. UI thread only.
    virtual std::string extraState() const { return {}; }
    virtual void setExtraState(const std::string&) {}
    // Called from the UI thread once per frame for deferred cleanup.
    virtual void idle() {}

    void prepare(double sampleRate, int maxBlock);
    // Process one block in place. `stereo` tells whether R carries a distinct
    // signal; modules that produce stereo set it to true.
    void run(float* L, float* R, int n, bool& stereo);

    // Live level after this module, for the UI meters.
    std::atomic<float> meter{0.0f};

protected:
    virtual void onPrepare(double sampleRate, int maxBlock) = 0;
    virtual void onReset() {}
    // Process n samples. If stereo is false only L needs processing.
    virtual void process(float* L, float* R, int n, bool stereo) = 0;
    virtual bool producesStereo() const { return false; }

    double sr_ = 48000;
    int maxBlock_ = 64;

private:
    const ModuleInfo& info_;
    std::unique_ptr<std::atomic<float>[]> values_;
    float wet_ = 1.0f;
    std::vector<float> dryL_, dryR_;
};

using ModulePtr = std::unique_ptr<Module>;

// ---- Registry ---------------------------------------------------------------
struct ModuleType {
    const ModuleInfo* info;
    std::function<ModulePtr()> create;
};
const std::vector<ModuleType>& moduleTypes();
ModulePtr createModule(const std::string& typeId);
const ModuleInfo* findModuleInfo(const std::string& typeId);

struct Registrar {
    Registrar(const ModuleInfo& info, std::function<ModulePtr()> create);
};

#define TF_REGISTER(Class) \
    static Registrar reg_##Class{Class::Info(), [] { return ModulePtr(new Class()); }}
