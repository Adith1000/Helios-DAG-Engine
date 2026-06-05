#include "parser.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

std::vector<Task> load_tasks(const std::string& filename) {
    std::ifstream in(filename);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    // Parse the JSON array
    nlohmann::json j = nlohmann::json::parse(in);

    std::vector<Task> tasks;
    for (auto& item : j) {
        Task t;
        t.id       = item.at("id").get<std::string>();
        t.priority = item.at("priority").get<int>();
        t.deps     = item.at("deps").get<std::vector<std::string>>();
        // New stateless-artifact fields. Optional so existing task graphs
        // (no runtime/script_name) still parse; such tasks become no-ops on
        // the worker and report "failed: unsupported runtime" cleanly.
        t.runtime     = item.value("runtime", "");
        t.script_name = item.value("script_name", "");
        tasks.push_back(std::move(t));
    }
    return tasks;
}