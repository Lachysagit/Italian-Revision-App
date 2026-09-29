#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sim {

// A token bucket per key, in memory. For the routes a script could hammer -
// joining with guessed codes, translation, sign-in starts - rather than a
// substitute for the daily allowance, which is counted in the database and
// survives a restart. This one forgets everything on restart, which is fine
// for "slow down" and would be wrong for "you have used your five".
class RateLimiter {
public:
    bool allow(const std::string& key, int capacity, std::chrono::seconds window);
    //true and a token spent, or false when the bucket is empty. capacity
    //tokens refill evenly over window, so a burst up to capacity is allowed
    //and then roughly capacity per window

private:
    using Clock = std::chrono::steady_clock;
    struct Bucket {
        double tokens = 0.0;
        Clock::time_point last;
    };

    void sweep(Clock::time_point now);

    std::mutex m_;
    std::unordered_map<std::string, Bucket> buckets_;
    Clock::time_point last_sweep_ = Clock::now();
};

}  // namespace sim
