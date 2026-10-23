// The whole signal path: input -> gate -> chain A (+ chain B in parallel)
// -> master -> looper -> output. Each chain is pedals -> amp -> cab -> rack.
#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include "engine/Looper.h"
#include "engine/Module.h"
#include "util/RingBuffer.h"

enum class Section { Stomp = 0, Amp = 1, Cab = 2, Rack = 3 };
const char* sectionName(Section s);

struct Chain {
    std::vector<ModulePtr> stomps;
    ModulePtr amp, cab;
    std::vector<ModulePtr> rack;
    std::atomic<float> levelDb{0.0f};
    std::atomic<float> pan{0.0f};  // -1 (left) .. 1 (right)

    std::vector<ModulePtr>* list(Section s) { return s == Section::Stomp ? &stomps : s == Section::Rack ? &rack : nullptr; }
    ModulePtr* slot(Section s) { return s == Section::Amp ? &amp : s == Section::Cab ? &cab : nullptr; }
    Module* get(Section s, int index);
    template <class F> void forEach(F&& f) {
        for (auto& m : stomps) f(*m);
        if (amp) f(*amp);
        if (cab) f(*cab);
        for (auto& m : rack) f(*m);
    }
};

// Everything a preset describes, built off the audio thread and swapped in.
struct RigState {
    ModulePtr gate;
    std::array<std::vector<ModulePtr>, 2> stomps, rack;
    std::array<ModulePtr, 2> amp, cab;
    std::array<float, 2> levelDb{0, 0}, pan{0, 0};
    bool dual = false;
    float inputDb = 0, masterDb = 0, bpm = 120;
};

class Rig {
public:
    static constexpr int kSubBlock = 64;
    static constexpr int kMaxStomps = 8;
    static constexpr int kMaxRack = 6;

    Rig();
    ~Rig();

    // Audio must not be running (or call under the lock).
    void prepare(double sampleRate);
    double sampleRate() const { return sr_; }

    // Audio thread. `in` is mono.
    void process(const float* in, float* outL, float* outR, int n);

    // ---- Structure edits (UI thread). Modules are prepared before insertion
    // and destroyed after the lock is released.
    bool insert(int chain, Section s, int index, ModulePtr m);
    void remove(int chain, Section s, int index);
    void move(int chain, Section s, int from, int to);
    void setSlot(int chain, Section s, ModulePtr m);  // amp / cab (nullptr = empty)
    void copyChain(int from, int to);
    void loadState(RigState&& st);  // swap in a whole preset

    // Only the UI thread changes the structure, so it may read it without the lock.
    Chain& chain(int i) { return chains_[size_t(i)]; }
    Module* gate() { return gate_.get(); }
    void idle();  // UI thread: per-frame housekeeping (IR hand-offs, meter decay)

    std::atomic<bool> dual{false};
    std::atomic<float> inputDb{0.0f}, masterDb{0.0f};
    std::atomic<bool> muted{false};  // e.g. tuner mute

    Looper looper;
    std::atomic<bool> recording{false};
    RingBuffer recordTap;  // interleaved stereo output while recording

    // Tuner input tap: latest samples, written continuously.
    static constexpr int kTunerSize = 8192;
    void copyTunerInput(float* dst, int n) const;

    // Meters (peak, decayed by idle()).
    std::atomic<float> inPeak{0}, outPeakL{0}, outPeakR{0};
    std::atomic<float> cpuLoad{0};
    std::atomic<bool> clipped{false};

    std::mutex& mutex() { return mtx_; }

private:
    void prepareModule(Module& m) { m.prepare(sr_, kSubBlock); }
    void runChain(Chain& c, const float* in, float* L, float* R, int n);

    std::mutex mtx_;
    std::array<Chain, 2> chains_;
    ModulePtr gate_;
    double sr_ = 48000;
    float inGain_ = 1, masterGain_ = 1;
    std::vector<float> mono_, aL_, aR_, bL_, bR_, rec_;
    std::vector<float> tuner_;
    std::atomic<int> tunerPos_{0};
};
