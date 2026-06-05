#pragma once
#include <string>
#include <vector>

struct Task {
    std::string id;
    int priority;
    std::vector<std::string> deps;
    std::string runtime;
    std::string script_name;
    std::vector<std::string> pkg_deps;
};