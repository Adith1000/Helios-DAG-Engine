# Helios-DAG: High-Throughput Distributed Workflow Engine

**Helios-DAG** is a high-performance, fault-tolerant distributed task scheduler written in C++20. It utilizes **ZeroMQ** for asynchronous messaging to orchestrate complex Directed Acyclic Graphs (DAGs) across a cluster of worker nodes. 

The system is designed to handle thousands of inter-dependent tasks with sub-millisecond scheduling latency, featuring automated cycle detection and real-time node recovery.

## 🚀 Key Features

* **Asynchronous Distribution:** Uses ZeroMQ `PUSH/PULL` patterns for lock-free task dispatching and load balancing across multiple workers.
* **Dynamic Fault Tolerance:** A dedicated Watchdog thread monitors 1Hz heartbeats via `PUB/SUB`. If a worker fails, its active tasks are automatically re-queued and reassigned within 5 seconds.
* **Graph Sanitization (Cycle Detection):** Implements a **3-Color Depth-First Search (DFS)** to identify and surgically remove deadlocked cycles before execution begins.
* **Priority-Aware Scheduling:** Uses a custom `std::priority_queue` implementation of Kahn’s Algorithm to ensure high-priority tasks are dispatched as soon as their dependencies are met.
* **Live Observability:** Integrated Prometheus-compatible metrics server (via `cpp-httplib`) providing real-time data on throughput, task completion, and active worker counts.

---

## 📊 Performance Benchmarks

Tested on a local cluster simulation:

| Metric | Result |
| :--- | :--- |
| **Total Task Volume** | 10,000 Tasks |
| **Execution Time** | ~21.2 seconds |
| **Average Throughput** | **470+ tasks / second** |
| **Scheduling Complexity** | $\mathcal{O}(V \log V + E)$ |
| **Recovery Latency** | < 5.0 seconds |

---

## 🛠️ Technical Stack

* **Language:** C++20 (Standard Template Library)
* **Networking:** ZeroMQ (libzmq / cppzmq)
* **Serialization:** nlohmann/json
* **Web/Metrics:** cpp-httplib
* **Build System:** CMake

---

## 📂 Project Structure

```text
Helios-DAG/
├── src/                # Core Implementations
│   ├── main.cpp        # Master Orchestrator & Fault Recovery
│   ├── worker.cpp      # Distributed Worker Logic
│   ├── scheduler.cpp   # Kahn's Algorithm & DFS Sanitizer
│   ├── tracer.cpp      # State Tracking & DOT Generation
│   └── parser.cpp      # JSON DAG Parser
├── include/            # Header Files
├── scripts/            # Python DAG Generation Utilities
├── tests/              # Sample JSON DAGs (Standard, Cyclic, Massive)
└── CMakeLists.txt      # Build Configuration


## ⚡ Quick Start

### Prerequisites
Ensure you have a C++17 compiler, CMake, ZeroMQ, and Graphviz installed.
```bash
# macOS (Homebrew)
brew install cmake zeromq graphviz nlohmann-json
```

### Build Instructions
```bash
mkdir build && cd build
cmake ..
make -j4
```

### Running the Engine
**1. Start the Master Node:**
```bash
./scheduler_master ../tests/tasks.json
```

**2. Start the Worker Nodes (Open in separate terminals):**
```bash
./worker
./worker
./worker
./worker
```

**3. Monitor Live Metrics:**
```bash
curl http://localhost:8080/metrics
```

---

## 📊 Visualizing the DAG

The engine automatically generates `dag.dot` state files during execution. To render the visual graph:
```bash
dot -Tpng dag.dot -o graph_output.png
```