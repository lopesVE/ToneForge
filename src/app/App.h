// Application state shared by the UI: rig, audio, MIDI, presets, tuner, recorder.
#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "app/Settings.h"
#include "audio/AudioEngine.h"
#include "audio/MidiInput.h"
#include "engine/Preset.h"
#include "engine/Rig.h"
#include "engine/Tuner.h"

class App {
public:
    App();
    ~App();
    // noAudio: never open audio devices or write settings (testing).
    void init(bool noAudio = false);
    bool firstRun = false;
    void shutdown();
    void frame();  // once per UI frame

    Rig rig;
    AudioEngine audio;
    MidiInput midi;
    AppSettings settings;

    // ---- Presets ----
    struct PresetItem {
        std::string name, path;
        bool factory = false;
        int factoryIndex = -1;
    };
    std::vector<PresetItem> presets;
    int currentPreset = -1;
    std::string currentName = "Novo";
    void refreshPresets();
    void loadPreset(int index);
    void stepPreset(int dir);
    bool saveCurrent();
    bool savePresetAs(const std::string& name);
    bool deleteCurrent();
    void newPreset();
    bool dirty() const { return dirty_; }
    bool currentIsFactory() const;
    const std::string& presetDir() const { return presetDir_; }

    // ---- Audio ----
    void applyAudio(const AudioConfig& cfg);

    // ---- MIDI ----
    void openMidi(const std::string& deviceName);
    void startLearn(const MidiTarget& t);
    void cancelLearn() { learning = false; }
    void clearMapping(const MidiTarget& t);
    int mappedCc(const MidiTarget& t) const;
    bool learning = false;
    MidiTarget learnTarget;
    std::string lastMidi;
    Module* resolve(const MidiTarget& t);

    // ---- Tempo / tuner / looper ----
    void tapTempo();
    void setTunerOpen(bool open);
    bool tunerOpen() const { return tunerOpen_; }
    PitchResult pitch;
    NoteInfo note;

    // ---- Recorder ----
    void toggleRecording();
    bool recording() const { return recRun_.load(); }
    double recordedSeconds() const;
    std::string lastRecording;

    // ---- Notifications ----
    void notify(const std::string& msg, float seconds = 3.0f);
    std::string toast;
    double toastUntil = 0;
    double now() const;

private:
    void applyMidi(const MidiMessage& m);
    void applyMapping(const MidiMapping& mm, int value, bool rising);
    void updateDirty();
    void markClean();
    void loadPresetJson(const json::Value& v, int index);
    void recorderLoop();
    void stopRecording();

    std::vector<json::Value> factory_;
    std::string presetDir_, savedJson_;
    bool dirty_ = false;
    int dirtyCounter_ = 0;

    bool noAudio_ = false;
    bool tunerOpen_ = false;
    double lastPitchTime_ = 0;
    std::vector<double> taps_;
    uint8_t ccLast_[128] = {};
    bool ccSeenLow_[128] = {};

    std::thread recThread_;
    std::atomic<bool> recRun_{false}, recFailed_{false};
    std::atomic<uint64_t> recFrames_{0};
    std::string recPath_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
