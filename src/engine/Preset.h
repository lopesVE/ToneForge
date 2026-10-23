// Preset (de)serialization and the preset folder on disk.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "engine/Rig.h"
#include "util/Json.h"

struct PresetData {
    std::string name;
    RigState state;
    // Extra string state (e.g. IR paths) applied after the modules are live.
    std::vector<std::pair<Module*, std::string>> extras;
};

json::Value moduleToJson(const Module& m);
json::Value rigToJson(Rig& rig, const std::string& name);
bool presetFromJson(const json::Value& v, PresetData& out);
void applyPreset(Rig& rig, PresetData&& p);

struct PresetEntry {
    std::string name, path;
    bool factory = false;
};
std::vector<PresetEntry> listPresets(const std::string& dir);
bool loadPresetFile(const std::string& path, json::Value& out);
bool savePresetFile(const std::string& path, const json::Value& v);
std::string presetFileName(const std::string& dir, const std::string& name);

// Built-in presets, also written to the preset folder on first run.
std::vector<json::Value> factoryPresets();
