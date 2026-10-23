#include "engine/Rig.h"

#include <cmath>
#include <cstring>

#include "dsp/Dsp.h"
#include "engine/Tempo.h"

std::atomic<float> g_tempoBpm{120.0f};

const char* sectionName(Section s) {
    switch (s) {
        case Section::Stomp: return "Pedais";
        case Section::Amp: return "Amp";
        case Section::Cab: return "Gabinete";
        case Section::Rack: return "Rack";
    }
    return "";
}

Module* Chain::get(Section s, int index) {
    if (auto* l = list(s)) return index >= 0 && index < int(l->size()) ? (*l)[size_t(index)].get() : nullptr;
    return slot(s)->get();
}

Rig::Rig() {
    gate_ = createModule("gate");
    tuner_.assign(kTunerSize, 0.0f);
}

Rig::~Rig() = default;

void Rig::prepare(double sampleRate) {
    std::lock_guard<std::mutex> lk(mtx_);
    sr_ = sampleRate;
    for (auto* v : {&mono_, &aL_, &aR_, &bL_, &bR_}) v->assign(kSubBlock, 0.0f);
    rec_.assign(kSubBlock * 2, 0.0f);
    if (gate_) prepareModule(*gate_);
    for (auto& c : chains_) c.forEach([&](Module& m) { prepareModule(m); });
    looper.prepare(sampleRate, 60.0);
    recordTap.allocate(size_t(sampleRate) * 2 * 4);  // 4 s of stereo headroom
    inGain_ = dsp::dbToGain(inputDb.load());
    masterGain_ = dsp::dbToGain(masterDb.load());
}

void Rig::runChain(Chain& c, const float* in, float* L, float* R, int n) {
    std::memcpy(L, in, sizeof(float) * size_t(n));
    bool stereo = false;
    for (auto& m : c.stomps) m->run(L, R, n, stereo);
    if (c.amp) c.amp->run(L, R, n, stereo);
    if (c.cab) c.cab->run(L, R, n, stereo);
    for (auto& m : c.rack) m->run(L, R, n, stereo);
    if (!stereo) std::memcpy(R, L, sizeof(float) * size_t(n));
    // Level and balance.
    const float g = dsp::dbToGain(c.levelDb.load(std::memory_order_relaxed));
    const float pan = c.pan.load(std::memory_order_relaxed);
    const float gl = g * std::min(1.0f, 1.0f - pan), gr = g * std::min(1.0f, 1.0f + pan);
    for (int i = 0; i < n; ++i) { L[i] *= gl; R[i] *= gr; }
}

void Rig::process(const float* in, float* outL, float* outR, int n) {
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) {  // structure is being swapped: output one silent block
        std::memset(outL, 0, sizeof(float) * size_t(n));
        std::memset(outR, 0, sizeof(float) * size_t(n));
        return;
    }
    const bool isDual = dual.load(std::memory_order_relaxed);
    for (int off = 0; off < n; off += kSubBlock) {
        const int m = std::min(kSubBlock, n - off);
        float* L = outL + off;
        float* R = outR + off;

        // Input gain (ramped), peak meter and tuner tap.
        const float inTarget = dsp::dbToGain(inputDb.load(std::memory_order_relaxed));
        const float inStep = (inTarget - inGain_) / float(m);
        float peak = 0;
        int tp = tunerPos_.load(std::memory_order_relaxed);
        for (int i = 0; i < m; ++i) {
            inGain_ += inStep;
            float x = in[off + i] * inGain_;
            mono_[size_t(i)] = x;
            peak = std::max(peak, std::fabs(x));
            tuner_[size_t((tp + i) & (kTunerSize - 1))] = x;
        }
        tunerPos_.store(tp + m, std::memory_order_release);
        inPeak.store(std::max(inPeak.load(std::memory_order_relaxed), peak), std::memory_order_relaxed);

        bool st = false;
        if (gate_) gate_->run(mono_.data(), bL_.data(), m, st);

        runChain(chains_[0], mono_.data(), aL_.data(), aR_.data(), m);
        if (isDual) {
            runChain(chains_[1], mono_.data(), bL_.data(), bR_.data(), m);
            for (int i = 0; i < m; ++i) { aL_[size_t(i)] += bL_[size_t(i)]; aR_[size_t(i)] += bR_[size_t(i)]; }
        }

        const float mTarget = dsp::dbToGain(masterDb.load(std::memory_order_relaxed));
        const float mStep = (mTarget - masterGain_) / float(m);
        for (int i = 0; i < m; ++i) {
            masterGain_ += mStep;
            L[i] = aL_[size_t(i)] * masterGain_;
            R[i] = aR_[size_t(i)] * masterGain_;
        }

        looper.process(L, R, m);

        // Safety: catch NaN/Inf from any module and keep the output in range.
        float pl = 0, pr = 0;
        bool clip = false;
        for (int i = 0; i < m; ++i) {
            if (!std::isfinite(L[i])) L[i] = 0;
            if (!std::isfinite(R[i])) R[i] = 0;
            if (std::fabs(L[i]) > 1 || std::fabs(R[i]) > 1) clip = true;
            L[i] = std::clamp(L[i], -1.0f, 1.0f);
            R[i] = std::clamp(R[i], -1.0f, 1.0f);
            pl = std::max(pl, std::fabs(L[i]));
            pr = std::max(pr, std::fabs(R[i]));
        }
        if (clip) clipped.store(true, std::memory_order_relaxed);
        outPeakL.store(std::max(outPeakL.load(std::memory_order_relaxed), pl), std::memory_order_relaxed);
        outPeakR.store(std::max(outPeakR.load(std::memory_order_relaxed), pr), std::memory_order_relaxed);

        if (recording.load(std::memory_order_relaxed)) {
            for (int i = 0; i < m; ++i) { rec_[size_t(2 * i)] = L[i]; rec_[size_t(2 * i + 1)] = R[i]; }
            recordTap.push(rec_.data(), size_t(2 * m));
        }
        if (muted.load(std::memory_order_relaxed)) {
            std::memset(L, 0, sizeof(float) * size_t(m));
            std::memset(R, 0, sizeof(float) * size_t(m));
        }
    }
}

void Rig::copyTunerInput(float* dst, int n) const {
    const int end = tunerPos_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) dst[i] = tuner_[size_t((end - n + i) & (kTunerSize - 1))];
}

bool Rig::insert(int ci, Section s, int index, ModulePtr m) {
    auto* l = chains_[size_t(ci)].list(s);
    if (!l || !m) return false;
    if (int(l->size()) >= (s == Section::Stomp ? kMaxStomps : kMaxRack)) return false;
    prepareModule(*m);
    std::lock_guard<std::mutex> lk(mtx_);
    index = std::clamp(index, 0, int(l->size()));
    l->insert(l->begin() + index, std::move(m));
    return true;
}

void Rig::remove(int ci, Section s, int index) {
    ModulePtr dead;
    if (auto* l = chains_[size_t(ci)].list(s)) {
        if (index < 0 || index >= int(l->size())) return;
        std::lock_guard<std::mutex> lk(mtx_);
        dead = std::move((*l)[size_t(index)]);
        l->erase(l->begin() + index);
    } else if (auto* slot = chains_[size_t(ci)].slot(s)) {
        std::lock_guard<std::mutex> lk(mtx_);
        dead = std::move(*slot);
    }
}  // `dead` is destroyed here, outside the lock

void Rig::move(int ci, Section s, int from, int to) {
    auto* l = chains_[size_t(ci)].list(s);
    if (!l || from == to || from < 0 || to < 0 || from >= int(l->size()) || to >= int(l->size())) return;
    std::lock_guard<std::mutex> lk(mtx_);
    ModulePtr m = std::move((*l)[size_t(from)]);
    l->erase(l->begin() + from);
    l->insert(l->begin() + to, std::move(m));
}

void Rig::setSlot(int ci, Section s, ModulePtr m) {
    auto* slot = chains_[size_t(ci)].slot(s);
    if (!slot) return;
    if (m) prepareModule(*m);
    ModulePtr dead;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        dead = std::move(*slot);
        *slot = std::move(m);
    }
}

static ModulePtr cloneModule(const Module& src) {
    ModulePtr m = createModule(src.info().typeId);
    if (!m) return nullptr;
    for (int i = 0; i < src.numParams(); ++i) m->set(i, src.get(i));
    m->bypassed.store(src.bypassed.load());
    return m;
}

void Rig::copyChain(int from, int to) {
    if (from == to) return;
    Chain& src = chains_[size_t(from)];
    auto cloneInto = [&](const std::vector<ModulePtr>& in, std::vector<ModulePtr>& out) {
        for (auto& m : in)
            if (auto c = cloneModule(*m)) out.push_back(std::move(c));
    };
    std::vector<ModulePtr> stomps, rack;
    cloneInto(src.stomps, stomps);
    cloneInto(src.rack, rack);
    ModulePtr amp = src.amp ? cloneModule(*src.amp) : nullptr;
    ModulePtr cab = src.cab ? cloneModule(*src.cab) : nullptr;
    for (auto& m : stomps) prepareModule(*m);
    for (auto& m : rack) prepareModule(*m);
    if (amp) prepareModule(*amp);
    if (cab) {
        prepareModule(*cab);
        cab->setExtraState(src.cab->extraState());
    }
    Chain& dst = chains_[size_t(to)];
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::swap(dst.stomps, stomps);
        std::swap(dst.rack, rack);
        std::swap(dst.amp, amp);
        std::swap(dst.cab, cab);
    }
}

void Rig::loadState(RigState&& st) {
    auto prep = [&](ModulePtr& m) { if (m) prepareModule(*m); };
    prep(st.gate);
    for (int c = 0; c < 2; ++c) {
        for (auto& m : st.stomps[size_t(c)]) prep(m);
        for (auto& m : st.rack[size_t(c)]) prep(m);
        prep(st.amp[size_t(c)]);
        prep(st.cab[size_t(c)]);
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (st.gate) std::swap(gate_, st.gate);
        for (int c = 0; c < 2; ++c) {
            Chain& ch = chains_[size_t(c)];
            std::swap(ch.stomps, st.stomps[size_t(c)]);
            std::swap(ch.rack, st.rack[size_t(c)]);
            std::swap(ch.amp, st.amp[size_t(c)]);
            std::swap(ch.cab, st.cab[size_t(c)]);
            ch.levelDb.store(st.levelDb[size_t(c)]);
            ch.pan.store(st.pan[size_t(c)]);
        }
        dual.store(st.dual);
        inputDb.store(st.inputDb);
        masterDb.store(st.masterDb);
        g_tempoBpm.store(st.bpm);
    }
}  // old modules (now in `st`) are destroyed by the caller, outside the lock

void Rig::idle() {
    if (gate_) gate_->idle();
    for (auto& c : chains_) c.forEach([](Module& m) { m.idle(); });
}
