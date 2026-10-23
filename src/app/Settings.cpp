#include "app/Settings.h"

#include "util/Json.h"
#include "util/Paths.h"

const char* globalTargetName(int g) {
    switch (g) {
        case MidiTarget::Master: return "Master";
        case MidiTarget::Input: return "Ganho de entrada";
        case MidiTarget::LooperRec: return "Looper: Rec/Dub";
        case MidiTarget::LooperPlay: return "Looper: Play/Stop";
        case MidiTarget::LooperUndo: return "Looper: Desfazer";
        case MidiTarget::Tuner: return "Afinador";
        case MidiTarget::PresetNext: return "Próximo preset";
        case MidiTarget::PresetPrev: return "Preset anterior";
        case MidiTarget::DualToggle: return "Liga/desliga cadeia B";
        case MidiTarget::TapTempo: return "Tap tempo";
    }
    return "?";
}

static std::string settingsPath() { return appDataDir() + "\\settings.json"; }

bool AppSettings::load() {
    std::string text;
    json::Value v;
    if (!readTextFile(settingsPath(), text) || !json::parse(text, v)) return false;
    if (const json::Value* a = v.find("audio")) {
        audio.backend = int(a->getNum("backend", 0));
        audio.inputName = a->getStr("input");
        audio.outputName = a->getStr("output");
        audio.sampleRate = int(a->getNum("sampleRate", 48000));
        audio.bufferSize = int(a->getNum("bufferSize", 128));
        audio.inputChannel = int(a->getNum("inputChannel", 0));
        audio.exclusive = a->getBool("exclusive", false);
    }
    midiDevice = v.getStr("midiDevice");
    programChangeSelectsPreset = v.getBool("programChange", true);
    lastPreset = v.getStr("lastPreset");
    a4 = float(v.getNum("a4", 440));
    tunerMutes = v.getBool("tunerMutes", true);
    uiScale = float(v.getNum("uiScale", 1.0));
    if (const json::Value* w = v.find("window")) {
        winX = int(w->getNum("x", 0));
        winY = int(w->getNum("y", 0));
        winW = int(w->getNum("w", 0));
        winH = int(w->getNum("h", 0));
        winMaximized = w->getBool("maximized", false);
    }
    midiMap.clear();
    if (const json::Value* m = v.find("midiMap")) {
        for (auto& e : m->arr) {
            MidiMapping mm;
            mm.cc = int(e.getNum("cc", 0));
            mm.target.kind = MidiTarget::Kind(int(e.getNum("kind", 0)));
            mm.target.chain = int(e.getNum("chain", 0));
            mm.target.section = int(e.getNum("section", 0));
            mm.target.slot = int(e.getNum("slot", 0));
            mm.target.paramId = e.getStr("param");
            mm.target.global = int(e.getNum("global", 0));
            midiMap.push_back(mm);
        }
    }
    return true;
}

void AppSettings::save() const {
    json::Value v = json::Value::object();
    json::Value a = json::Value::object();
    a["backend"] = audio.backend;
    a["input"] = audio.inputName;
    a["output"] = audio.outputName;
    a["sampleRate"] = audio.sampleRate;
    a["bufferSize"] = audio.bufferSize;
    a["inputChannel"] = audio.inputChannel;
    a["exclusive"] = audio.exclusive;
    v["audio"] = a;
    v["midiDevice"] = midiDevice;
    v["programChange"] = programChangeSelectsPreset;
    v["lastPreset"] = lastPreset;
    v["a4"] = a4;
    v["tunerMutes"] = tunerMutes;
    v["uiScale"] = uiScale;
    json::Value w = json::Value::object();
    w["x"] = winX;
    w["y"] = winY;
    w["w"] = winW;
    w["h"] = winH;
    w["maximized"] = winMaximized;
    v["window"] = w;
    json::Value m = json::Value::array();
    for (auto& mm : midiMap) {
        json::Value e = json::Value::object();
        e["cc"] = mm.cc;
        e["kind"] = int(mm.target.kind);
        e["chain"] = mm.target.chain;
        e["section"] = mm.target.section;
        e["slot"] = mm.target.slot;
        e["param"] = mm.target.paramId;
        e["global"] = mm.target.global;
        m.push(e);
    }
    v["midiMap"] = m;
    writeTextFile(settingsPath(), json::dump(v));
}
