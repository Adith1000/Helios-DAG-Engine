#include "scheduler.hpp"
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <stdexcept>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <algorithm>

void sanitize_dag(std::vector<Task>& tasks) {
    std::unordered_map<std::string, Task*> task_map;
    std::unordered_map<std::string, int> state; // 0=Unvisited, 1=Visiting, 2=Visited
    
    std::vector<std::string> current_path; 
    std::unordered_set<std::string> deadlocked_tasks; 

    for (auto& t : tasks) {
        task_map[t.id] = &t;
        state[t.id] = 0;
    }

    auto dfs = [&](auto& self, const std::string& node_id) -> void {
        state[node_id] = 1; 
        current_path.push_back(node_id); 

        Task* t = task_map[node_id];
        
        for (const std::string& dep_id : t->deps) {
            if (task_map.find(dep_id) == task_map.end()) continue;

            if (state[dep_id] == 1) {
                // CYCLE DETECTED! Trace back through the path to find the culprits
                std::cout << "[\033[1;31mDEADLOCK DETECTED\033[0m] Cycle found: ";
                
                auto it = std::find(current_path.begin(), current_path.end(), dep_id);
                if (it != current_path.end()) {
                    for (auto cycle_it = it; cycle_it != current_path.end(); ++cycle_it) {
                        std::cout << *cycle_it << " -> ";
                        deadlocked_tasks.insert(*cycle_it);
                    }
                    std::cout << dep_id << " (Loop closes)\n";
                }
            } else if (state[dep_id] == 0) {
                self(self, dep_id);
            }
        }
        
        state[node_id] = 2; 
        current_path.pop_back(); 
    };

    // 1. Run the DFS to find all cycles
    for (auto& t : tasks) {
        if (state[t.id] == 0) dfs(dfs, t.id);
    }

    // 2. The Executioner: Remove tasks and clean up edges
    if (!deadlocked_tasks.empty()) {
        std::cout << "[\033[1;33mDAG SANITIZER\033[0m] Removing " 
                  << deadlocked_tasks.size() << " deadlocked tasks entirely...\n";

        // Erase the deadlocked tasks from the main vector
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
            [&](const Task& t) {
                return deadlocked_tasks.count(t.id) > 0;
            }), tasks.end());

        // Clean up surviving tasks so they aren't waiting on ghosts
        for (auto& t : tasks) {
            t.deps.erase(std::remove_if(t.deps.begin(), t.deps.end(),
                [&](const std::string& dep) {
                    return deadlocked_tasks.count(dep) > 0;
                }), t.deps.end());
        }
    }
}

struct TaskCompare {
    bool operator()(const Task* a, const Task* b) const {
        return a->priority < b->priority;
    }
};

void run_scheduler(const std::vector<Task>& tasks, const DispatchFn& dispatch_fn) {
    std::unordered_map<std::string, const Task*> task_map;
    std::unordered_map<std::string, std::vector<std::string>> adj;
    std::unordered_map<std::string, int> indegree;

    for (auto& t : tasks) {
        task_map[t.id] = &t;
        indegree[t.id] = 0; 
    }

    for (auto& t : tasks) {
        for (auto& dep : t.deps) {
            if (task_map.find(dep) == task_map.end()) {
                std::cout << "[\033[1;33mWARNING\033[0m] Task '" << t.id 
                          << "' depends on phantom task '" << dep << "'. Ignoring.\n";
                continue;
            }

            adj[dep].push_back(t.id);
            indegree[t.id]++;
        }
    }

    std::priority_queue<const Task*, std::vector<const Task*>, TaskCompare> ready;
    for (auto& [id, deg] : indegree) {
        if (deg == 0) ready.push(task_map[id]);
    }

    std::mutex sched_mtx;
    std::condition_variable sched_cv;
    int processed = 0;
    int active_tasks = 0;

    while (processed < (int)tasks.size()) {
        std::unique_lock<std::mutex> lock(sched_mtx);

        while (!ready.empty()) {
            const Task* cur = ready.top();
            ready.pop();
            active_tasks++;

            lock.unlock();
            
            dispatch_fn(*cur, [&, cur]() { 
                std::lock_guard<std::mutex> cb_lock(sched_mtx);
                if (auto it = adj.find(cur->id); it != adj.end()) {
                    for (auto& nbr_id : it->second) {
                        if (--indegree[nbr_id] == 0) {
                            ready.push(task_map[nbr_id]);
                        }
                    }
                }
                processed++;
                active_tasks--;
                sched_cv.notify_one(); 
            });
            
            lock.lock();
        }

        if (processed < (int)tasks.size() && ready.empty()) {
            if (active_tasks == 0) throw std::runtime_error("Unexpected scheduling stall.");
            sched_cv.wait(lock);
        }
    }
}