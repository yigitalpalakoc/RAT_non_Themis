// client_agent/src/Agent.cpp
#include "Agent.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── helpers ────────────────────────────────────────────────────────────────

static std::string now() {
    auto tp = std::chrono::system_clock::now();
    auto t  = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

static std::string trim(std::string s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// ── Agent ──────────────────────────────────────────────────────────────────

Agent::Agent() : m_tcp(std::make_unique<TCPClient>()) {}
Agent::~Agent() { stop(); }

bool Agent::initialize(const std::string& configPath, const std::string& clientId) {
    if (!m_lock.acquire()) return false;

    if (!loadConfig()) { m_lock.release(); return false; }
    if (clientId.empty()) {
        std::cerr << "[Agent] Empty client ID\n";
        m_lock.release();
        return false;
    }
    m_clientId = clientId;

    m_tcp->setMessageCallback([this](const std::string& msg) {
        std::cout << "[" << now() << "] Recv: " << msg << "\n";
        logMessage("RECV", msg);
    });
    m_tcp->setConnectionCallback([](bool up) {
        std::cout << (up ? "Connected\n" : "Disconnected\n");
    });

    std::cout << "[Agent] " << m_clientId << " → " << m_host << ":" << m_port << "\n";
    return true;
}

void Agent::start() {
    if (m_running) return;
    m_running = true;
    m_tcp->connect(m_host, m_port, m_clientId);

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)
                       && !std::getenv("RAT_CLIENT_ID");
    if (interactive) {
        if (pipe(m_pipe) == 0) {
            int fl = fcntl(m_pipe[0], F_GETFL, 0);
            fcntl(m_pipe[0], F_SETFL, fl | O_NONBLOCK);
        }
        m_stdinThread = std::thread(&Agent::stdinLoop, this);
        std::cout << "[Agent] Interactive — type messages and press Enter.\n";
    }
}

void Agent::stop() {
    if (!m_running) return;
    std::cout << "[Agent] Stopping...\n";
    m_running = false;

    // Wake the stdin thread out of select(), then join it.
    if (m_pipe[1] != -1) { char x = 0; write(m_pipe[1], &x, 1); }
    if (m_stdinThread.joinable()) m_stdinThread.join();
    for (int& fd : m_pipe) { if (fd != -1) { close(fd); fd = -1; } }

    m_tcp->disconnect();
    m_lock.release();
    std::cout << "[Agent] Stopped\n";
}

void Agent::stdinLoop() {
    fd_set fds;
    while (m_running && !g_shutdown_flag) {
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (m_pipe[0] != -1) FD_SET(m_pipe[0], &fds);
        int nfds = std::max(STDIN_FILENO, m_pipe[0]) + 1;

        if (select(nfds, &fds, nullptr, nullptr, nullptr) <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (m_pipe[0] != -1 && FD_ISSET(m_pipe[0], &fds)) break;

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                if (std::cin.eof()) g_shutdown_flag = 1;
                break;
            }
            line = trim(line);
            if (!line.empty()) sendText(line);
        }
    }
}

void Agent::sendText(const std::string& text) {
    if (!m_tcp->isConnected()) { std::cout << "Not connected.\n"; return; }
    m_tcp->sendMessage(text);
    logMessage("SENT", text);
    std::cout << "[" << now() << "] Sent: " << text << "\n";
}

void Agent::logMessage(const std::string& dir, const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    if (!m_logFile.is_open()) {
        mkdir("logs", 0755);
        m_logFile.open("logs/" + m_clientId + "_messages.log", std::ios::app);
    }
    if (m_logFile.is_open())
        m_logFile << "[" << now() << "] [" << dir << "] " << msg << "\n" << std::flush;
}

bool Agent::loadConfig() {
    std::string p = "./connection_config.json";
    std::ifstream f(p);
    if (!f) return false;
    try {
        auto root = json::parse(f);
        const auto& cfg = root.at("connection_config").at(0);
        m_host = cfg.value("server_ip",   "");
        m_port = cfg.value("server_port",  0);
        if (!m_host.empty() && m_port > 0) {
            std::cout << "[Agent] Config: " << p << "\n";
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Agent] Config error (" << p << "): " << e.what() << "\n";
    }
    std::cerr << "[Agent] No valid server config found\n";
    return false;
}
