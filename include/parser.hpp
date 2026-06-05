#pragma once
#include "task.hpp"
#include <string>
#include <vector>

// Load tasks from the given JSON file and return them.
std::vector<Task> load_tasks(const std::string& filename);
