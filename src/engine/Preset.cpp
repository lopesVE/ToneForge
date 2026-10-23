#include "engine/Preset.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "engine/Tempo.h"
#include "util/Paths.h"

json::Value moduleToJson(const Module& m) {
    json::Value v = json::Value::object();
    v["type"] = m.info().typeId;
    v["bypass"] = m.bypassed.load();
    json::Value params = json::Value::object();
    for (int i = 0; i < m.numParams(); ++i) params[m.desc(i).id] = m.get(i);
    v["params"] = params;
    std::string extra = m.extraState();
    if (!extra.empty()) v["extra"] = extra;
    return v;
}

static ModulePtr moduleFromJson(const json::Value& v, PresetData& out) {
    if (!v.isObject()) return nullptr;
    ModulePtr m = createModule(v.getStr("type"));
    if (!m) return nullptr;
    m->bypassed.store(v.getBool("bypass", false));
    if (const json::Value* params = v.find("params")) {
        for (auto& kv : params->obj) {
            int idx = m->info().paramIndex(kv.first);
            if (idx >= 0 && kv.second.type == json::Value::Type::Number) m->set(idx, float(kv.second.num));
        }
    }
    std::string extra = v.getStr("extra");
    if (!extra.empty()) out.extras.emplace_back(m.get(), extra);
    return m;
}

json::Value rigToJson(Rig& rig, const std::string& name) {
    json::Value v = json::Value::object();
    v["name"] = name;
    v["version"] = 1;
    v["dual"] = rig.dual.load();
    v["input"] = rig.inputDb.load();
    v["master"] = rig.masterDb.load();
    v["bpm"] = g_tempoBpm.load();
    if (rig.gate()) v["gate"] = moduleToJson(*rig.gate());
    json::Value chains = json::Value::array();
    for (int c = 0; c < 2; ++c) {
        Chain& ch = rig.chain(c);
        json::Value cj = json::Value::object();
        cj["level"] = ch.levelDb.load();
        cj["pan"] = ch.pan.load();
        json::Value stomps = json::Value::array();
        for (auto& m : ch.stomps) stomps.push(moduleToJson(*m));
        cj["stomps"] = stomps;
        cj["amp"] = ch.amp ? moduleToJson(*ch.amp) : json::Value();
        cj["cab"] = ch.cab ? moduleToJson(*ch.cab) : json::Value();
        json::Value rack = json::Value::array();
        for (auto& m : ch.rack) rack.push(moduleToJson(*m));
        cj["rack"] = rack;
        chains.push(cj);
    }
    v["chains"] = chains;
    return v;
}

bool presetFromJson(const json::Value& v, PresetData& out) {
    if (!v.isObject()) return false;
    out.name = v.getStr("name", "Sem nome");
    RigState& st = out.state;
    st.dual = v.getBool("dual", false);
    st.inputDb = float(v.getNum("input", 0));
    st.masterDb = float(v.getNum("master", 0));
    st.bpm = float(v.getNum("bpm", 120));
    if (const json::Value* g = v.find("gate")) st.gate = moduleFromJson(*g, out);
    if (!st.gate) st.gate = createModule("gate");
    const json::Value* chains = v.find("chains");
    if (!chains || !chains->isArray()) return false;
    for (size_t c = 0; c < 2 && c < chains->arr.size(); ++c) {
        const json::Value& cj = chains->arr[c];
        st.levelDb[c] = float(cj.getNum("level", 0));
        st.pan[c] = float(cj.getNum("pan", 0));
        if (const json::Value* s = cj.find("stomps"))
            for (auto& m : s->arr)
                if (auto mod = moduleFromJson(m, out); mod && st.stomps[c].size() < size_t(Rig::kMaxStomps))
                    st.stomps[c].push_back(std::move(mod));
        if (const json::Value* a = cj.find("amp")) st.amp[c] = moduleFromJson(*a, out);
        if (const json::Value* k = cj.find("cab")) st.cab[c] = moduleFromJson(*k, out);
        if (const json::Value* r = cj.find("rack"))
            for (auto& m : r->arr)
                if (auto mod = moduleFromJson(m, out); mod && st.rack[c].size() < size_t(Rig::kMaxRack))
                    st.rack[c].push_back(std::move(mod));
    }
    return true;
}

void applyPreset(Rig& rig, PresetData&& p) {
    auto extras = std::move(p.extras);
    rig.loadState(std::move(p.state));
    for (auto& [m, extra] : extras) m->setExtraState(extra);
}

std::vector<PresetEntry> listPresets(const std::string& dir) {
    std::vector<PresetEntry> out;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = utf8ToWide(dir) + L"\\*.json";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        PresetEntry e;
        e.path = dir + "\\" + wideToUtf8(fd.cFileName);
        json::Value v;
        if (loadPresetFile(e.path, v)) {
            e.name = v.getStr("name");
            e.factory = v.getBool("factory", false);
        }
        if (e.name.empty()) {
            e.name = wideToUtf8(fd.cFileName);
            e.name = e.name.substr(0, e.name.size() - 5);
        }
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) {
        if (a.factory != b.factory) return a.factory;  // factory presets first
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

bool loadPresetFile(const std::string& path, json::Value& out) {
    std::string text;
    return readTextFile(path, text) && json::parse(text, out) && out.isObject();
}

bool savePresetFile(const std::string& path, const json::Value& v) { return writeTextFile(path, json::dump(v)); }

std::string presetFileName(const std::string& dir, const std::string& name) {
    std::string safe;
    for (char c : name) safe += (strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32) ? '_' : c;
    if (safe.empty()) safe = "preset";
    return dir + "\\" + safe + ".json";
}
