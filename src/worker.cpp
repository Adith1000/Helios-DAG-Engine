#include <zmq.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <random>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// Shared state between the main worker thread and the heartbeat thread
std::mutex state_mtx;
std::string current_task_id = "";
bool is_working = false;

// Whitelist of accepted runtimes -> the interpreter binary we exec.
// Anything not in this map is rejected: we never feed an arbitrary
// "runtime" string to the system.
static const std::unordered_map<std::string, std::string> RUNTIMES = {
    {"python3", "python3"},
    {"python",  "python3"},
    {"node",    "node"},
    {"nodejs",  "node"},
    {"bash",    "bash"},
    {"sh",      "sh"},
};

// Generate a random ID for this worker so the Master can track who is who
std::string generate_worker_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "W-" + std::to_string(dis(gen));
}

// A script_name must be a single, plain filename. No path separators, no
// "..", no shell metacharacters. This protects both the temp-file path we
// write and (defence in depth) anything downstream that handles the name.
static bool is_safe_basename(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

// Execute `interp script_path` WITHOUT a shell. Using fork()/execvp() with an
// argument vector means the script name can never be interpreted as a command
// (no injection possible, unlike std::system / "/bin/sh -c").
// Returns the child's exit code, or -1 if it could not be run.
static int run_script(const std::string& interp, const fs::path& script_path) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // --- child ---
        std::string interp_copy = interp;          // execvp wants mutable buffers
        std::string path_copy   = script_path.string();
        std::vector<char*> argv;
        argv.push_back(interp_copy.data());
        argv.push_back(path_copy.data());
        argv.push_back(nullptr);
        execvp(interp_copy.c_str(), argv.data());
        _exit(127);                                // exec failed
    }

    // --- parent ---
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int main() {
    std::string worker_id = generate_worker_id();

    // Resolve the master location once. In containers this is set to "master".
    const char* master_env = std::getenv("MASTER_IP");
    std::string master_ip = (master_env && *master_env) ? master_env : "localhost";

    zmq::context_t context(1);

    // 1) PULL: Receive tasks from Master
    zmq::socket_t receiver(context, ZMQ_PULL);
    receiver.connect("tcp://" + master_ip + ":5555");

    // 2) PUSH: Send completions to Master
    zmq::socket_t sender(context, ZMQ_PUSH);
    sender.connect("tcp://" + master_ip + ":5556");

    // 3) PUB: Broadcast heartbeats
    zmq::socket_t publisher(context, ZMQ_PUB);
    publisher.connect("tcp://" + master_ip + ":5557");

    // 4) REQ: Fetch script artifacts from the Master's artifact server.
    //    Used only by this (main) thread -> safe single-threaded socket use.
    zmq::socket_t artifact_req(context, ZMQ_REQ);
    artifact_req.connect("tcp://" + master_ip + ":5558");

    std::cout << "[" << worker_id << "] Started against master '" << master_ip
              << "'. Waiting for tasks...\n";

    // --- The Heartbeat Thread (unchanged behaviour) ---
    std::thread heartbeat_thread([&]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            nlohmann::json hb;
            hb["worker_id"] = worker_id;
            {
                std::lock_guard<std::mutex> lock(state_mtx);
                if (is_working) {
                    hb["status"]  = "working";
                    hb["task_id"] = current_task_id;
                } else {
                    hb["status"] = "idle";
                }
            }

            std::string hb_str = hb.dump();
            zmq::message_t msg(hb_str.size());
            memcpy(msg.data(), hb_str.c_str(), hb_str.size());
            publisher.send(msg, zmq::send_flags::none);
        }
    });
    heartbeat_thread.detach();

    // --- The Stateless JIT Execution Loop ---
    while (true) {
        zmq::message_t task_msg;
        auto res = receiver.recv(task_msg, zmq::recv_flags::none);
        if (!res) continue;

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(
                std::string(static_cast<char*>(task_msg.data()), task_msg.size()));
        } catch (const std::exception& e) {
            std::cerr << "[" << worker_id << "] Bad task JSON: " << e.what() << "\n";
            continue;
        }

        const std::string task_id     = j.value("id", "");
        const std::string runtime     = j.value("runtime", "");
        const std::string script_name = j.value("script_name", "");

        // Flip to "working" so the heartbeat thread reports it.
        {
            std::lock_guard<std::mutex> lock(state_mtx);
            current_task_id = task_id;
            is_working = true;
        }
        std::cout << "[" << worker_id << "] Task " << task_id
                  << " runtime=" << runtime << " script=" << script_name << "\n";

        bool ok = false;
        std::string failure_reason;

        auto rit = RUNTIMES.find(runtime);
        if (rit == RUNTIMES.end()) {
            failure_reason = "unsupported runtime: " + runtime;
        } else if (!is_safe_basename(script_name)) {
            failure_reason = "unsafe script name: " + script_name;
        } else {
            // ---- 1. Fetch the script body from the artifact server ----
            std::string body;
            bool fetched = false;
            try {
                zmq::message_t req(script_name.size());
                memcpy(req.data(), script_name.data(), script_name.size());
                artifact_req.send(req, zmq::send_flags::none);

                zmq::message_t status_msg;
                if (!artifact_req.recv(status_msg, zmq::recv_flags::none)) {
                    failure_reason = "artifact server: no reply";
                } else {
                    std::string status(static_cast<char*>(status_msg.data()), status_msg.size());

                    std::string fetched_body;
                    if (artifact_req.get(zmq::sockopt::rcvmore)) {
                        zmq::message_t body_msg;
                        if (artifact_req.recv(body_msg, zmq::recv_flags::none)) {
                            fetched_body.assign(static_cast<char*>(body_msg.data()), body_msg.size());
                        }
                    }

                    if (status == "OK") {
                        body = std::move(fetched_body);
                        fetched = true;
                    } else {
                        failure_reason = "artifact server: " + fetched_body;
                    }
                }
            } catch (const zmq::error_t& e) {
                failure_reason = std::string("artifact fetch failed: ") + e.what();
            }

            // ---- 2-5. Materialise, execute, clean up ----
            if (fetched) {
                // Unique per-task scratch dir avoids collisions / symlink races
                // that a fixed /tmp/<name> path would expose us to.
                char tmpl[] = "/tmp/helios-XXXXXX";
                char* tmpdir = mkdtemp(tmpl);
                if (!tmpdir) {
                    failure_reason = "could not create temp dir";
                } else {
                    fs::path script_path = fs::path(tmpdir) / script_name;
                    {
                        std::ofstream out(script_path, std::ios::binary);
                        out.write(body.data(), static_cast<std::streamsize>(body.size()));
                    }

                    int code = run_script(rit->second, script_path);
                    ok = (code == 0);
                    if (!ok) failure_reason = "exit code " + std::to_string(code);

                    std::error_code ec;
                    fs::remove_all(tmpdir, ec);   // delete the whole scratch dir
                }
            }
        }

        // Back to idle before reporting.
        {
            std::lock_guard<std::mutex> lock(state_mtx);
            is_working = false;
            current_task_id = "";
        }

        // ---- 6. Report status back to the Master ----
        nlohmann::json complete_j = {
            {"id", task_id},
            {"status", ok ? "finished" : "failed"}
        };
        if (!ok && !failure_reason.empty()) complete_j["error"] = failure_reason;

        std::string complete_str = complete_j.dump();
        zmq::message_t reply(complete_str.size());
        memcpy(reply.data(), complete_str.c_str(), complete_str.size());
        sender.send(reply, zmq::send_flags::none);

        if (!ok)
            std::cerr << "[" << worker_id << "] Task " << task_id
                      << " FAILED: " << failure_reason << "\n";
        else
            std::cout << "[" << worker_id << "] Task " << task_id << " finished.\n";
    }

    return 0;
}