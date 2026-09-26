#pragma once

// Bounded reconnect backoff. After the attempts run out, the session stops retrying and
// waits for a device-arrival notification, so a missing camera costs zero CPU.

#include <chrono>
#include <optional>
#include <vector>

namespace ixc::camera {

class ReconnectPolicy {
public:
    using ms = std::chrono::milliseconds;

    ReconnectPolicy() : ReconnectPolicy({ms(250), ms(500), ms(1000), ms(2000), ms(4000)}, 6) {}
    // `delays` is used in order; once exhausted the last delay repeats until maxAttempts.
    ReconnectPolicy(std::vector<ms> delays, int maxAttempts);

    void Reset() { attempt_ = 0; }
    int Attempts() const { return attempt_; }
    int MaxAttempts() const { return maxAttempts_; }

    // Delay before the next attempt, or nullopt when attempts are exhausted.
    std::optional<ms> NextDelay();

private:
    std::vector<ms> delays_;
    int maxAttempts_;
    int attempt_ = 0;
};

}  // namespace ixc::camera
