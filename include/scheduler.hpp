#pragma once
#include "task.hpp"
#include <vector>
#include <functional>
#include <string>

// 1. Validates the graph and deletes cycles using 3-Color DFS
void sanitize_dag(std::vector<Task>& tasks);

// 2. Executes the valid graph using parallel Kahn's algorithm
using TaskCallback = std::function<void()>;
using DispatchFn = std::function<void(const Task&, TaskCallback)>;

void run_scheduler(const std::vector<Task>& tasks, const DispatchFn& dispatch_fn);