// Speaker cabinet: built-in filter models with microphone type, position and
// distance, a stereo room, or a user-supplied impulse response (WAV/AIFF/FLAC).
#include "modules/Cab.h"

#include <chrono>
#include <cmath>
#include <thread>

#include "miniaudio.h"
#include "util/Paths.h"

using namespace dsp;

namespace {

struct Peak { float f, q, db; };
struct CabModel {
    const char* name;
    float hpF, hpQ;
    Peak peaks[4];
    float lpF, lpQ, lp2F;
};

const CabModel kCabs[] = {
    {"1x12 Open Back US", 100, 1.1f, {{115, 1.5f, 3}, {1200, 1.0f, -2}, {2500, 1.5f, 4}, {4000, 2.0f, 2}}, 5500, 0.8f, 7500},
    {"2x12 Blue Alnico", 85, 1.0f, {{140, 1.2f, 2}, {1800, 1.2f, 3}, {3200, 2.0f, 5}, {700, 1.0f, -1.5f}}, 5800, 0.9f, 8000},
    {"4x12 Greenback", 75, 1.3f, {{100, 1.5f, 4}, {1500, 1.0f, 3}, {2800, 2.0f, 2}, {400, 1.2f, -1.5f}}, 4500, 0.9f, 6500},
    {"4x12 Modern V30", 70, 1.4f, {{90, 1.5f, 4}, {600, 1.2f, -3}, {2400, 1.5f, 5}, {4500, 2.0f, 3}}, 5200, 1.0f, 7000},
    {"1x15 Bass", 45, 1.0f, {{80, 1.0f, 3}, {800, 1.0f, -2}, {2500, 1.5f, 2}, {300, 1.0f, 0}}, 3500, 0.8f, 5000},
    {"8x10 Bass", 55, 1.2f, {{100, 1.2f, 4}, {500, 1.0f, -2}, {2000, 1.5f, 3}, {300, 1.0f, 0}}, 4500, 0.8f, 6500},
};
constexpr int kNumCabs = int(sizeof(kCabs) / sizeof(kCabs[0]));
constexpr int kIrModel = kNumCabs;  // last choice: user IR

std::vector<std::string> cabNames() {
    std::vector<std::string> v;
    for (auto& c : kCabs) v.push_back(c.name);
    v.push_back("IR do usuário (arquivo)");
    return v;
}

// Early-reflection taps (ms, gain) for the stereo room.
const float kRoomTapsL[][2] = {{4.1f, 0.62f}, {9.7f, 0.48f}, {15.3f, 0.38f}, {22.9f, 0.3f}, {31.7f, 0.22f}, {43.1f, 0.15f}};
const float kRoomTapsR[][2] = {{5.3f, 0.6f}, {11.2f, 0.47f}, {17.8f, 0.37f}, {25.4f, 0.28f}, {35.3f, 0.21f}, {47.9f, 0.14f}};

}  // namespace

const ModuleInfo& Cab::Info() {
    static ModuleInfo i{"cab", "Gabinete + Mic", "Alto-falante, microfone e sala. Também carrega IRs (.wav) próprias.",
                        Category::Cab,
                        {Choice("model", "Gabinete", cabNames(), 2),
                         Choice("mic", "Microfone", {"Dinâmico", "Ribbon", "Condensador"}, 0),
                         P("position", "Posição", 0, 100, 30, "%", 0), P("distance", "Distância", 0, 100, 15, "%", 0),
                         P("room", "Sala", 0, 100, 10, "%", 0), P("lowcut", "Low Cut", 20, 300, 60, "Hz", 0, 2.0f),
                         P("highcut", "High Cut", 3000, 20000, 12000, "Hz", 0, 2.0f), P("level", "Level", -24, 12, 0, "dB")}};
    return i;
}

Cab::Cab() : Module(Info()) {}

Cab::~Cab() = default;

void Cab::onPrepare(double sr, int) {
    const int maxParts = int(std::ceil(kMaxIrSeconds * sr / ImpulseResponse::kBlock));
    for (auto& c : conv_) c.prepare(maxParts);
    for (auto& d : early_) d.prepare(int(0.01 * sr));
    room_.prepare(int(0.06 * sr));
    for (auto& r : roomLp_) r.setLowpass(sr, 5000);
    for (float& v : last_) v = -1e9f;
    // Sample rate changes invalidate a loaded IR: rebuild it from its file.
    if (!irPath_.empty() && irSampleRate_ != sr) {
        std::string err;
        loadIr(irPath_, err);
    }
    if (ImpulseResponse* p = irNext_.exchange(nullptr)) {
        irCur_ = p;
        irSwapped_.store(true);
    }
    onReset();
}

void Cab::onReset() {
    for (int c = 0; c < 2; ++c) {
        for (auto& b : f_[c]) b.reset();
        conv_[c].reset();
        early_[c].reset();
        roomLp_[c].reset();
    }
    room_.reset();
}

void Cab::updateFilters() {
    bool changed = false;
    for (int p = 0; p < kCount; ++p) changed |= get(p) != last_[p];
    if (!changed) return;
    for (int p = 0; p < kCount; ++p) last_[p] = get(p);
    const int model = getInt(kModel);
    const CabModel& cm = kCabs[model >= kNumCabs ? 2 : model];
    const int mic = getInt(kMic);
    const float pos = get(kPosition) * 0.01f, dist = get(kDistance) * 0.01f;
    for (int c = 0; c < 2; ++c) {
        Biquad* f = f_[c];
        f[0].highpass(sr_, cm.hpF, cm.hpQ);
        for (int k = 0; k < 4; ++k) f[1 + k].peak(sr_, cm.peaks[k].f, cm.peaks[k].q, cm.peaks[k].db);
        f[5].lowpass(sr_, cm.lpF * lerp(1.25f, 0.75f, pos), cm.lpQ);
        f[6].lowpass(sr_, cm.lp2F, 0.7);
        // Microphone colour.
        if (mic == 0) f[7].peak(sr_, 5000, 1.5, 4);
        else if (mic == 1) f[7].highShelf(sr_, 4500, -6);
        else f[7].highShelf(sr_, 8000, 3);
        // Cap (bright) to edge (dark).
        f[8].highShelf(sr_, 2500, lerp(4, -8, pos));
        // Proximity effect for directional mics.
        float prox = mic == 2 ? 2.5f : (mic == 1 ? 7.0f : 6.0f);
        f[9].lowShelf(sr_, 180, prox * (1 - dist) - 2 * dist);
        f[10].highpass(sr_, get(kLowCut), 0.7071);
        if (get(kHighCut) >= 19999) f[11].identity(); else f[11].lowpass(sr_, get(kHighCut), 0.7071);
    }
    earlyDelay_ = lerp(0.3f, 3.0f, dist) * 0.001f * float(sr_);
    earlyMix_ = lerp(0.0f, 0.35f, dist);
}

void Cab::process(float* L, float* R, int n, bool stereoIn) {
    if (ImpulseResponse* p = irNext_.exchange(nullptr)) {
        irCur_ = p;
        conv_[0].reset();
        conv_[1].reset();
        irSwapped_.store(true);
    }
    updateFilters();
    const bool useIr = getInt(kModel) == kIrModel && irCur_ != nullptr;
    const float level = dbToGain(get(kLevel));
    for (int c = 0; c < (stereoIn ? 2 : 1); ++c) {
        float* x = c ? R : L;
        Biquad* f = f_[c];
        if (useIr) {
            conv_[c].process(*irCur_, x, n);
            for (int i = 0; i < n; ++i) x[i] = f[11].process(f[10].process(x[i]));
        } else {
            for (int i = 0; i < n; ++i) {
                float v = x[i];
                for (int k = 0; k < 12; ++k) v = f[k].process(v);
                early_[c].push(v);
                x[i] = (v + early_[c].read(std::max(2.0f, earlyDelay_)) * earlyMix_) * 0.9f;
            }
        }
    }
    if (!stereoIn) std::memcpy(R, L, sizeof(float) * size_t(n));

    // Stereo room: early reflections of the mid signal, different on each side.
    const float roomMix = get(kRoom) * 0.01f;
    const float ms = 0.001f * float(sr_);
    for (int i = 0; i < n; ++i) {
        float mid = 0.5f * (L[i] + R[i]);
        room_.push(mid);
        if (roomMix > 0.0f) {
            float rl = 0, rr = 0;
            for (auto& t : kRoomTapsL) rl += room_.read(t[0] * ms) * t[1];
            for (auto& t : kRoomTapsR) rr += room_.read(t[0] * ms) * t[1];
            L[i] += roomLp_[0].lp(rl) * roomMix;
            R[i] += roomLp_[1].lp(rr) * roomMix;
        }
        L[i] *= level;
        R[i] *= level;
    }
}

void Cab::idle() {
    if (irSwapped_.exchange(false) && irPending_) irActive_ = std::move(irPending_);
}

bool Cab::loadIr(const std::string& pathUtf8, std::string& error) {
    // Settle any hand-off that is still in flight before replacing it.
    if (irPending_) {
        ImpulseResponse* expected = irPending_.get();
        if (irNext_.compare_exchange_strong(expected, nullptr)) {
            irPending_.reset();  // audio thread never saw it
        } else {
            while (!irSwapped_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            irSwapped_.store(false);
            irActive_ = std::move(irPending_);
        }
    }

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, ma_uint32(sr_));
    ma_decoder dec;
    std::wstring w = utf8ToWide(pathUtf8);
    if (ma_decoder_init_file_w(w.c_str(), &cfg, &dec) != MA_SUCCESS) {
        error = "Não consegui abrir o arquivo de IR (use WAV, AIFF, FLAC ou MP3).";
        return false;
    }
    std::vector<float> samples;
    const size_t maxFrames = size_t(kMaxIrSeconds * sr_);
    float buf[4096];
    while (samples.size() < maxFrames) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf, 4096, &got) != MA_SUCCESS || got == 0) break;
        samples.insert(samples.end(), buf, buf + got);
    }
    ma_decoder_uninit(&dec);
    if (samples.size() < 16) {
        error = "O arquivo de IR está vazio ou é curto demais.";
        return false;
    }
    auto ir = ImpulseResponse::build(samples, int(maxFrames));
    size_t slash = pathUtf8.find_last_of("\\/");
    ir->name = slash == std::string::npos ? pathUtf8 : pathUtf8.substr(slash + 1);
    irPath_ = pathUtf8;
    irName_ = ir->name;
    irSampleRate_ = sr_;
    irPending_ = std::move(ir);
    irNext_.store(irPending_.get());
    return true;
}

void Cab::setExtraState(const std::string& s) {
    if (s.empty()) return;
    std::string err;
    loadIr(s, err);
}

TF_REGISTER(Cab);
