#pragma once
#include "task.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>

enum class TaskState { NotStarted, Running, Finished };

struct TracerNode {
    std::string id;
    int priority;
    std::vector<std::string> deps;
    TaskState state;
};

class Tracer {
public:
    void init(const std::vector<Task>& tasks);
    void set_state(const std::string& id, TaskState state);
    void dump_dot(const std::string& filename) const;

private:
    std::unordered_map<std::string, TracerNode> task_map;
    mutable std::mutex mtx; // Mutable so we can lock it in const methods like dump_dot
};