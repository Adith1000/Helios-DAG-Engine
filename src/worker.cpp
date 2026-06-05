#include <zmq.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <random>
#include <vector>
#include <unordered_map>
#include <functional>
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

// ---------------------------------------------------------------------------
// Runtime strategy table (the extensible "Strategy Pattern").
//
// Each runtime declares:
//   * supports_pkgs : whether pkg_deps can be installed for it
//   * setup(scratch, deps) -> ordered list of argv commands to run (cwd=scratch)
//   * exec(scratch, script) -> the argv that actually runs the script (cwd=scratch)
//
// To add Ruby, Go, etc. later, you only add one entry here. Nothing else in
// the pipeline changes.
// ---------------------------------------------------------------------------
struct RuntimeStrategy {
    bool supports_pkgs = false;
    std::function<std::vector<std::vector<std::string>>(const fs::path&,
                                                        const std::vector<std::string>&)> setup;
    std::function<std::vector<std::string>(const fs::path&, const fs::path&)> exec;
};

static const std::unordered_map<std::string, RuntimeStrategy>& runtime_table() {
    static const std::unordered_map<std::string, RuntimeStrategy> table = [] {
        std::unordered_map<std::string, RuntimeStrategy> m;

        // ---- python3 : venv + pip -------------------------------------------
        RuntimeStrategy py;
        py.supports_pkgs = true;
        py.setup = [](const fs::path& scratch, const std::vector<std::string>& deps) {
            std::vector<std::vector<std::string>> cmds;
            // python3 -m venv venv
            cmds.push_back({"python3", "-m", "venv", "venv"});
            // <scratch>/venv/bin/pip install --no-input <deps...>
            std::vector<std::string> pip = {
                (scratch / "venv" / "bin" / "pip").string(), "install", "--no-input"
            };
            pip.insert(pip.end(), deps.begin(), deps.end());
            cmds.push_back(std::move(pip));
            return cmds;
        };
        py.exec = [](const fs::path& scratch, const fs::path& script) {
            // Use the venv interpreter if it exists; otherwise the system one.
            fs::path venv_py = scratch / "venv" / "bin" / "python3";
            std::error_code ec;
            std::string interp = fs::exists(venv_py, ec) ? venv_py.string()
                                                         : std::string("python3");
            return std::vector<std::string>{interp, script.string()};
        };
        m["python3"] = py;
        m["python"]  = py;

        // ---- node : npm + local node_modules --------------------------------
        RuntimeStrategy node;
        node.supports_pkgs = true;
        node.setup = [](const fs::path& /*scratch*/, const std::vector<std::string>& deps) {
            std::vector<std::vector<std::string>> cmds;
            cmds.push_back({"npm", "init", "-y"});
            std::vector<std::string> inst = {"npm", "install", "--no-audit", "--no-fund"};
            inst.insert(inst.end(), deps.begin(), deps.end());
            cmds.push_back(std::move(inst));
            return cmds;
        };
        node.exec = [](const fs::path& /*scratch*/, const fs::path& script) {
            // Run with cwd=scratch so node resolves the local node_modules.
            return std::vector<std::string>{"node", script.string()};
        };
        m["node"]   = node;
        m["nodejs"] = node;

        // ---- bash / sh : no package manager ---------------------------------
        RuntimeStrategy bash;
        bash.supports_pkgs = false;
        bash.exec = [](const fs::path&, const fs::path& script) {
            return std::vector<std::string>{"bash", script.string()};
        };
        m["bash"] = bash;

        RuntimeStrategy sh = bash;
        sh.exec = [](const fs::path&, const fs::path& script) {
            return std::vector<std::string>{"sh", script.string()};
        };
        m["sh"] = sh;

        return m;
    }();
    return table;
}

// Generate a random ID for this worker so the Master can track who is who
std::string generate_worker_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "W-" + std::to_string(dis(gen));
}

// script_name must be a single plain filename (no separators, no "..").
static bool is_safe_basename(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

// Package names go into argv (never a shell), so shell metacharacters cannot
// inject. This allowlist is defence-in-depth and ALSO blocks argument
// injection: a leading '-' (e.g. "--upgrade", "-rfile") could otherwise be
// read as a flag by pip/npm. We permit the characters real specs need
// (versions like "pandas>=2.0", extras "pkg[x]", scoped "@scope/pkg") while
// excluding ; & | $ ` ( ) { } * ? quotes and whitespace.
static bool is_safe_pkg_name(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    if (s.front() == '-') return false;                       // no flag injection
    if (s.find("..") != std::string::npos) return false;      // no local-path tricks
    static const std::string allowed =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "._-+=<>~![],@/";
    for (unsigned char c : s) {
        if (allowed.find(static_cast<char>(c)) == std::string::npos) return false;
    }
    return true;
}

// Run argv[0] with the given arguments, no shell, optionally in `cwd`.
// argv buffers are built in the parent so the child only calls async-signal-
// safe functions (chdir/execvp) after fork(). Returns the exit code or -1.
static int run_process(const std::vector<std::string>& args, const fs::path& cwd) {
    if (args.empty()) return -1;

    std::vector<std::string> storage = args;     // owns the argv strings
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    std::string cwd_str = cwd.string();

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!cwd_str.empty() && chdir(cwd_str.c_str()) != 0) _exit(126);
        execvp(argv[0], argv.data());
        _exit(127);                              // exec failed (binary missing)
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int main() {
    std::string worker_id = generate_worker_id();

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
    zmq::socket_t artifact_req(context, ZMQ_REQ);
    artifact_req.connect("tcp://" + master_ip + ":5558");

    std::cout << "[" << worker_id << "] Started against master '" << master_ip
              << "'. Waiting for tasks...\n";

    // --- The Heartbeat Thread ---
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

    // --- The Polyglot JIT Execution Loop ---
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

        std::vector<std::string> pkg_deps;
        if (j.contains("pkg_deps") && j["pkg_deps"].is_array()) {
            for (auto& d : j["pkg_deps"])
                if (d.is_string()) pkg_deps.push_back(d.get<std::string>());
        }

        {
            std::lock_guard<std::mutex> lock(state_mtx);
            current_task_id = task_id;
            is_working = true;
        }
        std::cout << "[" << worker_id << "] Task " << task_id
                  << " runtime=" << runtime << " script=" << script_name
                  << " deps=" << pkg_deps.size() << "\n";

        bool ok = false;
        std::string failure_reason;

        const auto& tbl = runtime_table();
        auto strat_it = tbl.find(runtime);

        if (strat_it == tbl.end()) {
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

            // ---- 2. Materialise into a unique scratch dir, set up env, run ----
            if (fetched) {
                char tmpl[] = "/tmp/helios-XXXXXX";
                char* tmpdir = mkdtemp(tmpl);
                if (!tmpdir) {
                    failure_reason = "could not create temp dir";
                } else {
                    const fs::path scratch(tmpdir);
                    const fs::path script_path = scratch / script_name;
                    {
                        std::ofstream out(script_path, std::ios::binary);
                        out.write(body.data(), static_cast<std::streamsize>(body.size()));
                    }

                    const RuntimeStrategy& strat = strat_it->second;
                    bool setup_ok = true;

                    // ---- Dependency resolution (Polyglot) ----
                    if (!pkg_deps.empty()) {
                        if (!strat.supports_pkgs || !strat.setup) {
                            setup_ok = false;
                            failure_reason = "runtime '" + runtime + "' does not support pkg_deps";
                        } else {
                            for (const auto& d : pkg_deps) {
                                if (!is_safe_pkg_name(d)) {
                                    setup_ok = false;
                                    failure_reason = "rejected unsafe package name: " + d;
                                    break;
                                }
                            }
                            if (setup_ok) {
                                for (const auto& cmd : strat.setup(scratch, pkg_deps)) {
                                    int rc = run_process(cmd, scratch);
                                    if (rc != 0) {
                                        setup_ok = false;
                                        failure_reason = "dependency install failed ('" +
                                                         cmd.front() + "', rc " + std::to_string(rc) + ")";
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    // ---- Execute the target script ----
                    if (setup_ok) {
                        const auto exec_argv = strat.exec(scratch, script_path);
                        int code = run_process(exec_argv, scratch);
                        ok = (code == 0);
                        if (!ok) failure_reason = "exit code " + std::to_string(code);
                    }

                    std::error_code ec;
                    fs::remove_all(tmpdir, ec);   // delete the whole scratch dir
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_mtx);
            is_working = false;
            current_task_id = "";
        }

        // ---- Report status back to the Master ----
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