#pragma once

// Single-slot "newest value wins" handoff between a producer (camera callback) and a consumer
// (preview / processing). Never queues: a value the consumer didn't take in time is replaced
// and counted as dropped, so latency can't build up behind a slow consumer.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace ixc::camera {

template <typename T>
class LatestMailbox {
public:
    // Returns true when an untaken value was replaced (a dropped frame).
    bool Put(T value) {
        std::optional<T> old;
        {
            std::lock_guard lock(mu_);
            old = std::move(slot_);
            slot_ = std::move(value);
        }
        // `old` is destroyed outside the lock (releasing a sample can be non-trivial).
        if (old) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    std::optional<T> Take() {
        std::lock_guard lock(mu_);
        std::optional<T> v = std::move(slot_);
        slot_.reset();
        return v;
    }

    void Clear() { (void)Take(); }

    std::uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void ResetDropped() { dropped_.store(0, std::memory_order_relaxed); }

private:
    std::mutex mu_;
    std::optional<T> slot_;
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace ixc::camera
