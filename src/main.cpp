#include "parser.hpp"
#include "task.hpp"
#include "scheduler.hpp"
#include "tracer.hpp"
#include "httplib.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <optional>
#include <cstdlib>

namespace fs = std::filesystem;

std::mutex state_mtx;
std::mutex sender_mtx;

std::unordered_map<std::string, std::function<void()>> active_callbacks;
std::unordered_map<std::string, std::string> active_task_json;

struct WorkerState {
    std::chrono::steady_clock::time_point last_heartbeat;
    std::string current_task;
};
std::unordered_map<std::string, WorkerState> workers;

// Live Metrics
std::atomic<int> metrics_tasks_completed{0};

// ---------------------------------------------------------------------------
// Safe script-path resolution for the Artifact Server.
//
// A worker sends us a script name. We must NEVER let that name escape the
// scripts directory (e.g. "../../etc/passwd" or an absolute path). We:
//   1. reject absolute paths and any ".." component outright, then
//   2. canonicalize the candidate and verify it is still *inside* the
//      canonical scripts directory.
// Returns the resolved on-disk path, or nullopt if the request is unsafe.
// ---------------------------------------------------------------------------
static std::optional<fs::path> resolve_script_path(const fs::path& base,
                                                   const std::string& requested) {
    if (requested.empty() || requested.size() > 256) return std::nullopt;

    fs::path rp(requested);
    if (rp.is_absolute()) return std::nullopt;
    for (const auto& part : rp) {
        if (part == "..") return std::nullopt;
    }

    std::error_code ec;
    fs::path canon_base = fs::weakly_canonical(base, ec);
    if (ec) return std::nullopt;

    fs::path candidate = fs::weakly_canonical(base / rp, ec);
    if (ec) return std::nullopt;

    fs::path rel = fs::relative(candidate, canon_base, ec);
    if (ec || rel.empty() || *rel.begin() == "..") return std::nullopt;

    return candidate;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " tasks.json\n";
        return 1;
    }

    try {
        // 1. Launch Metrics
        std::thread([]{
            httplib::Server svr;
            svr.Get("/metrics", [](auto&, auto& res){
                std::ostringstream oss;
                int active_workers = 0;
                {
                    std::lock_guard<std::mutex> lock(state_mtx);
                    active_workers = workers.size();
                }
                oss << "# HELP dag_tasks_completed_total Total tasks successfully processed\n";
                oss << "dag_tasks_completed_total " << metrics_tasks_completed.load() << "\n";
                oss << "# HELP dag_active_workers Current number of heartbeating workers\n";
                oss << "dag_active_workers " << active_workers << "\n";
                res.set_content(oss.str(), "text/plain");
            });
            svr.listen("0.0.0.0", 8080);
        }).detach();

        // 2. Load & Sanitize Graph (Offline Processing)
        auto tasks = load_tasks(argv[1]);
        std::cout << "Loaded " << tasks.size() << " tasks. Sanitizing graph...\n";
        sanitize_dag(tasks);

        Tracer tracer;
        tracer.init(tasks);

        // 3. Setup ZeroMQ
        zmq::context_t context(1);
        zmq::socket_t sender(context, ZMQ_PUSH);
        sender.bind("tcp://*:5555");

        zmq::socket_t receiver(context, ZMQ_PULL);
        receiver.bind("tcp://*:5556");

        zmq::socket_t subscriber(context, ZMQ_SUB);
        subscriber.bind("tcp://*:5557");
        subscriber.set(zmq::sockopt::subscribe, "");

        // 3b. Artifact Server (REP on 5558) -------------------------------------
        // Serves raw script bodies to workers on demand. The ZeroMQ *context* is
        // safe to share across threads; the REP *socket* is created and used only
        // inside this thread, which is the rule for ZeroMQ socket thread-safety.
        std::thread artifact_server([&context]() {
            const char* dir_env = std::getenv("SCRIPTS_DIR");
            fs::path scripts_dir = dir_env ? fs::path(dir_env) : fs::path("./scripts");

            zmq::socket_t rep(context, ZMQ_REP);
            rep.bind("tcp://*:5558");
            std::cout << "[ARTIFACT] Serving scripts from " << scripts_dir << " on :5558\n";

            while (true) {
                zmq::message_t request;
                try {
                    if (!rep.recv(request, zmq::recv_flags::none)) continue;
                } catch (const zmq::error_t& e) {
                    if (e.num() == ETERM) return;   // context torn down on shutdown
                    std::cerr << "[ARTIFACT] recv error: " << e.what() << "\n";
                    continue;
                }

                std::string requested(static_cast<char*>(request.data()), request.size());
                std::string status = "OK";
                std::string body;

                auto safe = resolve_script_path(scripts_dir, requested);
                if (!safe) {
                    status = "ERR";
                    body = "rejected unsafe script name: " + requested;
                    std::cerr << "[ARTIFACT] \033[1;31mREJECTED\033[0m path: "
                              << requested << "\n";
                } else {
                    std::ifstream f(*safe, std::ios::binary);
                    if (!f) {
                        status = "ERR";
                        body = "script not found: " + requested;
                    } else {
                        std::ostringstream ss;
                        ss << f.rdbuf();
                        body = ss.str();
                    }
                }

                // Reply is always a 2-frame message: [status][body].
                zmq::message_t status_frame(status.size());
                memcpy(status_frame.data(), status.data(), status.size());
                zmq::message_t body_frame(body.size());
                memcpy(body_frame.data(), body.data(), body.size());

                try {
                    rep.send(status_frame, zmq::send_flags::sndmore);
                    rep.send(body_frame, zmq::send_flags::none);
                } catch (const zmq::error_t& e) {
                    if (e.num() == ETERM) return;
                    std::cerr << "[ARTIFACT] send error: " << e.what() << "\n";
                }
            }
        });
        artifact_server.detach();
        // -----------------------------------------------------------------------

        std::cout << "Master ready. Waiting 3 seconds for workers to connect...\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 4. Heartbeat Listener
        std::thread hb_listener([&]() {
            try {
                while (true) {
                    zmq::message_t msg;
                    if (subscriber.recv(msg, zmq::recv_flags::none)) {
                        auto j = nlohmann::json::parse(std::string(static_cast<char*>(msg.data()), msg.size()));
                        std::string wid = j["worker_id"];
                        std::lock_guard<std::mutex> lock(state_mtx);
                        workers[wid].last_heartbeat = std::chrono::steady_clock::now();
                        workers[wid].current_task = (j["status"] == "working") ? j["task_id"].get<std::string>() : "";
                    }
                }
            } catch (const zmq::error_t&) { }
        });
        hb_listener.detach();

        // 5. Watchdog Reaper
        std::thread reaper([&]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto now = std::chrono::steady_clock::now();

                std::lock_guard<std::mutex> lock(state_mtx);
                for (auto it = workers.begin(); it != workers.end(); ) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_heartbeat).count() >= 5) {
                        std::cout << "\n[WATCHDOG] \033[1;31mWorker " << it->first << " TIMED OUT!\033[0m\n";
                        std::string lost_task = it->second.current_task;

                        if (!lost_task.empty() && active_task_json.count(lost_task)) {
                            std::string dump = active_task_json[lost_task];
                            zmq::message_t msg(dump.size());
                            memcpy(msg.data(), dump.c_str(), dump.size());
                            std::lock_guard<std::mutex> send_lock(sender_mtx);
                            sender.send(msg, zmq::send_flags::none);
                        }
                        it = workers.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        });
        reaper.detach();

        // 6. Completion Receiver
        std::thread receiver_thread([&]() {
            try {
                while (true) {
                    zmq::message_t msg;
                    if (receiver.recv(msg, zmq::recv_flags::none)) {
                        auto j = nlohmann::json::parse(std::string(static_cast<char*>(msg.data()), msg.size()));
                        std::string task_id = j["id"];
                        std::string status = j.value("status", "finished");

                        if (status == "failed") {
                            // The scheduler has no notion of failure, so we still
                            // unblock dependents to avoid stalling the DAG, but we
                            // surface the error loudly. (See README note on this.)
                            std::cerr << "[\033[1;31mTASK FAILED\033[0m] " << task_id
                                      << " (" << j.value("error", "no detail")
                                      << ") - unblocking dependents anyway\n";
                        }

                        tracer.set_state(task_id, TaskState::Finished);
                        metrics_tasks_completed++;

                        std::function<void()> cb;
                        {
                            std::lock_guard<std::mutex> lock(state_mtx);
                            cb = active_callbacks[task_id];
                            active_callbacks.erase(task_id);
                            active_task_json.erase(task_id);
                        }
                        if (cb) cb();
                    }
                }
            } catch (const zmq::error_t& e) {
                if (e.num() == ETERM) {
                    // Expected shutdown, exit quietly
                    return;
                } else {
                    // Something unexpected went wrong!
                    std::cerr << "ZeroMQ Error in background thread: " << e.what() << "\n";
                }
            }
        });
        receiver_thread.detach();

        // 7. Execute DAG
        auto t0 = std::chrono::steady_clock::now();
        std::cout << "Dispatching tasks...\n";

        run_scheduler(tasks, [&](const Task& t, auto on_complete) {
            tracer.set_state(t.id, TaskState::Running);
            // The dispatch payload now carries the full stateless-artifact schema
            // so the worker knows which runtime + script to fetch and execute.
            std::string dump = nlohmann::json{
                {"id", t.id},
                {"priority", t.priority},
                {"runtime", t.runtime},
                {"script_name", t.script_name},
                {"pkg_deps", t.pkg_deps}      // <-- workers need this to build the venv
            }.dump();

            {
                std::lock_guard<std::mutex> lock(state_mtx);
                active_callbacks[t.id] = on_complete;
                active_task_json[t.id] = dump;
            }

            zmq::message_t msg(dump.size());
            memcpy(msg.data(), dump.c_str(), dump.size());
            std::lock_guard<std::mutex> send_lock(sender_mtx);
            sender.send(msg, zmq::send_flags::none);
        });

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "\n============================================\n";
        std::cout << "All " << tasks.size() << " tasks processed! Total time: " << ms << " ms\n";
        std::cout << "============================================\n";

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << "\n";
        return 2;
    }
    return 0;
}