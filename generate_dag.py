import json
import random

NUM_TASKS = 10000
MAX_DEPS_PER_TASK = 5

tasks = []

print(f"Generating {NUM_TASKS} tasks...")

for i in range(NUM_TASKS):
    task_id = f"T_{i}"
    # Random priority between 1 and 10
    priority = random.randint(1, 10)
    
    deps = []
    # To guarantee a DAG, task 'i' can only depend on tasks from 0 to 'i-1'
    if i > 0:
        # Pick a random number of dependencies (0 to MAX_DEPS)
        num_deps = random.randint(0, min(MAX_DEPS_PER_TASK, i))
        if num_deps > 0:
            # Randomly select 'num_deps' tasks from the previously created tasks
            dep_indices = random.sample(range(i), num_deps)
            deps = [f"T_{j}" for j in dep_indices]
            
    tasks.append({
        "id": task_id,
        "priority": priority,
        "deps": deps
    })

# Write to the JSON file
output_file = "tasks.json"
with open(output_file, "w") as f:
    json.dump(tasks, f, indent=2)

print(f"Success! Generated {output_file}.")