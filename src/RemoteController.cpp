// remote_access_tool/src/RemoteController.cpp

#include "RemoteController.hpp"
#include "SSHManager.hpp"
#include "ShellManager.hpp"
#include "SCPManager.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

static constexpr int SERVER_PORT = 2222;

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

static std::string getLocalIP() {
    struct ifaddrs* ifaddr;
    std::string ip = "127.0.0.1";
    if (getifaddrs(&ifaddr) == -1) return ip;
    for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        char addr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr,
                  addr, sizeof(addr));
        if (std::string(addr) != "127.0.0.1") { ip = addr; break; }
    }
    freeifaddrs(ifaddr);
    return ip;
}

// Fork a shell command with a wall-clock timeout; returns the wait status.
static int timed_system(const char* cmd, int timeout_sec) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd, nullptr); _exit(127); }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    struct timespec ts{0, 20'000'000};
    while (true) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) return status;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -1;
        }
        nanosleep(&ts, nullptr);
        if (ts.tv_nsec < 100'000'000)
            ts.tv_nsec = std::min(ts.tv_nsec * 2, 100'000'000L);
    }
}

static bool writeAll(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return false;
        buf += n; len -= n;
    }
    return true;
}

// ── RemoteController ───────────────────────────────────────────────────────

RemoteController::RemoteController()  {}
RemoteController::~RemoteController() { stopTCPServer(); SSHManager::getInstance().killAllSessions(); }

void RemoteController::logMessage(const std::string& clientId,
                                   const std::string& dir,
                                   const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    auto it = m_logs.find(clientId);
    if (it == m_logs.end()) {
        mkdir("logs", 0755);
        m_logs[clientId].open("logs/" + clientId + "_messages.log", std::ios::app);
        it = m_logs.find(clientId);
    }
    if (it->second.is_open())
        it->second << "[" << now() << "] [" << dir << "] " << msg << "\n" << std::flush;
}

bool RemoteController::loadClients(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file) { std::cerr << "Cannot open config: " << configPath << "\n"; return false; }

    nlohmann::json root;
    try { file >> root; }
    catch (const nlohmann::json::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n"; return false;
    }

    if (!root.contains("clients") || !root["clients"].is_array()) {
        std::cerr << "Missing 'clients' array in config\n"; return false;
    }

    m_clients.clear();
    for (const auto& j : root["clients"]) {
        Client c; c.loadFromJson(j); m_clients.push_back(c);
    }
    std::cout << "Loaded " << m_clients.size() << " clients\n";
    return true;
}

void RemoteController::startTCPServer() {
    m_serverIp  = getLocalIP();
    m_tcpHandler = std::make_unique<TCPHandler>(
        SERVER_PORT,
        [this](const std::string& clientId, const std::string& msg) {
            std::cout << "[" << now() << "] Recv from " << clientId << ": " << msg << "\n";
            logMessage(clientId, "RECV", msg);
        },
        [this](const std::string& clientId, bool connected) {
            Client* client = getClientById(clientId);
            if (!client) {
                if (connected && m_tcpHandler) m_tcpHandler->disconnectClient(clientId);
                return;
            }
            client->setConnected(connected);
            std::cout << "[" << now() << "] Client " << clientId
                      << (connected ? " connected" : " disconnected") << "\n";
        }
    );
    m_tcpHandler->setValidationCallback([this](const std::string& id) {
        return getClientById(id) != nullptr;
    });

    if (!m_tcpHandler->start())
        std::cerr << "Failed to start TCP server on port " << SERVER_PORT << "\n";
    else
        std::cout << "TCP server listening on " << m_serverIp << ":" << SERVER_PORT << "\n";
}

void RemoteController::stopTCPServer() {
    if (m_tcpHandler) { m_tcpHandler->stop(); m_tcpHandler.reset(); }
}

void RemoteController::pushConfigToClients(const std::string& serverIp) {
    std::string ip = serverIp.empty() ? getLocalIP() : serverIp;
    std::cout << "Using local IP: " << ip << "\n";

    nlohmann::json root;
    root["connection_config"] = nlohmann::json::array({
        { {"server_ip", ip}, {"server_port", SERVER_PORT} }
    });
    std::string json_str = root.dump(2);

    char tmp[] = "/tmp/rat_config_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { std::cerr << "mkstemp failed\n"; return; }
    if (!writeAll(fd, json_str.c_str(), json_str.size())) {
        std::cerr << "Write to temp file failed\n"; close(fd); unlink(tmp); return;
    }
    close(fd);

    for (const auto& client : m_clients) {
        std::string remote = "/home/" + client.getUser() + "/connection_config.json";
        std::cout << "Pushing config to " << client.getId() << "\n";
        SCPManager::getInstance().uploadFile(client, tmp, remote, false);
    }
    unlink(tmp);
    std::cout << "Config pushed to all clients.\n";
}

void RemoteController::pushAgentBinaryToClients() {
    const std::string binary = "../client_agent/build/client_agent";
    if (access(binary.c_str(), F_OK) != 0) {
        std::cerr << "Warning: client_agent not found at " << binary << " — skipping.\n";
        return;
    }

    const std::string sshBase =
        "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
        " -i " + SSHManager::getInstance().getSSHKeyPath();

    // Step 1 — kill any running agent on all clients in parallel.
    {
        std::vector<std::thread> threads;
        threads.reserve(m_clients.size());
        for (const auto& client : m_clients) {
            std::ostringstream cmd;
            cmd << sshBase << " -p " << client.getPort() << " " << client.getSSHTarget()
                << " \"pkill -x client_agent 2>/dev/null; sleep 0.3\"";
            std::string s = cmd.str();
            threads.emplace_back([s]() { timed_system(s.c_str(), 5); });
        }
        for (auto& t : threads) t.join();
    }

    // Step 2 — upload the binary.
    for (const auto& client : m_clients) {
        std::string remote = "/home/" + client.getUser() + "/client_agent";
        std::cout << "Uploading client_agent to " << client.getId() << "\n";
        SCPManager::getInstance().uploadFile(client, binary, remote, false);
    }

    // Step 3 — start the agent on all clients in parallel.
    {
        std::vector<std::thread> threads;
        threads.reserve(m_clients.size());
        for (const auto& client : m_clients) {
            std::string remote = "/home/" + client.getUser() + "/client_agent";
            std::string start  = "chmod +x " + remote + " && cd /home/" + client.getUser() +
                                  " && RAT_CLIENT_ID=" + client.getId() +
                                  " nohup ./client_agent >/dev/null 2>&1 </dev/null & sleep 0.5";
            std::ostringstream cmd;
            cmd << sshBase << " -p " << client.getPort() << " " << client.getSSHTarget()
                << " \"" << start << "\"";
            std::cout << "Starting client_agent on " << client.getId() << "\n";
            std::string s  = cmd.str();
            std::string id = client.getId();
            threads.emplace_back([s, id]() {
                int ret = timed_system(s.c_str(), 5);
                if (ret == -1)
                    std::cerr << "Timeout launching agent on " << id << "\n";
                else if (WIFEXITED(ret) && WEXITSTATUS(ret) != 0 && WEXITSTATUS(ret) != 2)
                    std::cerr << "Agent launch failed (exit " << WEXITSTATUS(ret)
                              << ") on " << id << "\n";
            });
        }
        for (auto& t : threads) t.join();
    }
    std::cout << "Agent binaries pushed and started.\n";
}

// ── command dispatch ───────────────────────────────────────────────────────

void RemoteController::parseAndExecute(const std::string& line) {
    if      (line.find("run ")    == 0) handleRunCommandOriginal(line);
    else if (line.find("scp ")    == 0) handleScpCommandOriginal(line);
    else if (line.find("shell ")  == 0) handleShellCommandOriginal(line);
    else if (line.find("msg ")    == 0) handleMsgCommand(line);
    else if (line.find("server ") == 0) handlePluginCommand(line.substr(7));
    else
        std::cout << "Unknown command. Type 'help' for available commands.\n";
}

void RemoteController::handleMsgCommand(const std::string& line) {
    std::string rest = trim(line.substr(4));   // everything after "msg "

    if (rest.find("all ") == 0) {
        std::string text = trim(rest.substr(4));
        if (text.empty()) { std::cout << "Usage: msg all <text>\n"; return; }
        cmdBroadcast(text);
    } else if (rest.find("tag ") == 0) {
        std::string tc = rest.substr(4);
        size_t sp = tc.find(' ');
        if (sp == std::string::npos) { std::cout << "Usage: msg tag <tag> <text>\n"; return; }
        cmdTag(tc.substr(0, sp), trim(tc.substr(sp + 1)));
    } else {
        size_t sp = rest.find(' ');
        if (sp == std::string::npos) { std::cout << "Usage: msg <id> <text>\n"; return; }
        cmdMsg(rest.substr(0, sp), trim(rest.substr(sp + 1)));
    }
}

void RemoteController::handleRunCommandOriginal(const std::string& line) {
    std::string rest = line.substr(4);
    if (rest.find("tag ") == 0) {
        std::string tc = rest.substr(4);
        size_t sp = tc.find(' ');
        if (sp == std::string::npos) { std::cout << "Usage: run tag <tag> <cmd>\n"; return; }
        executeCommandByTag(tc.substr(0, sp), trim(tc.substr(sp + 1)));
    } else if (rest.find("all ") == 0) {
        executeCommandOnAll(trim(rest.substr(4)));
    } else {
        size_t sp = rest.find(' ');
        if (sp == std::string::npos) { std::cout << "Usage: run <id> <cmd>\n"; return; }
        executeCommand(rest.substr(0, sp), trim(rest.substr(sp + 1)));
    }
}

void RemoteController::handleScpCommandOriginal(const std::string& line) {
    std::string rest = line.substr(4);
    if (rest.find("get ") == 0) {
        auto t = tokenize(rest.substr(4));
        if (t.size() < 3) { std::cout << "Usage: scp get <id> <remote> <local>\n"; return; }
        downloadFile(t[0], t[1], t[2]);
    } else if (rest.find("tag ") == 0) {
        auto t = tokenize(rest.substr(4));
        if (t.size() < 3) { std::cout << "Usage: scp tag <tag> <local> <remote>\n"; return; }
        SCPManager::getInstance().uploadToTagged(m_clients, t[0], t[1], t[2]);
    } else if (rest.find("all ") == 0) {
        auto t = tokenize(rest.substr(4));
        if (t.size() < 2) { std::cout << "Usage: scp all <local> <remote>\n"; return; }
        SCPManager::getInstance().uploadToAll(m_clients, t[0], t[1]);
    } else {
        auto t = tokenize(rest);
        if (t.size() < 3) { std::cout << "Usage: scp <id> <local> <remote>\n"; return; }
        uploadFile(t[0], t[1], t[2]);
    }
}

void RemoteController::handleShellCommandOriginal(const std::string& line) {
    std::string id = trim(line.substr(6));
    if (id.empty()) { std::cout << "Usage: shell <id>\n"; return; }
    openInteractiveShell(id);
}

void RemoteController::handlePluginCommand(const std::string& line) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return;

    if (tokens[0] == "status") {
        cmdStatus();
    } else {
        std::cout << "Unknown server command. Type 'help'.\n";
    }
}

// ── server commands ────────────────────────────────────────────────────────

void RemoteController::cmdStatus() {
    std::cout << "\n=== Server Status ===\n"
              << "Server: " << m_serverIp << ":" << SERVER_PORT << "\n"
              << "Total clients: " << m_clients.size() << "\n";
    if (m_tcpHandler) {
        auto connected = m_tcpHandler->getConnectedClients();
        std::cout << "Connected: " << connected.size() << "\n";
        for (const auto& c : m_clients) {
            bool up = std::find(connected.begin(), connected.end(), c.getId()) != connected.end();
            std::cout << "  • " << c.getDisplayName() << (up ? " [CONNECTED]" : " [DISCONNECTED]") << "\n";
        }
    }
}

void RemoteController::cmdMsg(const std::string& clientId, const std::string& text) {
    if (!m_tcpHandler) { std::cerr << "TCP handler not available\n"; return; }
    std::cout << "[" << now() << "] Send to " << clientId << ": " << text << "\n";
    logMessage(clientId, "SENT", text);
    m_tcpHandler->sendToClient(clientId, text);
}

void RemoteController::cmdBroadcast(const std::string& text) {
    if (!m_tcpHandler) { std::cerr << "TCP handler not available\n"; return; }
    m_tcpHandler->broadcastToAll(text);
    std::cout << "Broadcast sent.\n";
}

void RemoteController::cmdTag(const std::string& tag, const std::string& text) {
    if (!m_tcpHandler) { std::cerr << "TCP handler not available\n"; return; }
    int sent = 0;
    for (auto& c : m_clients)
        if (c.hasTag(tag) && c.isConnected() && m_tcpHandler->sendToClient(c.getId(), text))
            sent++;
    std::cout << "Sent to " << sent << " clients with tag '" << tag << "'\n";
}

// ── client access ──────────────────────────────────────────────────────────

void RemoteController::executeCommand(const std::string& id, const std::string& cmd) {
    Client* c = getClientById(id);
    if (!c) { std::cout << "Client '" << id << "' not found\n"; return; }
    SSHManager::getInstance().executeCommand(*c, cmd, true);
}

void RemoteController::executeCommandByTag(const std::string& tag, const std::string& cmd) {
    bool found = false;
    for (const auto& c : m_clients)
        if (c.hasTag(tag)) { SSHManager::getInstance().executeCommand(c, cmd, true); found = true; }
    if (!found) std::cout << "No clients with tag '" << tag << "'\n";
}

void RemoteController::executeCommandOnAll(const std::string& cmd) {
    for (const auto& c : m_clients)
        SSHManager::getInstance().executeCommand(c, cmd, true);
}

void RemoteController::uploadFile(const std::string& id, const std::string& local,
                                   const std::string& remote) {
    Client* c = getClientById(id);
    if (!c) { std::cout << "Client '" << id << "' not found\n"; return; }
    SCPManager::getInstance().uploadFile(*c, local, remote, false);
}

void RemoteController::downloadFile(const std::string& id, const std::string& remote,
                                     const std::string& local) {
    Client* c = getClientById(id);
    if (!c) { std::cout << "Client '" << id << "' not found\n"; return; }
    SCPManager::getInstance().downloadFile(*c, remote, local);
}

void RemoteController::openInteractiveShell(const std::string& id) {
    Client* c = getClientById(id);
    if (!c) { std::cout << "Client '" << id << "' not found\n"; return; }
    ShellManager::getInstance().executeInteractiveShell(*c);
}

Client* RemoteController::getClientById(const std::string& id) {
    for (auto& c : m_clients) if (c.getId() == id) return &c;
    return nullptr;
}

std::vector<Client> RemoteController::getClientsByTag(const std::string& tag) const {
    std::vector<Client> result;
    for (const auto& c : m_clients) if (c.hasTag(tag)) result.push_back(c);
    return result;
}

// ── string utilities ───────────────────────────────────────────────────────

std::string RemoteController::trim(const std::string& s) const {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

std::vector<std::string> RemoteController::tokenize(const std::string& str, char delim) const {
    std::vector<std::string> out;
    std::string tok;
    std::istringstream ss(str);
    while (std::getline(ss, tok, delim)) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

bool RemoteController::isBlank(const std::string& s) const {
    return std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c); });
}

void RemoteController::printHelp() const {
    std::cout <<
        "============================================================\n"
        "                 Remote SSH/SCP Controller\n"
        "============================================================\n"
        "  run <id> <cmd>                 Execute on remote (async)\n"
        "  run tag <tag> <cmd>            Execute on all with tag (async)\n"
        "  run all <cmd>                  Execute on all clients (async)\n"
        "\n"
        "  scp <id> <local> <remote>      Upload local → remote (sync)\n"
        "  scp get <id> <remote> <local>  Download remote → local (sync)\n"
        "  scp tag <tag> <local> <remote> Upload to all with tag (async)\n"
        "  scp all <local> <remote>       Upload to all clients (async)\n"
        "\n"
        "  shell <id>                     Interactive SSH shell (sync)\n"
        "\n"
        "  msg <id> <text>                Send message to a client\n"
        "  msg tag <tag> <text>           Send to all with tag\n"
        "  msg all <text>                 Broadcast to all connected\n"
        "\n"
        "  server status                  Show server/client status\n"
        "  help / ?                       Show this help\n"
        "  quit / exit                    Quit\n"
        "============================================================\n"
        "Clients: " << m_clients.size() << "  Port: " << SERVER_PORT << "\n";
}
