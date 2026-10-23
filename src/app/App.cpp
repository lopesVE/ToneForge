#include "app/App.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "dsp/Dsp.h"
#include "engine/Tempo.h"
#include "miniaudio.h"
#include "util/Paths.h"

App::App() = default;

App::~App() { shutdown(); }

double App::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

void App::notify(const std::string& msg, float seconds) {
    toast = msg;
    toastUntil = now() + seconds;
}

void App::init(bool noAudio) {
    noAudio_ = noAudio;
    firstRun = !settings.load() && !noAudio;
    presetDir_ = documentsDir("Presets");
    factory_ = factoryPresets();
    refreshPresets();

    // Start with the last preset, or the first factory one.
    int start = 0;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].name == settings.lastPreset) start = int(i);
    loadPreset(start);

    rig.prepare(double(settings.audio.sampleRate));
    if (noAudio_) return;
    audio.refreshDevices(settings.audio.backend);
    // On the first run the user picks devices first: starting on the default
    // microphone and speakers could cause feedback.
    if (!firstRun) applyAudio(settings.audio);
    if (!settings.midiDevice.empty()) openMidi(settings.midiDevice);
}

void App::shutdown() {
    stopRecording();
    audio.stop();
    midi.close();
    if (currentPreset >= 0 && currentPreset < int(presets.size())) settings.lastPreset = presets[size_t(currentPreset)].name;
    if (!noAudio_) settings.save();
}

void App::frame() {
    MidiMessage m;
    while (midi.pop(m)) applyMidi(m);
    rig.idle();

    if (tunerOpen_ && now() - lastPitchTime_ > 0.06) {
        lastPitchTime_ = now();
        static float buf[4096];
        rig.copyTunerInput(buf, 4096);
        PitchResult r = detectPitch(buf, 4096, rig.sampleRate());
        if (r.freq > 0 && r.clarity > 0.75f) {
            bool close = pitch.freq > 0 && std::fabs(std::log2(r.freq / pitch.freq)) < 0.03;
            pitch.freq = close ? pitch.freq * 0.55f + r.freq * 0.45f : r.freq;
            pitch.clarity = r.clarity;
            note = noteFromFreq(pitch.freq, settings.a4);
        } else {
            pitch.clarity *= 0.8f;
            if (pitch.clarity < 0.3f) pitch.freq = 0;
        }
    }
    if (!recRun_.load() && recThread_.joinable()) {  // worker gave up on its own
        recThread_.join();
        if (recFailed_.load()) notify("Não consegui criar o arquivo de gravação.", 5.0f);
    }
    updateDirty();
}

// ---------------------------------------------------------------- Presets ---

void App::refreshPresets() {
    std::string keep = currentPreset >= 0 && currentPreset < int(presets.size()) ? presets[size_t(currentPreset)].name : "";
    presets.clear();
    for (size_t i = 0; i < factory_.size(); ++i) {
        PresetItem it;
        it.name = factory_[i].getStr("name");
        it.factory = true;
        it.factoryIndex = int(i);
        presets.push_back(it);
    }
    for (auto& e : listPresets(presetDir_)) {
        PresetItem it;
        it.name = e.name;
        it.path = e.path;
        presets.push_back(it);
    }
    currentPreset = -1;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].name == keep) currentPreset = int(i);
}

bool App::currentIsFactory() const {
    return currentPreset >= 0 && currentPreset < int(presets.size()) && presets[size_t(currentPreset)].factory;
}

void App::loadPresetJson(const json::Value& v, int index) {
    PresetData pd;
    if (!presetFromJson(v, pd)) {
        notify("Preset inválido.");
        return;
    }
    currentName = pd.name;
    applyPreset(rig, std::move(pd));
    currentPreset = index;
    markClean();
}

void App::loadPreset(int index) {
    if (index < 0 || index >= int(presets.size())) return;
    const PresetItem& it = presets[size_t(index)];
    if (it.factory) {
        loadPresetJson(factory_[size_t(it.factoryIndex)], index);
        return;
    }
    json::Value v;
    if (!loadPresetFile(it.path, v)) {
        notify("Não consegui ler o preset " + it.name);
        return;
    }
    loadPresetJson(v, index);
}

void App::stepPreset(int dir) {
    if (presets.empty()) return;
    int n = int(presets.size());
    loadPreset(((currentPreset < 0 ? 0 : currentPreset) + dir + n) % n);
}

bool App::savePresetAs(const std::string& nameIn) {
    std::string name = nameIn.empty() ? "Meu preset" : nameIn;
    for (auto& f : factory_)
        if (f.getStr("name") == name) name += " (meu)";
    std::string path = presetFileName(presetDir_, name);
    if (!savePresetFile(path, rigToJson(rig, name))) {
        notify("Erro ao salvar em " + path);
        return false;
    }
    currentName = name;
    currentPreset = -1;
    refreshPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (!presets[i].factory && presets[i].name == name) currentPreset = int(i);
    markClean();
    notify("Preset salvo: " + name);
    return true;
}

bool App::saveCurrent() {
    if (currentPreset < 0 || currentIsFactory()) return false;
    const PresetItem& it = presets[size_t(currentPreset)];
    if (!savePresetFile(it.path, rigToJson(rig, it.name))) {
        notify("Erro ao salvar o preset.");
        return false;
    }
    markClean();
    notify("Preset salvo.");
    return true;
}

bool App::deleteCurrent() {
    if (currentPreset < 0 || currentIsFactory()) return false;
    std::string name = presets[size_t(currentPreset)].name;
    if (!DeleteFileW(utf8ToWide(presets[size_t(currentPreset)].path).c_str())) return false;
    int idx = currentPreset;
    currentPreset = -1;
    refreshPresets();
    loadPreset(std::min(idx, int(presets.size()) - 1));
    notify("Preset excluído: " + name);
    return true;
}

void App::newPreset() {
    RigState st;
    st.gate = createModule("gate");
    st.amp[0] = createModule("amp");
    st.cab[0] = createModule("cab");
    st.amp[1] = createModule("amp");
    st.cab[1] = createModule("cab");
    rig.loadState(std::move(st));
    currentName = "Novo";
    currentPreset = -1;
    markClean();
    dirty_ = true;
}

void App::markClean() {
    savedJson_ = json::dump(rigToJson(rig, currentName));
    dirty_ = false;
}

void App::updateDirty() {
    if (++dirtyCounter_ < 20) return;
    dirtyCounter_ = 0;
    if (currentPreset < 0) { dirty_ = true; return; }
    dirty_ = json::dump(rigToJson(rig, currentName)) != savedJson_;
}

// ------------------------------------------------------------------ Audio ---

void App::applyAudio(const AudioConfig& cfg) {
    settings.audio = cfg;
    if (noAudio_) return;
    if (audio.start(cfg, rig)) {
        const AudioStatus& s = audio.status();
        notify("Áudio: " + std::to_string(s.sampleRate) + " Hz, " + std::to_string(s.bufferSize) + " amostras");
    } else {
        notify(audio.status().error, 6.0f);
        // Keep the engine prepared so the UI and offline features still work.
        rig.prepare(double(cfg.sampleRate));
    }
    settings.save();
}

// ------------------------------------------------------------------- MIDI ---

void App::openMidi(const std::string& deviceName) {
    midi.close();
    settings.midiDevice.clear();
    if (deviceName.empty()) return;
    auto devs = MidiInput::devices();
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i] == deviceName) {
            if (midi.open(int(i))) {
                settings.midiDevice = deviceName;
                notify("MIDI: " + deviceName);
            } else {
                notify("Não consegui abrir o MIDI " + deviceName);
            }
            return;
        }
    }
}

void App::startLearn(const MidiTarget& t) {
    learning = true;
    learnTarget = t;
    notify("Mova um controle no seu controlador MIDI...", 10.0f);
}

void App::clearMapping(const MidiTarget& t) {
    auto& m = settings.midiMap;
    m.erase(std::remove_if(m.begin(), m.end(), [&](const MidiMapping& mm) { return mm.target == t; }), m.end());
    settings.save();
}

int App::mappedCc(const MidiTarget& t) const {
    for (auto& mm : settings.midiMap)
        if (mm.target == t) return mm.cc;
    return -1;
}

Module* App::resolve(const MidiTarget& t) {
    if (t.section < 0) return rig.gate();
    if (t.chain < 0 || t.chain > 1 || t.section > 3) return nullptr;
    return rig.chain(t.chain).get(Section(t.section), t.slot);
}

void App::applyMidi(const MidiMessage& m) {
    char buf[64];
    if (m.type() == 0xB0) {
        std::snprintf(buf, sizeof buf, "CC %d = %d (canal %d)", m.data1, m.data2, m.channel() + 1);
        lastMidi = buf;
        const int cc = m.data1, val = m.data2;
        const bool rising = val >= 64 && ccLast_[cc] < 64;
        if (val < 64) ccSeenLow_[cc] = true;
        ccLast_[cc] = uint8_t(val);
        if (learning) {
            learning = false;
            clearMapping(learnTarget);
            settings.midiMap.push_back({cc, learnTarget});
            settings.save();
            notify("MIDI CC " + std::to_string(cc) + " mapeado.");
            return;
        }
        for (auto& mm : settings.midiMap)
            if (mm.cc == cc) applyMapping(mm, val, rising);
    } else if (m.type() == 0xC0) {
        std::snprintf(buf, sizeof buf, "Program Change %d (canal %d)", m.data1, m.channel() + 1);
        lastMidi = buf;
        if (settings.programChangeSelectsPreset) loadPreset(m.data1);
    }
}

void App::applyMapping(const MidiMapping& mm, int value, bool rising) {
    const MidiTarget& t = mm.target;
    const float v01 = float(value) / 127.0f;
    if (t.kind == MidiTarget::Global) {
        switch (t.global) {
            case MidiTarget::Master: rig.masterDb.store(dsp::lerp(-40, 12, v01)); break;
            case MidiTarget::Input: rig.inputDb.store(dsp::lerp(-24, 24, v01)); break;
            case MidiTarget::LooperRec: if (rising) rig.looper.command(Looper::kRecDub); break;
            case MidiTarget::LooperPlay: if (rising) rig.looper.command(Looper::kPlayStop); break;
            case MidiTarget::LooperUndo: if (rising) rig.looper.command(Looper::kUndo); break;
            case MidiTarget::Tuner: if (rising) setTunerOpen(!tunerOpen_); break;
            case MidiTarget::PresetNext: if (rising) stepPreset(1); break;
            case MidiTarget::PresetPrev: if (rising) stepPreset(-1); break;
            case MidiTarget::DualToggle: if (rising) rig.dual.store(!rig.dual.load()); break;
            case MidiTarget::TapTempo: if (rising) tapTempo(); break;
        }
        return;
    }
    Module* mod = resolve(t);
    if (!mod) return;
    if (t.kind == MidiTarget::Bypass) {
        // Latching switches send high/low; momentary ones only send high.
        if (ccSeenLow_[mm.cc]) mod->bypassed.store(value < 64);
        else if (rising || value >= 64) mod->bypassed.store(!mod->bypassed.load());
        return;
    }
    int idx = mod->info().paramIndex(t.paramId);
    if (idx < 0) return;
    const ParamDesc& d = mod->desc(idx);
    if (d.toggle) mod->set(idx, value >= 64 ? 1.0f : 0.0f);
    else if (d.isChoice()) mod->set(idx, std::round(v01 * d.max));
    else mod->setNorm(idx, v01);
}

// ------------------------------------------------------- Tempo and tuner ---

void App::tapTempo() {
    double t = now();
    if (!taps_.empty() && t - taps_.back() > 2.0) taps_.clear();
    taps_.push_back(t);
    if (taps_.size() > 5) taps_.erase(taps_.begin());
    if (taps_.size() >= 2) {
        double avg = (taps_.back() - taps_.front()) / double(taps_.size() - 1);
        g_tempoBpm.store(float(std::clamp(60.0 / avg, 30.0, 300.0)));
    }
}

void App::setTunerOpen(bool open) {
    tunerOpen_ = open;
    rig.muted.store(open && settings.tunerMutes);
    if (!open) pitch = PitchResult{};
}

// --------------------------------------------------------------- Recorder ---

double App::recordedSeconds() const { return double(recFrames_.load()) / std::max(1.0, rig.sampleRate()); }

void App::toggleRecording() {
    if (recRun_.load()) { stopRecording(); return; }
    std::time_t tt = std::time(nullptr);
    std::tm lt{};
    localtime_s(&lt, &tt);
    char name[64];
    std::strftime(name, sizeof name, "ToneForge_%Y%m%d_%H%M%S.wav", &lt);
    recPath_ = documentsDir("Gravações") + "\\" + name;
    if (recThread_.joinable()) recThread_.join();
    recFailed_.store(false);
    rig.recordTap.clear();
    recFrames_.store(0);
    recRun_.store(true);
    rig.recording.store(true);
    recThread_ = std::thread([this] { recorderLoop(); });
    notify("Gravando em " + recPath_, 4.0f);
}

void App::stopRecording() {
    const bool wasRunning = recRun_.exchange(false);
    rig.recording.store(false);
    if (recThread_.joinable()) recThread_.join();
    if (wasRunning) {
        lastRecording = recPath_;
        notify("Gravação salva: " + recPath_, 6.0f);
    }
}

void App::recorderLoop() {
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, ma_uint32(rig.sampleRate()));
    ma_encoder enc;
    if (ma_encoder_init_file_w(utf8ToWide(recPath_).c_str(), &cfg, &enc) != MA_SUCCESS) {
        rig.recording.store(false);
        recFailed_.store(true);
        recRun_.store(false);
        return;
    }
    std::vector<float> buf(8192);
    auto drain = [&] {
        size_t got;
        while ((got = rig.recordTap.pop(buf.data(), buf.size())) > 0) {
            ma_encoder_write_pcm_frames(&enc, buf.data(), got / 2, nullptr);
            recFrames_.fetch_add(got / 2);
        }
    };
    while (recRun_.load()) {
        drain();
        Sleep(20);
    }
    drain();
    ma_encoder_uninit(&enc);
}
