#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sim {

class WorkerPool {
public:
    using Job = std::function<void()>; //Job now means what its = too

    explicit WorkerPool(std::size_t thread_count);
    ~WorkerPool();

    void enqueue(Job job);
private:
    void worker_loop();

    std::vector<std::thread> threads_;
    std::queue<Job> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace sim