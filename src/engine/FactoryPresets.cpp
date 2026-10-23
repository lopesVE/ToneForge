// Built-in presets covering the main styles.
#include <initializer_list>
#include <utility>

#include "engine/Preset.h"

namespace {

using Params = std::initializer_list<std::pair<const char*, float>>;

json::Value mod(const char* type, Params params, bool bypass = false) {
    json::Value v = json::Value::object();
    v["type"] = type;
    v["bypass"] = bypass;
    json::Value p = json::Value::object();
    for (auto& kv : params) p[kv.first] = kv.second;
    v["params"] = p;
    return v;
}

json::Value list(std::initializer_list<json::Value> items) {
    json::Value a = json::Value::array();
    for (auto& i : items) a.push(i);
    return a;
}

// Amp model indices (see Amp.cpp) and cab model indices (see Cab.cpp).
enum { kTweed, kUsClean, kPlexi, kBritLead, kModern, kChime, kBassTube, kSolid };
enum { kOpen112, kBlue212, kGreen412, kV30412, kBass115, kBass810 };

json::Value amp(int model, Params extra = {}) {
    json::Value v = mod("amp", extra);
    v["params"]["model"] = model;
    return v;
}
json::Value cab(int model, Params extra = {}) {
    json::Value v = mod("cab", extra);
    v["params"]["model"] = model;
    return v;
}

json::Value chain(json::Value stomps, json::Value a, json::Value c, json::Value rack, float level = 0, float pan = 0) {
    json::Value v = json::Value::object();
    v["level"] = level;
    v["pan"] = pan;
    v["stomps"] = std::move(stomps);
    v["amp"] = std::move(a);
    v["cab"] = std::move(c);
    v["rack"] = std::move(rack);
    return v;
}

// Chain B used when a single-chain preset is switched to dual mode.
json::Value spareChain() {
    return chain(list({}), amp(kChime, {{"gain", 5}, {"master", 6}}), cab(kBlue212, {{"position", 40}}), list({}), -3, 0);
}

json::Value preset(const char* name, json::Value a, json::Value b = json::Value(), bool dual = false,
                   float gateThr = -62, float bpm = 120, float master = -3) {
    json::Value v = json::Value::object();
    v["name"] = name;
    v["factory"] = true;
    v["version"] = 1;
    v["dual"] = dual;
    v["input"] = 0.0f;
    v["master"] = master;
    v["bpm"] = bpm;
    v["gate"] = mod("gate", {{"threshold", gateThr}});
    json::Value chains = json::Value::array();
    chains.push(std::move(a));
    chains.push(b.isNull() ? spareChain() : std::move(b));
    v["chains"] = chains;
    return v;
}

}  // namespace

std::vector<json::Value> factoryPresets() {
    std::vector<json::Value> p;

    p.push_back(preset("Clean Sparkle",
        chain(list({mod("compressor", {{"threshold", -28}, {"ratio", 3}, {"makeup", 7}, {"mix", 70}})}),
              amp(kUsClean, {{"gain", 4}, {"bass", 5.5f}, {"middle", 4.5f}, {"treble", 6.5f}, {"presence", 6}, {"master", 6}, {"bright", 1}}),
              cab(kOpen112, {{"position", 35}, {"distance", 20}, {"room", 15}}),
              list({mod("chorus", {{"rate", 0.6f}, {"depth", 40}, {"mix", 35}}),
                    mod("reverb", {{"type", 3}, {"decay", 45}, {"mix", 22}})})),
        json::Value(), false, -70));

    p.push_back(preset("Blues Crunch",
        chain(list({mod("overdrive", {{"drive", 3.5f}, {"tone", 5.5f}, {"level", 5}})}),
              amp(kTweed, {{"gain", 6}, {"bass", 5}, {"middle", 6}, {"treble", 6}, {"master", 7}}),
              cab(kOpen112, {{"position", 30}, {"distance", 15}, {"room", 12}}),
              list({mod("delay", {{"time", 340}, {"feedback", 25}, {"mix", 18}, {"mode", 1}}),
                    mod("reverb", {{"type", 0}, {"decay", 40}, {"mix", 18}})}))));

    p.push_back(preset("Classic Rock",
        chain(list({}),
              amp(kPlexi, {{"gain", 7.5f}, {"bass", 5}, {"middle", 6.5f}, {"treble", 6}, {"presence", 6}, {"master", 7}}),
              cab(kGreen412, {{"position", 30}, {"distance", 10}, {"room", 10}}),
              list({mod("delay", {{"time", 380}, {"feedback", 30}, {"mix", 15}, {"mode", 1}}),
                    mod("reverb", {{"type", 1}, {"decay", 45}, {"mix", 15}})}))));

    p.push_back(preset("Lead 800",
        chain(list({mod("overdrive", {{"drive", 1.5f}, {"tone", 6}, {"level", 8}})}),
              amp(kBritLead, {{"gain", 6.5f}, {"bass", 5}, {"middle", 7}, {"treble", 6}, {"presence", 6}, {"master", 6}}),
              cab(kGreen412, {{"position", 25}, {"distance", 10}, {"room", 12}}),
              list({mod("delay", {{"sync", 1}, {"division", 3}, {"feedback", 35}, {"mix", 25}, {"pingpong", 1}}),
                    mod("reverb", {{"type", 1}, {"decay", 55}, {"mix", 20}})})),
        json::Value(), false, -60, 120, -4));

    p.push_back(preset("Modern Metal",
        chain(list({mod("overdrive", {{"drive", 0}, {"tone", 6}, {"level", 6}})}),
              amp(kModern, {{"gain", 7}, {"bass", 6}, {"middle", 4}, {"treble", 6.5f}, {"presence", 6.5f}, {"depth", 6}, {"master", 6}}),
              cab(kV30412, {{"position", 25}, {"distance", 8}, {"room", 6}}),
              list({mod("peq", {{"lowcut", 80}, {"m1f", 500}, {"m1g", -2}}),
                    mod("reverb", {{"type", 0}, {"decay", 30}, {"mix", 10}})})),
        json::Value(), false, -52, 120, -5));

    p.push_back(preset("Ambient Swells",
        chain(list({mod("volume", {{"volume", 100}}), mod("compressor", {{"threshold", -30}, {"ratio", 4}, {"makeup", 8}})}),
              amp(kUsClean, {{"gain", 4.5f}, {"treble", 6}, {"master", 6}}),
              cab(kBlue212, {{"position", 45}, {"distance", 30}, {"room", 25}}),
              list({mod("chorus", {{"rate", 0.3f}, {"depth", 60}, {"mix", 30}}),
                    mod("delay", {{"time", 620}, {"feedback", 55}, {"mix", 40}, {"mode", 2}, {"mod", 40}, {"pingpong", 1}}),
                    mod("reverb", {{"type", 1}, {"decay", 88}, {"predelay", 40}, {"mix", 45}})})),
        json::Value(), false, -70, 100));

    p.push_back(preset("Funk Wah",
        chain(list({mod("compressor", {{"threshold", -26}, {"ratio", 5}, {"attack", 3}, {"makeup", 8}}),
                    mod("wah", {{"mode", 1}, {"sens", 6}, {"q", 7}})}),
              amp(kUsClean, {{"gain", 4}, {"bass", 4.5f}, {"middle", 5}, {"treble", 7}, {"master", 6}, {"bright", 1}}),
              cab(kOpen112, {{"position", 35}, {"distance", 15}}),
              list({mod("reverb", {{"type", 0}, {"decay", 30}, {"mix", 12}})}))));

    p.push_back(preset("Vintage Fuzz",
        chain(list({mod("fuzz", {{"fuzz", 7}, {"bias", 8}, {"volume", -4}}), mod("vibe", {{"speed", 2.5f}, {"intensity", 60}})}),
              amp(kPlexi, {{"gain", 4}, {"master", 7}}),
              cab(kGreen412, {{"position", 35}, {"distance", 15}, {"room", 15}}),
              list({mod("reverb", {{"type", 3}, {"decay", 50}, {"mix", 20}})}))));

    p.push_back(preset("Dual Amp Stereo",
        chain(list({mod("overdrive", {{"drive", 2.5f}, {"level", 4}})}),
              amp(kPlexi, {{"gain", 7}, {"middle", 6.5f}, {"master", 7}}),
              cab(kGreen412, {{"position", 30}, {"distance", 10}}),
              list({mod("delay", {{"time", 360}, {"feedback", 25}, {"mix", 15}})}), -3, -0.8f),
        chain(list({}),
              amp(kChime, {{"gain", 6}, {"treble", 6.5f}, {"master", 6}}),
              cab(kBlue212, {{"position", 40}, {"distance", 15}}),
              list({mod("reverb", {{"type", 1}, {"decay", 45}, {"mix", 18}})}), -3, 0.8f),
        true));

    p.push_back(preset("Bass DI",
        chain(list({mod("compressor", {{"threshold", -25}, {"ratio", 4}, {"attack", 15}, {"makeup", 6}})}),
              amp(kBassTube, {{"gain", 4}, {"bass", 6}, {"middle", 5}, {"treble", 5}, {"master", 6}}),
              cab(kBass810, {{"position", 30}, {"distance", 20}}),
              list({mod("geq7", {{"b800", -2}, {"b3200", 2}})})),
        json::Value(), false, -65));

    p.push_back(preset("Rotary Keys",
        chain(list({mod("overdrive", {{"drive", 1}, {"level", 2}})}),
              amp(kTweed, {{"gain", 4}, {"master", 6}}),
              cab(kBlue212, {{"position", 30}}),
              list({mod("rotary", {{"speed", 0}, {"drive", 3}}), mod("reverb", {{"type", 0}, {"mix", 15}})}))));

    p.push_back(preset("Octave Doom",
        chain(list({mod("octaver", {{"direct", 60}, {"sub1", 70}}), mod("bigfuzz", {{"sustain", 7}, {"tone", 4}, {"volume", -2}})}),
              amp(kModern, {{"gain", 4}, {"bass", 6.5f}, {"middle", 4}, {"master", 6}}),
              cab(kV30412, {{"position", 30}, {"distance", 15}}),
              list({mod("reverb", {{"type", 1}, {"decay", 60}, {"mix", 20}})})),
        json::Value(), false, -55, 70));

    p.push_back(preset("Whammy Lead",
        chain(list({mod("pitch", {{"shift", 12}, {"mix", 100}, {"voice", 0}}, true),
                    mod("distortion", {{"distortion", 6}, {"filter", 4}, {"volume", -3}})}),
              amp(kPlexi, {{"gain", 5}, {"master", 7}}),
              cab(kGreen412, {{"position", 25}}),
              list({mod("delay", {{"time", 420}, {"feedback", 30}, {"mix", 20}})}))));

    // Output trims so every preset lands near -18 dBFS RMS (see tf_selftest).
    const std::pair<const char*, float> masters[] = {
        {"Clean Sparkle", -1}, {"Blues Crunch", -7.8f}, {"Classic Rock", -4}, {"Lead 800", -1.3f},
        {"Modern Metal", -1.6f}, {"Ambient Swells", -2}, {"Funk Wah", -4.2f}, {"Vintage Fuzz", -3.3f},
        {"Dual Amp Stereo", -1}, {"Bass DI", 1.5f}, {"Rotary Keys", -2.7f}, {"Octave Doom", -3}, {"Whammy Lead", -5.5f}};
    for (auto& v : p)
        for (auto& [name, master] : masters)
            if (v.getStr("name") == name) v["master"] = master;
    return p;
}
