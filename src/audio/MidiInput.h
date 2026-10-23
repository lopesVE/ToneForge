// MIDI input through the Windows multimedia API (WinMM).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct MidiMessage {
    uint8_t status = 0, data1 = 0, data2 = 0;
    int type() const { return status & 0xF0; }
    int channel() const { return status & 0x0F; }
};

class MidiInput {
public:
    ~MidiInput();
    static std::vector<std::string> devices();
    bool open(int index);
    void close();
    int openIndex() const { return index_; }
    // UI thread: next queued message, if any.
    bool pop(MidiMessage& m);

    void enqueue(uint32_t packed);  // driver callback thread

private:
    void* handle_ = nullptr;
    int index_ = -1;
    static constexpr int kQueue = 1024;
    uint32_t queue_[kQueue] = {};
    std::atomic<uint32_t> write_{0}, read_{0};
};
