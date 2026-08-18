#include "sim/worker.hpp"

#include <utility>

namespace sim {

WorkerPool::WorkerPool(std::size_t thread_count) { //unsigned int param
    for (std::size_t i = 0; i < thread_count; ++i) { 
        //for loop with i set to same type as variable looped through
        threads_.emplace_back([this] { worker_loop(); });
    }
}

void WorkerPool::worker_loop() {
    while (true) {
        Job currentjob;

        {
            std::unique_lock<std::mutex> lock(mutex_); 
            //local variable lock given mutex_ instantiated to the type of unique_lock
            conditionalv_.wait(lock, [this] {
                return stop_ || !jobs_.empty();
            // this is used to access the WorkerPool objects private variables
            //predicate which returns true if stop_ is true or jobs is not empty
            //if its true the thread hangs onto the lock and progresses
            });

            if (stop_ && jobs_.empty()) {
                return;
            //check if no jobs and stop_ is true therefore shutdown
            }

            currentjob = std::move(jobs_.front());
            // the front element of jobs is a std::function<void()>
            //therefore is can be stored in the 
            jobs_.pop();    
            //pop the now unspecified first element of jobs away

        } //lock releases before job operation

        currentjob();
    }
}