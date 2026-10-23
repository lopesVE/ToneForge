// Persistent app settings (%APPDATA%\ToneForge\settings.json).
#pragma once
#include <string>
#include <vector>

#include "audio/AudioEngine.h"
#include "engine/Rig.h"

// What a MIDI CC controls.
struct MidiTarget {
    enum Kind { Param, Bypass, Global } kind = Param;
    // Module targets are addressed by position so they survive preset changes.
    int chain = 0;
    int section = -1;  // -1 = input gate, else int(Section)
    int slot = 0;
    std::string paramId;
    // Global targets.
    enum GlobalId { Master, Input, LooperRec, LooperPlay, LooperUndo, Tuner, PresetNext, PresetPrev, DualToggle, TapTempo };
    int global = Master;

    bool operator==(const MidiTarget& o) const {
        return kind == o.kind && (kind == Global ? global == o.global
                                                 : chain == o.chain && section == o.section && slot == o.slot &&
                                                       (kind == Bypass || paramId == o.paramId));
    }
};

struct MidiMapping {
    int cc = 0;
    MidiTarget target;
};

const char* globalTargetName(int g);

struct AppSettings {
    AudioConfig audio;
    std::string midiDevice;
    bool programChangeSelectsPreset = true;
    std::vector<MidiMapping> midiMap;
    std::string lastPreset;
    float a4 = 440.0f;
    bool tunerMutes = true;
    float uiScale = 1.0f;
    // Main window placement (normal, non-maximized rectangle). winW == 0: unset.
    int winX = 0, winY = 0, winW = 0, winH = 0;
    bool winMaximized = false;

    bool load();  // false when no settings file exists yet (first run)
    void save() const;
};
