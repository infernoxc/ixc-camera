#include "camera/reconnect_policy.h"

namespace ixc::camera {

ReconnectPolicy::ReconnectPolicy(std::vector<ms> delays, int maxAttempts)
    : delays_(std::move(delays)), maxAttempts_(maxAttempts < 0 ? 0 : maxAttempts) {
    if (delays_.empty()) delays_.push_back(ms(1000));
}

std::optional<ReconnectPolicy::ms> ReconnectPolicy::NextDelay() {
    if (attempt_ >= maxAttempts_) return std::nullopt;
    const size_t i = static_cast<size_t>(attempt_) < delays_.size() ? static_cast<size_t>(attempt_) : delays_.size() - 1;
    ++attempt_;
    return delays_[i];
}

}  // namespace ixc::camera
