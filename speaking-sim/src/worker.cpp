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
            //uns mutex lock on the mutex object which is a class variable of worker
            conditionalv_.wait(lock, [this] {
                return stop_ || !jobs_.empty();
            //conditionalv is of type std::conditional which has the .wait method
            //.wait takes the lock object and the lambda which captures the WorkerPool Object
        
            //predicate returns FALSE if no work and no shutdown, 
            //.wait calls mutex unlock and its released and thread sleeps

            //predicate returns TRUE if shutdown or there is work
            //if statement is evaluated to check for shutdown
            });

            if (stop_ && jobs_.empty()) {
                return;
            //only shutdown if stop_ is true and no jobs 
            }

            currentjob = std::move(jobs_.front());
            // the front element of jobs is a std::function<void()>
            //therefore is can be stored in the currentjob which is the same type
            jobs_.pop();    
            //pop the now unspecified first element of jobs away

        } //lock releases before job operation as lock variable goes out of scope

        currentjob();
    }
}