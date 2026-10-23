// Stereo looper with overdub and one level of undo/redo. All buffers are
// allocated in prepare(); the audio thread never allocates. Overdubs go to one
// of two layer buffers; the previous layer is folded into the base loop
// incrementally so no single audio block does a full-buffer pass.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

class Looper {
public:
    enum class State { Empty, Recording, Playing, Overdubbing, Stopped };
    enum Command { kRecDub = 1, kPlayStop = 2, kUndo = 4, kClear = 8 };

    void prepare(double sr, double maxSeconds) {
        sr_ = sr;
        cap_ = int(sr * maxSeconds);
        for (auto& b : base_) b.assign(size_t(cap_), 0.0f);
        for (auto& l : layer_)
            for (auto& b : l) b.assign(size_t(cap_), 0.0f);
        len_ = pos_ = 0;
        state_ = State::Empty;
        publish();
    }

    // UI thread.
    void command(Command c) { pending_.fetch_or(int(c)); }
    State state() const { return State(stateOut_.load()); }
    float progress() const { int l = lenOut_.load(); return l > 0 ? float(posOut_.load()) / float(l) : 0.0f; }
    float lengthSeconds() const { return float(lenOut_.load() / sr_); }
    float recordedSeconds() const { return float(posOut_.load() / sr_); }
    bool hasUndo() const { return undoAvailable_.load(); }
    bool undone() const { return undoneOut_.load(); }
    std::atomic<float> levelDb{0.0f};

    // Audio thread: record `in` and add loop playback into L/R.
    void process(float* L, float* R, int n) {
        handleCommands();
        if (state_ == State::Empty || state_ == State::Stopped) { publish(); return; }
        mergeStep(n);
        const float gain = std::pow(10.0f, levelDb.load(std::memory_order_relaxed) / 20.0f);
        for (int i = 0; i < n; ++i) {
            if (state_ == State::Recording) {
                base_[0][size_t(pos_)] = L[i];
                base_[1][size_t(pos_)] = R[i];
                layer_[0][0][size_t(pos_)] = layer_[0][1][size_t(pos_)] = 0;
                layer_[1][0][size_t(pos_)] = layer_[1][1][size_t(pos_)] = 0;
                if (++pos_ >= cap_) { len_ = cap_; pos_ = 0; state_ = State::Playing; }
                continue;
            }
            const size_t p = size_t(pos_);
            float pl = base_[0][p], pr = base_[1][p];
            for (int k = 0; k < 2; ++k) {
                if (muted_[k]) continue;
                pl += layer_[k][0][p];
                pr += layer_[k][1][p];
            }
            if (state_ == State::Overdubbing) {
                layer_[cur_][0][p] += L[i];
                layer_[cur_][1][p] += R[i];
            }
            L[i] += pl * gain;
            R[i] += pr * gain;
            if (++pos_ >= len_) pos_ = 0;
        }
        publish();
    }

private:
    void handleCommands() {
        int cmd = pending_.exchange(0);
        if (!cmd) return;
        if (cmd & kClear) {
            state_ = State::Empty;
            len_ = pos_ = 0;
            hasLayer_[0] = hasLayer_[1] = false;
            muted_[0] = muted_[1] = false;
            merging_ = false;
            return;
        }
        if (cmd & kRecDub) {
            switch (state_) {
                case State::Empty: pos_ = 0; state_ = State::Recording; break;
                case State::Recording: finishRecording(State::Playing); break;
                case State::Playing: startLayer(); break;
                case State::Overdubbing: state_ = State::Playing; break;
                case State::Stopped: pos_ = 0; startLayer(); break;
            }
        }
        if (cmd & kPlayStop) {
            switch (state_) {
                case State::Empty: break;
                case State::Recording: finishRecording(State::Stopped); break;
                case State::Playing:
                case State::Overdubbing: state_ = State::Stopped; pos_ = 0; break;
                case State::Stopped: pos_ = 0; state_ = State::Playing; break;
            }
        }
        if (cmd & kUndo) {
            if (state_ == State::Overdubbing) state_ = State::Playing;
            if (hasLayer_[cur_]) muted_[cur_] = !muted_[cur_];
        }
    }
    void finishRecording(State next) {
        len_ = pos_;
        pos_ = 0;
        if (len_ < int(0.1 * sr_)) { state_ = State::Empty; len_ = 0; return; }
        state_ = next;
    }
    void startLayer() {
        if (hasLayer_[cur_]) {
            if (merging_) mergeAll();  // previous fold still running: finish it now
            mergeLayer_ = cur_;
            mergeCursor_ = 0;
            merging_ = true;
            cur_ = 1 - cur_;
        }
        hasLayer_[cur_] = true;
        muted_[cur_] = false;
        state_ = State::Overdubbing;
    }
    void mergeRange(int from, int to) {
        const int k = mergeLayer_;
        for (int c = 0; c < 2; ++c) {
            float* b = base_[c].data();
            float* l = layer_[k][c].data();
            for (int i = from; i < to; ++i) {
                if (!muted_[k]) b[i] += l[i];
                l[i] = 0;
            }
        }
    }
    void mergeStep(int n) {
        if (!merging_) return;
        // Finish the fold within ~0.5 s regardless of loop length.
        int step = std::max(256, int(double(len_) * n / (0.5 * sr_)) + 1);
        int to = std::min(len_, mergeCursor_ + step);
        mergeRange(mergeCursor_, to);
        mergeCursor_ = to;
        if (mergeCursor_ >= len_) finishMerge();
    }
    void mergeAll() {
        mergeRange(mergeCursor_, len_);
        finishMerge();
    }
    void finishMerge() {
        merging_ = false;
        hasLayer_[mergeLayer_] = false;
        muted_[mergeLayer_] = false;
    }
    void publish() {
        stateOut_.store(int(state_));
        posOut_.store(state_ == State::Recording ? pos_ : pos_);
        lenOut_.store(state_ == State::Recording ? std::max(pos_, 1) : len_);
        undoAvailable_.store(hasLayer_[cur_]);
        undoneOut_.store(hasLayer_[cur_] && muted_[cur_]);
    }

    double sr_ = 48000;
    int cap_ = 0, len_ = 0, pos_ = 0;
    State state_ = State::Empty;
    std::vector<float> base_[2], layer_[2][2];
    int cur_ = 0, mergeLayer_ = 0, mergeCursor_ = 0;
    bool hasLayer_[2] = {false, false}, muted_[2] = {false, false}, merging_ = false;
    std::atomic<int> pending_{0}, stateOut_{0}, posOut_{0}, lenOut_{0};
    std::atomic<bool> undoAvailable_{false}, undoneOut_{false};
};
