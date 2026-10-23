// Single-producer / single-consumer lock-free float ring buffer.
#pragma once
#include <algorithm>
#include <atomic>
#include <vector>

class RingBuffer {
public:
    void allocate(size_t capacity) {
        size_t n = 1;
        while (n < capacity) n <<= 1;
        buf_.assign(n, 0.0f);
        mask_ = n - 1;
        write_.store(0);
        read_.store(0);
    }
    size_t available() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed); }
    size_t space() const { return buf_.size() - (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire)); }

    // Producer. Returns false (and drops data) when full.
    bool push(const float* data, size_t n) {
        if (buf_.empty() || n > space()) return false;
        size_t w = write_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = data[i];
        write_.store(w + n, std::memory_order_release);
        return true;
    }
    // Consumer.
    size_t pop(float* out, size_t maxN) {
        size_t n = std::min(maxN, available());
        size_t r = read_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(r + i) & mask_];
        read_.store(r + n, std::memory_order_release);
        return n;
    }
    void clear() { read_.store(write_.load()); }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    std::atomic<size_t> write_{0}, read_{0};
};
