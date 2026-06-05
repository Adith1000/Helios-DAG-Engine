#pragma once
#include <atomic>
#include <cstddef>   

extern std::atomic<std::size_t> tasks_enqueued_total;
extern std::atomic<std::size_t> tasks_completed_total;
extern std::atomic<std::size_t> tasks_in_queue;
extern std::atomic<std::size_t> threads_idle;