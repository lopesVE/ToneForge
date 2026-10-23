// Offline self-test: renders every factory preset and every module with a
// synthetic plucked-guitar signal, checks for NaN/clipping/levels, measures
// CPU cost, and exercises the tuner, looper and preset round trip.
// Usage: tf_selftest [render-dir]
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "audio/AudioEngine.h"
#include "dsp/Dsp.h"
#include "engine/Preset.h"
#include "engine/Rig.h"
#include "engine/Tuner.h"
#include "miniaudio.h"
#include "util/Paths.h"

namespace {

constexpr double kSr = 48000;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FALHOU: %s\n", what.c_str());
    }
}

// Karplus-Strong plucks: an E major strum, a pause, then a single-note riff.
std::vector<float> guitarSignal(double seconds) {
    const int n = int(seconds * kSr);
    std::vector<float> out(size_t(n), 0.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uni(-1, 1);
    auto pluck = [&](double start, double freq, float amp) {
        int period = int(kSr / freq);
        std::vector<float> line(static_cast<size_t>(period));
        for (auto& v : line) v = uni(rng);
        int s0 = int(start * kSr);
        float prev = 0;
        for (int i = 0; s0 + i < n && i < int(3.0 * kSr); ++i) {
            size_t k = size_t(i % period);
            float cur = line[k];
            line[k] = 0.996f * 0.5f * (cur + prev);
            prev = cur;
            out[size_t(s0 + i)] += cur * amp;
        }
    };
    const double chord[] = {82.41, 123.47, 164.81, 207.65, 246.94, 329.63};
    for (int strum = 0; strum < 2; ++strum)
        for (int s = 0; s < 6; ++s) pluck(strum * 1.6 + s * 0.012, chord[s], 0.11f);
    const double riff[] = {82.41, 98.0, 110.0, 123.47, 146.83, 164.81, 196.0, 220.0};
    for (int i = 0; i < 8; ++i) pluck(3.3 + i * 0.16, riff[i], 0.22f);
    return out;
}

struct Stats {
    float peak = 0, rmsDb = -120;
    int nonFinite = 0;
    double realtimeFactor = 0;
};

Stats render(Rig& rig, const std::vector<float>& in, std::vector<float>* interleaved) {
    Stats s;
    const int block = 256;
    std::vector<float> L(block), R(block);
    double energy = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t off = 0; off < in.size(); off += block) {
        int n = int(std::min<size_t>(block, in.size() - off));
        rig.process(in.data() + off, L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) ++s.nonFinite;
            s.peak = std::max({s.peak, std::fabs(L[i]), std::fabs(R[i])});
            energy += 0.5 * (double(L[i]) * L[i] + double(R[i]) * R[i]);
            if (interleaved) { interleaved->push_back(L[i]); interleaved->push_back(R[i]); }
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    s.realtimeFactor = (double(in.size()) / kSr) / std::max(secs, 1e-9);
    s.rmsDb = 10.0f * float(std::log10(energy / double(in.size()) + 1e-12));
    return s;
}

void writeWav(const std::string& path, const std::vector<float>& interleaved) {
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, ma_uint32(kSr));
    ma_encoder enc;
    if (ma_encoder_init_file_w(utf8ToWide(path).c_str(), &cfg, &enc) != MA_SUCCESS) return;
    ma_encoder_write_pcm_frames(&enc, interleaved.data(), interleaved.size() / 2, nullptr);
    ma_encoder_uninit(&enc);
}

void clearRig(Rig& rig) {
    RigState st;
    st.gate = createModule("gate");
    st.gate->bypassed.store(true);
    rig.loadState(std::move(st));
}

}  // namespace

// Opens the default duplex device with the output muted, runs 2 s, reports.
int audioSmoke() {
    Rig rig;
    rig.muted.store(true);
    AudioEngine ae;
    ae.refreshDevices(0);
    std::printf("Entradas WASAPI: %zu, saídas: %zu\n", ae.inputDevices().size(), ae.outputDevices().size());
    AudioConfig cfg;
    cfg.bufferSize = 256;
    if (!ae.start(cfg, rig)) {
        std::printf("FALHOU ao abrir: %s\n", ae.status().error.c_str());
        return 1;
    }
    const AudioStatus& s = ae.status();
    std::printf("Aberto: %s -> %s, %d Hz, %d amostras, latência ~%.1f ms\n", s.inputName.c_str(), s.outputName.c_str(),
                s.sampleRate, s.bufferSize, s.latencyMs);
    Sleep(2000);
    float cpu = rig.cpuLoad.load();
    std::printf("Callbacks rodando: %s (CPU do áudio %.1f%%)\n", cpu > 0 ? "sim" : "NÃO", cpu * 100);
    ae.stop();
    return cpu > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 1 && std::string(argv[1]) == "--audio-smoke") return audioSmoke();
    std::string renderDir = argc > 1 ? argv[1] : "";
    const std::vector<float> guitar = guitarSignal(5.0);

    Rig rig;
    rig.prepare(kSr);

    float inPeak = 0;
    for (float v : guitar) inPeak = std::max(inPeak, std::fabs(v));
    std::printf("Sinal de teste: %.1f s, pico %.1f dBFS\n\n", guitar.size() / kSr, dsp::gainToDb(inPeak));

    // ---- Factory presets ----
    std::printf("== Presets de fábrica ==\n");
    std::printf("%-18s %8s %8s %10s\n", "preset", "pico dB", "RMS dB", "x tempo real");
    for (auto& pj : factoryPresets()) {
        PresetData pd;
        check(presetFromJson(pj, pd), "parse " + pj.getStr("name"));
        std::string name = pd.name;
        applyPreset(rig, std::move(pd));
        std::vector<float> wav;
        Stats s = render(rig, guitar, renderDir.empty() ? nullptr : &wav);
        std::printf("%-18s %8.1f %8.1f %10.0f\n", name.c_str(), dsp::gainToDb(s.peak), s.rmsDb, s.realtimeFactor);
        check(s.nonFinite == 0, name + ": amostras NaN/Inf");
        check(s.rmsDb > -26 && s.rmsDb < -12, name + ": nível RMS fora da faixa (-26..-12 dB)");
        check(s.peak < 0.95f, name + ": saída encostando em 0 dBFS");
        check(s.realtimeFactor > 10, name + ": CPU alta demais");
        if (!renderDir.empty()) writeWav(renderDir + "\\" + name + ".wav", wav);
    }

    // ---- Amp calibration: each model at default knobs into the default cab ----
    std::printf("\n== Calibração dos amps (alvo: RMS perto de -18 dB) ==\n");
    {
        const ModuleInfo* ai = findModuleInfo("amp");
        const int models = int(ai->params[0].choices.size());
        for (int model = 0; model < models; ++model) {
            clearRig(rig);
            ModulePtr a = createModule("amp");
            a->set(0, float(model));
            rig.setSlot(0, Section::Amp, std::move(a));
            rig.setSlot(0, Section::Cab, createModule("cab"));
            Stats s = render(rig, guitar, nullptr);
            std::printf("  %-26s pico %6.1f dB  RMS %6.1f dB\n", ai->params[0].choices[size_t(model)].c_str(),
                        dsp::gainToDb(s.peak), s.rmsDb);
            check(s.rmsDb > -22 && s.rmsDb < -14, "amp " + ai->params[0].choices[size_t(model)] + " fora do nível alvo");
        }
    }

    // ---- Every module alone, bypassed gate, no amp ----
    std::printf("\n== Módulos isolados ==\n");
    for (auto& t : moduleTypes()) {
        clearRig(rig);
        ModulePtr m = t.create();
        if (t.info->category == Category::Amp) rig.setSlot(0, Section::Amp, std::move(m));
        else if (t.info->category == Category::Cab) rig.setSlot(0, Section::Cab, std::move(m));
        else rig.insert(0, Section::Stomp, 0, std::move(m));
        Stats s = render(rig, guitar, nullptr);
        std::printf("  %-12s pico %6.1f dB  RMS %6.1f dB  %6.0fx\n", t.info->typeId.c_str(), dsp::gainToDb(s.peak), s.rmsDb,
                    s.realtimeFactor);
        check(s.nonFinite == 0, t.info->typeId + ": NaN/Inf");
        check(s.rmsDb > -50, t.info->typeId + ": saída silenciosa");
        // Sweep every parameter to its extremes to catch instabilities.
        Module* mod = rig.chain(0).get(t.info->category == Category::Amp ? Section::Amp
                                       : t.info->category == Category::Cab ? Section::Cab : Section::Stomp, 0);
        for (int p = 0; p < mod->numParams(); ++p) {
            float orig = mod->get(p);
            for (float v : {mod->desc(p).min, mod->desc(p).max}) {
                mod->set(p, v);
                Stats e = render(rig, std::vector<float>(guitar.begin(), guitar.begin() + 24000), nullptr);
                check(e.nonFinite == 0, t.info->typeId + "." + mod->desc(p).id + " no extremo: NaN/Inf");
            }
            mod->set(p, orig);
        }
    }

    // ---- Tuner ----
    std::printf("\n== Afinador ==\n");
    for (double f : {41.2, 82.41, 110.0, 196.0, 329.63, 659.25}) {
        std::vector<float> sine(4096);
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.3f * float(std::sin(dsp::kTwoPi * f * double(i) / kSr) + 0.4 * std::sin(dsp::kTwoPi * 2 * f * double(i) / kSr));
        PitchResult r = detectPitch(sine.data(), int(sine.size()), kSr);
        NoteInfo ni = noteFromFreq(r.freq);
        std::printf("  %7.2f Hz -> %7.2f Hz (%s%d, %+.1f cents)\n", f, r.freq, ni.name.c_str(), ni.octave, ni.cents);
        check(std::fabs(1200.0 * std::log2(r.freq / f)) < 3.0, "afinador errou " + std::to_string(f) + " Hz");
    }

    // ---- Looper ----
    std::printf("\n== Looper ==\n");
    {
        clearRig(rig);
        rig.prepare(kSr);
        std::vector<float> one(48128, 0.0f), silence(48128, 0.0f), L(256), R(256);
        for (size_t i = 0; i < one.size(); ++i) one[i] = 0.2f * float(std::sin(dsp::kTwoPi * 220 * double(i) / kSr));
        auto run = [&](const std::vector<float>& in) {
            float pk = 0;
            for (size_t off = 0; off < in.size(); off += 256) {
                rig.process(in.data() + off, L.data(), R.data(), 256);
                for (float v : L) pk = std::max(pk, std::fabs(v));
            }
            return pk;
        };
        rig.looper.command(Looper::kRecDub);
        run(one);
        rig.looper.command(Looper::kRecDub);  // close loop -> playing
        float playPeak = run(silence);
        std::printf("  loop de %.2f s, pico na reprodução %.2f\n", rig.looper.lengthSeconds(), playPeak);
        check(rig.looper.state() == Looper::State::Playing, "looper deveria estar tocando");
        check(playPeak > 0.15f, "looper não reproduziu a gravação");
        rig.looper.command(Looper::kRecDub);  // overdub
        run(one);
        rig.looper.command(Looper::kRecDub);
        float dubPeak = run(silence);
        rig.looper.command(Looper::kUndo);
        float undoPeak = run(silence);
        std::printf("  pico com overdub %.2f, após desfazer %.2f\n", dubPeak, undoPeak);
        check(dubPeak > playPeak * 1.5f, "overdub não somou");
        check(std::fabs(undoPeak - playPeak) < 0.05f, "desfazer não removeu o overdub");
        rig.looper.command(Looper::kClear);
        run(silence);
        check(rig.looper.state() == Looper::State::Empty, "limpar não esvaziou o looper");
    }

    // ---- Preset JSON round trip ----
    std::printf("\n== Ida e volta de preset (JSON) ==\n");
    {
        PresetData pd;
        presetFromJson(factoryPresets()[8], pd);  // dual amp preset
        applyPreset(rig, std::move(pd));
        std::string a = json::dump(rigToJson(rig, "x"));
        json::Value parsed;
        check(json::parse(a, parsed), "JSON gerado não é válido");
        PresetData pd2;
        check(presetFromJson(parsed, pd2), "JSON gerado não carrega");
        applyPreset(rig, std::move(pd2));
        std::string b = json::dump(rigToJson(rig, "x"));
        check(a == b, "preset mudou após ida e volta");
        std::printf("  %zu bytes, %s\n", a.size(), a == b ? "idêntico" : "DIFERENTE");
    }

    std::printf("\n%s (%d falha(s))\n", g_failures ? "RESULTADO: FALHOU" : "RESULTADO: OK", g_failures);
    return g_failures ? 1 : 0;
}
