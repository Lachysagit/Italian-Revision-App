#include "sim/rate_limit.hpp"

#include <algorithm>

namespace sim {

namespace {

// A bucket untouched this long has long since refilled, so forgetting it
// changes nothing but the size of the map.
constexpr auto kIdleForget = std::chrono::minutes(15);
constexpr auto kSweepEvery = std::chrono::minutes(5);

}  // namespace

bool RateLimiter::allow(const std::string& key, int capacity,
                        std::chrono::seconds window) {
    const Clock::time_point now = Clock::now();
    std::lock_guard<std::mutex> lock(m_);
    sweep(now);

    auto [it, inserted] = buckets_.try_emplace(key);
    Bucket& bucket = it->second;
    if (inserted) {
        bucket.tokens = capacity;
        bucket.last = now;
    } else {
        const double elapsed =
            std::chrono::duration<double>(now - bucket.last).count();
        const double rate = static_cast<double>(capacity) / window.count();
        bucket.tokens = std::min<double>(capacity, bucket.tokens + elapsed * rate);
        bucket.last = now;
    }

    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

void RateLimiter::sweep(Clock::time_point now) {
    if (now - last_sweep_ < kSweepEvery) return;
    last_sweep_ = now;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        it = now - it->second.last > kIdleForget ? buckets_.erase(it) : std::next(it);
    }
}

}  // namespace sim
