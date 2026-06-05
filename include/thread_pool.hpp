#pragma once
#include <cstddef>
#include <functional>

struct ThreadPool {
    // Start 'num_threads' worker threads
    void start(size_t num_threads);

    // Enqueue a job for workers to execute
    void enqueue(std::function<void()> job);

    // Signal shutdown: finish queued jobs and join threads
    void shutdown();
};
