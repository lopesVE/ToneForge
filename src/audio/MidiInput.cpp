#include "audio/MidiInput.h"

#include <windows.h>
#include <mmsystem.h>

#include "util/Paths.h"

static void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (msg == MIM_DATA) reinterpret_cast<MidiInput*>(instance)->enqueue(uint32_t(param1));
}

MidiInput::~MidiInput() { close(); }

std::vector<std::string> MidiInput::devices() {
    std::vector<std::string> out;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps;
        if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) out.push_back(wideToUtf8(caps.szPname));
        else out.push_back("MIDI " + std::to_string(i + 1));
    }
    return out;
}

bool MidiInput::open(int index) {
    close();
    if (index < 0) return true;
    HMIDIIN h = nullptr;
    if (midiInOpen(&h, UINT(index), DWORD_PTR(&midiProc), DWORD_PTR(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return false;
    midiInStart(h);
    handle_ = h;
    index_ = index;
    return true;
}

void MidiInput::close() {
    if (handle_) {
        HMIDIIN h = static_cast<HMIDIIN>(handle_);
        midiInStop(h);
        midiInReset(h);
        midiInClose(h);
        handle_ = nullptr;
    }
    index_ = -1;
}

void MidiInput::enqueue(uint32_t packed) {
    uint32_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) >= kQueue) return;  // full: drop
    queue_[w % kQueue] = packed;
    write_.store(w + 1, std::memory_order_release);
}

bool MidiInput::pop(MidiMessage& m) {
    uint32_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    uint32_t p = queue_[r % kQueue];
    read_.store(r + 1, std::memory_order_release);
    m.status = uint8_t(p & 0xFF);
    m.data1 = uint8_t((p >> 8) & 0x7F);
    m.data2 = uint8_t((p >> 16) & 0x7F);
    return true;
}
