#include "tracer.hpp"
#include <fstream>

void Tracer::init(const std::vector<Task>& tasks) {
    std::lock_guard<std::mutex> lock(mtx);
    for (const auto& t : tasks) {
        task_map[t.id] = { t.id, t.priority, t.deps, TaskState::NotStarted };
    }
}

void Tracer::set_state(const std::string& id, TaskState state) {
    std::lock_guard<std::mutex> lock(mtx);
    task_map[id].state = state;
}

void Tracer::dump_dot(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream out(filename);
    out << "digraph DAG {\n";

    for (const auto& [id, node] : task_map) {
        std::string color = "gray";
        if (node.state == TaskState::Running) color = "blue";
        else if (node.state == TaskState::Finished) color = "green";

        out << "  \"" << id << "\" [style=filled, fillcolor=" << color
            << ", label=\"" << id << "\\nP=" << node.priority << "\"];\n";
        for (const std::string& dep : node.deps) {
            out << "  \"" << dep << "\" -> \"" << id << "\";\n";
        }
    }
    out << "}\n";
}