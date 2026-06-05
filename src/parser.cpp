#include "parser.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

std::vector<Task> load_tasks(const std::string& filename) {
    std::ifstream in(filename);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    nlohmann::json j = nlohmann::json::parse(in);

    std::vector<Task> tasks;
    for (auto& item : j) {
        Task t;
        t.id       = item.at("id").get<std::string>();
        t.priority = item.at("priority").get<int>();
        // deps is OPTIONAL. The root task legitimately has no dependencies and
        // omits the key; using .at() here threw json.out_of_range.403 on it and
        // killed the master before any task was dispatched.
        t.deps     = item.value("deps", std::vector<std::string>{});
        // Stateless-artifact + polyglot fields. All optional for backward compat.
        t.runtime     = item.value("runtime", "");
        t.script_name = item.value("script_name", "");
        t.pkg_deps    = item.value("pkg_deps", std::vector<std::string>{});
        tasks.push_back(std::move(t));
    }
    return tasks;
}