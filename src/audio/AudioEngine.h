// Audio device I/O through miniaudio (WASAPI shared/exclusive, DirectSound, WinMM).
#pragma once
#include <memory>
#include <string>
#include <vector>

class Rig;

struct AudioConfig {
    int backend = 0;               // index into AudioEngine::backendNames()
    std::string inputName, outputName;  // empty = system default
    int sampleRate = 48000;
    int bufferSize = 128;          // frames per period
    int inputChannel = 0;          // 0 = input 1, 1 = input 2, 2 = mix 1+2
    bool exclusive = false;        // WASAPI exclusive mode (lower latency)
};

struct AudioStatus {
    bool running = false;
    std::string error;
    int sampleRate = 0, bufferSize = 0;
    float latencyMs = 0;
    std::string inputName, outputName;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    static const std::vector<std::string>& backendNames();
    // Enumerates devices for the backend selected in `cfg`.
    void refreshDevices(int backend);
    const std::vector<std::string>& inputDevices() const { return inputs_; }
    const std::vector<std::string>& outputDevices() const { return outputs_; }

    bool start(const AudioConfig& cfg, Rig& rig);
    void stop();
    const AudioStatus& status() const { return status_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> inputs_, outputs_;
    AudioStatus status_;
};
