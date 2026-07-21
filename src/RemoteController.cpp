// remote_access_tool/src/RemoteController.cpp
#include "RemoteController.hpp"
#include "SSHManager.hpp"
#include "ShellManager.hpp"
#include "SCPManager.hpp"
#include "MessageHandler.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

static constexpr int SERVER_PORT = 2222;

RemoteController::RemoteController() {}

RemoteController::~RemoteController() {
    stopTCPServer();
    SSHManager::getInstance().killAllSessions();
}

static std::string getLocalIP() {
    struct ifaddrs *ifaddr, *ifa;
    std::string ip = "127.0.0.1";
    if (getifaddrs(&ifaddr) == -1) {
        std::cerr << "getifaddrs failed\n";
        return ip;
    }
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
            char addr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sa->sin_addr), addr, INET_ADDRSTRLEN);
            if (std::string(addr) != "127.0.0.1") {
                ip = addr;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return ip;
}

static int timed_system(const char* cmd, int timeout_sec) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(127);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);

    struct timespec ts{ 0, 20'000'000 };
    while (true) {
        int status;
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid)
            return status;

        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -1;
        }

        nanosleep(&ts, nullptr);
        if (ts.tv_nsec < 100'000'000)
            ts.tv_nsec = std::min(ts.tv_nsec * 2, (long)100'000'000);
    }
}

static bool writeAll(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return false;
        buf += n;
        len -= n;
    }
    return true;
}

bool RemoteController::loadClients(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << configPath << std::endl;
        return false;
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
        return false;
    }

    if (!root.contains("clients") || !root["clients"].is_array()) {
        std::cerr << "clients array missing in JSON" << std::endl;
        return false;
    }

    m_clients.clear();
    for (const auto& clientJson : root["clients"]) {
        Client client;
        client.loadFromJson(clientJson);
        m_clients.push_back(client);
    }

    std::cout << "Loaded " << m_clients.size() << " clients" << std::endl;
    return true;
}

void RemoteController::pushConfigToClients(const std::string& serverIp) {
    std::string ip = serverIp;
    if (ip.empty())
        ip = getLocalIP();
    std::cout << "Using local IP: " << ip << std::endl;

    nlohmann::json root;
    root["connection_config"] = nlohmann::json::array({
        { {"server_ip", ip}, {"server_port", SERVER_PORT} }
    });
    std::string json_str = root.dump(2);

    char temp_template[] = "/tmp/rat_config_XXXXXX";
    int fd = mkstemp(temp_template);
    if (fd < 0) {
        std::cerr << "Failed to create temporary file\n";
        return;
    }
    if (!writeAll(fd, json_str.c_str(), json_str.size())) {
        std::cerr << "Failed to write config to temporary file\n";
        close(fd);
        unlink(temp_template);
        return;
    }
    close(fd);

    std::cout << "Created config file: " << temp_template << std::endl;

    for (const auto& client : m_clients) {
        std::string remotePath = "/home/" + client.getUser() + "/connection_config.json";
        std::cout << "Uploading to " << client.getId() << " at " << remotePath << std::endl;
        SCPManager::getInstance().uploadFile(client, temp_template, remotePath, false);
    }

    unlink(temp_template);
    std::cout << "Configuration pushed to all clients.\n";
}

void RemoteController::pushMsgConfigToClients() {
    for (const auto& client : m_clients) {
        std::string json = m_rulesManager.serializeConfig(client.getId());

        char temp_template[] = "/tmp/rat_msgcfg_XXXXXX";
        int fd = mkstemp(temp_template);
        if (fd < 0) {
            std::cerr << "Failed to create temp file for msg_config\n";
            continue;
        }
        if (!writeAll(fd, json.c_str(), json.size())) {
            std::cerr << "Failed to write msg_config for " << client.getId() << "\n";
            close(fd);
            unlink(temp_template);
            continue;
        }
        close(fd);

        std::string remotePath = "/home/" + client.getUser() + "/msg_config.json";
        std::cout << "Uploading msg_config to " << client.getId() << " at " << remotePath << std::endl;
        SCPManager::getInstance().uploadFile(client, temp_template, remotePath, false);

        unlink(temp_template);
    }
    std::cout << "Message configurations pushed to all clients.\n";
}

void RemoteController::pushAgentBinaryToClients() {
    const std::string agentBinaryPath = "../client_agent/bin/client_agent";
    if (access(agentBinaryPath.c_str(), F_OK) == -1) {
        std::cerr << "Warning: client_agent binary not found at " << agentBinaryPath
                  << " – skipping agent push.\n";
        return;
    }

    const std::string sshBase =
        "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
        " -i " + SSHManager::getInstance().getSSHKeyPath();

    {
        std::vector<std::thread> kthreads;
        kthreads.reserve(m_clients.size());
        for (const auto& client : m_clients) {
            std::ostringstream cmd;
            cmd << sshBase
                << " -p " << client.getPort()
                << " "    << client.getSSHTarget()
                << " \"pkill -x client_agent 2>/dev/null; sleep 0.3\"";
            std::string cmdStr = cmd.str();
            kthreads.emplace_back([cmdStr]() {
                timed_system(cmdStr.c_str(), 5);
            });
        }
        for (auto& t : kthreads) t.join();
    }

    for (const auto& client : m_clients) {
        std::string remotePath = "/home/" + client.getUser() + "/client_agent";
        std::cout << "Uploading client_agent to " << client.getId()
                  << " at " << remotePath << std::endl;
        SCPManager::getInstance().uploadFile(client, agentBinaryPath, remotePath, false);
    }

    {
        std::vector<std::thread> lthreads;
        lthreads.reserve(m_clients.size());
        for (const auto& client : m_clients) {
            std::string user       = client.getUser();
            std::string remotePath = "/home/" + user + "/client_agent";
            std::string startCmd =
                "chmod +x " + remotePath + " && "
                "cd /home/" + user + " && "
                "RAT_CLIENT_ID=" + client.getId() + " "
                "nohup ./client_agent >/dev/null 2>&1 </dev/null & "
                "sleep 0.5";
            std::ostringstream cmd;
            cmd << sshBase
                << " -p " << client.getPort()
                << " "    << client.getSSHTarget()
                << " \"" << startCmd << "\"";
            std::cout << "Starting client_agent on " << client.getId() << std::endl;

            std::string cmdStr  = cmd.str();
            std::string clientId = client.getId();
            lthreads.emplace_back([cmdStr, clientId]() {
                int ret = timed_system(cmdStr.c_str(), 5);
                if (ret == -1) {
                    std::cerr << "Timeout launching agent on " << clientId << "\n";
                } else if (WIFEXITED(ret) && WEXITSTATUS(ret) != 0 && WEXITSTATUS(ret) != 2) {
                    std::cerr << "Warning: agent launch failed (exit " << WEXITSTATUS(ret)
                              << ") for client " << clientId << "\n";
                }
            });
        }
        for (auto& t : lthreads) t.join();
    }

    std::cout << "Client agent binaries pushed and started on all clients.\n";
}

void RemoteController::startTCPServer() {
    m_serverIp = getLocalIP();
    m_tcpHandler = std::make_unique<TCPHandler>(
        SERVER_PORT,
        [this](const std::string& clientId, const std::string& msg) {
            MessageHandler::getInstance().rcvMsg(clientId, msg);
        },
        [this](const std::string& clientId, bool connected) {
            Client* client = getClientById(clientId);
            if (!client) {
                if (connected && m_tcpHandler)
                    m_tcpHandler->disconnectClient(clientId);
                return;
            }
            client->setConnected(connected);
            std::cout << "[" << MessageHandler::getInstance().getCurrentTime()
                      << "] Client " << clientId
                      << (connected ? " connected" : " disconnected") << "\n";
        }
    );

    m_tcpHandler->setValidationCallback([this](const std::string& id) {
        return getClientById(id) != nullptr;
    });

    if (!m_tcpHandler->start())
        std::cerr << "Failed to start TCP server on port " << SERVER_PORT << "\n";
    else {
        MessageHandler::getInstance().setTCPHandler(m_tcpHandler.get());
        std::cout << "TCP server listening on " << m_serverIp << ":" << SERVER_PORT << "\n";
    }
}

void RemoteController::stopTCPServer() {
    if (m_tcpHandler) {
        m_tcpHandler->stop();
        m_tcpHandler.reset();
    }
}

void RemoteController::parseAndExecute(const std::string& line) {
    if      (line.find("run ")    == 0) handleRunCommandOriginal(line);
    else if (line.find("scp ")    == 0) handleScpCommandOriginal(line);
    else if (line.find("shell ")  == 0) handleShellCommandOriginal(line);
    else if (line.find("server ") == 0) handlePluginCommand(line.substr(7));
    else
        std::cout << "Unknown command. Type 'help' for available commands." << std::endl;
}

void RemoteController::handleRunCommandOriginal(const std::string& line) {
    std::string rest = line.substr(4);

    if (rest.find("tag ") == 0) {
        std::string tag_cmd = rest.substr(4);
        size_t space = tag_cmd.find(' ');
        if (space == std::string::npos) {
            std::cout << "Usage: run tag <tag> <cmd>" << std::endl;
            return;
        }
        executeCommandByTag(tag_cmd.substr(0, space), trim(tag_cmd.substr(space + 1)));
    } else if (rest.find("all ") == 0) {
        executeCommandOnAll(trim(rest.substr(4)));
    } else {
        size_t space = rest.find(' ');
        if (space == std::string::npos) {
            std::cout << "Usage: run <id> <cmd>" << std::endl;
            return;
        }
        executeCommand(rest.substr(0, space), trim(rest.substr(space + 1)));
    }
}

void RemoteController::handleScpCommandOriginal(const std::string& line) {
    std::string rest = line.substr(4);

    if (rest.find("get ") == 0) {
        auto tokens = tokenize(rest.substr(4));
        if (tokens.size() < 3) {
            std::cout << "Usage: scp get <id> <remote> <local>" << std::endl;
            return;
        }
        downloadFile(tokens[0], tokens[1], tokens[2]);
    } else if (rest.find("tag ") == 0) {
        auto tokens = tokenize(rest.substr(4));
        if (tokens.size() < 3) {
            std::cout << "Usage: scp tag <tag> <local> <remote>" << std::endl;
            return;
        }
        SCPManager::getInstance().uploadToTagged(m_clients, tokens[0], tokens[1], tokens[2]);
    } else if (rest.find("all ") == 0) {
        auto tokens = tokenize(rest.substr(4));
        if (tokens.size() < 2) {
            std::cout << "Usage: scp all <local> <remote>" << std::endl;
            return;
        }
        SCPManager::getInstance().uploadToAll(m_clients, tokens[0], tokens[1]);
    } else {
        auto tokens = tokenize(rest);
        if (tokens.size() < 3) {
            std::cout << "Usage: scp <id> <local> <remote>" << std::endl;
            return;
        }
        uploadFile(tokens[0], tokens[1], tokens[2]);
    }
}

void RemoteController::handleShellCommandOriginal(const std::string& line) {
    std::string id = trim(line.substr(6));
    if (id.empty()) {
        std::cout << "Usage: shell <id>" << std::endl;
        return;
    }
    openInteractiveShell(id);
}

static size_t skipWords(const std::string& s, size_t offset, int n) {
    for (int i = 0; i < n; ++i) {
        while (offset < s.size() && !std::isspace((unsigned char)s[offset]))
            ++offset;
        while (offset < s.size() && std::isspace((unsigned char)s[offset]))
            ++offset;
    }
    return offset;
}

void RemoteController::handlePluginCommand(const std::string& line) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return;
    const std::string& cmd = tokens[0];

    if (cmd == "status") {
        cmdStatus();

    } else if (cmd == "clients") {
        cmdClients();

    } else if (cmd == "connected") {
        cmdConnected();

    } else if (cmd == "msg") {

        if (tokens.size() < 3) {
            std::cout << "Usage: server msg <clientId> <text>\n";
            return;
        }
        size_t textStart = skipWords(line, 0, 2);
        std::string text = trim(line.substr(textStart));
        if (text.empty())
            std::cout << "Usage: server msg <clientId> <text>\n";
        else
            cmdMsg(tokens[1], text);

    } else if (cmd == "broadcast") {
        if (tokens.size() < 2) {
            std::cout << "Usage: server broadcast <text>\n";
            return;
        }
        size_t textStart = skipWords(line, 0, 1);
        std::string text = trim(line.substr(textStart));
        if (text.empty())
            std::cout << "Usage: server broadcast <text>\n";
        else
            cmdBroadcast(text);

    } else if (cmd == "tag") {
        if (tokens.size() < 3) {
            std::cout << "Usage: server tag <tag> <text>\n";
            return;
        }
        size_t textStart = skipWords(line, 0, 2);
        std::string text = trim(line.substr(textStart));
        if (text.empty())
            std::cout << "Usage: server tag <tag> <text>\n";
        else
            cmdTag(tokens[1], text);

    } else if (cmd == "rules") {
        if (tokens.size() < 3) {
            std::cout << "Usage: server rules <subcommand> <clientId> [...]\n"
                         "Type 'rules-help' for details.\n";
            return;
        }
        const std::string& sub = tokens[1];
        const std::string& cid = tokens[2];

        if (sub == "list") {
            cmdRulesList(cid);
        } else if (sub == "add") {
            size_t argsStart = skipWords(line, 0, 3);
            cmdRulesAdd(cid, trim(line.substr(argsStart)));
        } else if (sub == "remove") {
            if (tokens.size() >= 4)
                cmdRulesRemove(cid, tokens[3]);
            else
                std::cout << "Usage: server rules remove <id> <n>\n";
        } else if (sub == "enable" || sub == "disable") {
            if (tokens.size() >= 4)
                cmdRulesSet(cid, tokens[3], sub == "enable");
            else
                std::cout << "Usage: server rules enable/disable <id> <n>\n";
        } else if (sub == "settings") {
            cmdRulesSettings(cid);
        } else if (sub == "set-log") {
            if (tokens.size() >= 4)
                cmdRulesSetLog(cid, tokens[3]);
            else
                std::cout << "Usage: server rules set-log <id> <true|false>\n";
        } else if (sub == "set-fwd") {
            if (tokens.size() >= 4)
                cmdRulesSetFwd(cid, tokens[3]);
            else
                std::cout << "Usage: server rules set-fwd <id> <true|false>\n";
        } else if (sub == "push") {
            cmdRulesPush(cid);
        } else {
            std::cout << "Unknown rules subcommand. Type 'rules-help' for usage.\n";
        }
    } else {
        std::cout << "Unknown server command. Type 'help' for available commands.\n";
    }
}

void RemoteController::cmdStatus() {
    std::cout << "\n=== Server Status ===\n"
              << "Server: " << m_serverIp << ":" << SERVER_PORT << "\n"
              << "Total clients: " << m_clients.size() << "\n";
    if (m_tcpHandler) {
        auto connected = m_tcpHandler->getConnectedClients();
        std::cout << "Currently connected: " << connected.size() << "\n";
        for (const auto& c : m_clients) {
            bool isConn = std::find(connected.begin(), connected.end(), c.getId()) != connected.end();
            std::cout << "  • " << c.getDisplayName()
                      << (isConn ? " [CONNECTED]" : " [DISCONNECTED]") << "\n";
        }
    }
}

void RemoteController::cmdClients() {
    std::cout << "\n=== Configured Clients ===\n";
    for (const auto& c : m_clients)
        std::cout << "  • " << c.getDisplayName() << "\n";
}

void RemoteController::cmdConnected() {
    if (!m_tcpHandler) {
        std::cout << "TCP server not running.\n";
        return;
    }
    auto connected = m_tcpHandler->getConnectedClients();
    std::cout << "Connected clients (" << connected.size() << "):\n";
    for (const auto& id : connected)
        std::cout << "  " << id << "\n";
}

void RemoteController::cmdMsg(const std::string& clientId, const std::string& text) {
    MessageHandler::getInstance().sendMsg(clientId, text);
}

void RemoteController::cmdBroadcast(const std::string& text) {
    if (!m_tcpHandler) {
        std::cerr << "TCP handler not available\n";
        return;
    }
    m_tcpHandler->broadcastToAll(text);
    std::cout << "Broadcast sent.\n";
}

void RemoteController::cmdTag(const std::string& tag, const std::string& text) {
    if (!m_tcpHandler) {
        std::cerr << "TCP handler not available\n";
        return;
    }
    int sent = 0;
    for (auto& c : m_clients) {
        if (c.hasTag(tag) && c.isConnected()) {
            if (m_tcpHandler->sendToClient(c.getId(), text))
                sent++;
        }
    }
    std::cout << "Sent to " << sent << " clients with tag '" << tag << "'\n";
}

void RemoteController::cmdRulesList(const std::string& clientId) {
    auto filters = m_rulesManager.listFilters(clientId);
    std::cout << "\n=== dbus_filters for client '" << clientId << "' (" << filters.size() << " filter(s)) ===\n";
    if (filters.empty()) {
        std::cout << "  (none)\n\n";
        return;
    }
    for (size_t i = 0; i < filters.size(); i++) {
        const auto& f = filters[i];
        std::cout << "  [" << i << "] " << (f.enabled ? "[ON ] " : "[OFF] ")
                  << '"' << f.name << "\"\n"
                  << "        bus=" << f.bus
                  << "  log=" << (f.log ? "true" : "false")
                  << "  forward=" << (f.forward ? "true" : "false") << "\n";
        if (!f.match.empty())
            std::cout << "        match: " << f.match << "\n";
        std::cout << "        types: [";
        for (size_t j = 0; j < f.types.size(); j++) {
            if (j) std::cout << ", ";
            std::cout << f.types[j];
        }
        std::cout << "]\n";
    }
    std::cout << "\n";
}

void RemoteController::cmdRulesAdd(const std::string& clientId, const std::string& filterArgs) {
    std::istringstream iss(filterArgs);
    std::string name, bus, logStr, fwdStr, typesToken, match;
    iss >> name >> bus >> logStr >> fwdStr >> typesToken;
    std::getline(iss, match);
    match = trim(match);

    if (name.empty() || bus.empty() || logStr.empty() || fwdStr.empty()) {
        std::cout << "Usage: server rules add <id> <n> <bus> <log> <forward> [types] [match]\n";
        return;
    }

    DbusFilter f;
    f.name    = name;
    f.bus     = bus;
    f.log     = (logStr == "true" || logStr == "1");
    f.forward = (fwdStr == "true" || fwdStr == "1");
    if (typesToken == "all" || typesToken.empty()) {
        f.types = {"method_call", "method_return", "signal", "error"};
    } else {
        std::stringstream tss(typesToken);
        std::string t;
        while (std::getline(tss, t, ','))
            if (!t.empty()) f.types.push_back(t);
    }
    f.match   = match;
    f.enabled = true;

    if (m_rulesManager.addFilter(clientId, f)) {
        std::cout << "Filter added.\n";
        if (m_tcpHandler && m_tcpHandler->isClientConnected(clientId)) {
            std::string json = m_rulesManager.serializeConfig(clientId);
            m_tcpHandler->sendToClient(clientId, "MSG_CONFIG:" + json);
            std::cout << "Config pushed to client.\n";
        }
    } else {
        std::cout << "Failed to add filter (maybe duplicate name).\n";
    }
}

void RemoteController::cmdRulesRemove(const std::string& clientId, const std::string& name) {
    if (m_rulesManager.removeFilter(clientId, name)) {
        std::cout << "Filter removed.\n";
        if (m_tcpHandler && m_tcpHandler->isClientConnected(clientId)) {
            std::string json = m_rulesManager.serializeConfig(clientId);
            m_tcpHandler->sendToClient(clientId, "MSG_CONFIG:" + json);
        }
    } else {
        std::cout << "Filter not found.\n";
    }
}

void RemoteController::cmdRulesSet(const std::string& clientId, const std::string& name, bool enabled) {
    if (m_rulesManager.setFilterEnabled(clientId, name, enabled)) {
        std::cout << "Filter " << (enabled ? "enabled" : "disabled") << ".\n";
        if (m_tcpHandler && m_tcpHandler->isClientConnected(clientId)) {
            std::string json = m_rulesManager.serializeConfig(clientId);
            m_tcpHandler->sendToClient(clientId, "MSG_CONFIG:" + json);
        }
    } else {
        std::cout << "Filter not found.\n";
    }
}

void RemoteController::cmdRulesSettings(const std::string& clientId) {
    auto s = m_rulesManager.getSettings(clientId);
    std::cout << "\n=== global_settings for client '" << clientId << "' ===\n"
              << "  default_log:      " << (s.default_log ? "true" : "false") << "\n"
              << "  default_forward:  " << (s.default_forward ? "true" : "false") << "\n"
              << "  log_file:         " << s.log_file << "\n"
              << "  max_message_size: " << s.max_message_size << "\n\n";
}

void RemoteController::cmdRulesSetLog(const std::string& clientId, const std::string& val) {
    GlobalSettings s = m_rulesManager.getSettings(clientId);
    s.default_log = (val == "true" || val == "1");
    if (m_rulesManager.setSettings(clientId, s)) {
        std::cout << "default_log set to " << (s.default_log ? "true" : "false") << "\n";
        pushConfigToClient(clientId);
    }
}

void RemoteController::cmdRulesSetFwd(const std::string& clientId, const std::string& val) {
    GlobalSettings s = m_rulesManager.getSettings(clientId);
    s.default_forward = (val == "true" || val == "1");
    if (m_rulesManager.setSettings(clientId, s)) {
        std::cout << "default_forward set to " << (s.default_forward ? "true" : "false") << "\n";
        pushConfigToClient(clientId);
    }
}

void RemoteController::cmdRulesPush(const std::string& clientId) {
    if (!m_tcpHandler || !m_tcpHandler->isClientConnected(clientId)) {
        std::cout << "Client not connected.\n";
        return;
    }
    std::string json = m_rulesManager.serializeConfig(clientId);
    m_tcpHandler->sendToClient(clientId, "MSG_CONFIG:" + json);
    std::cout << "Config pushed to " << clientId << "\n";
}

void RemoteController::pushConfigToClient(const std::string& clientId) {
    if (m_tcpHandler && m_tcpHandler->isClientConnected(clientId)) {
        std::string json = m_rulesManager.serializeConfig(clientId);
        m_tcpHandler->sendToClient(clientId, "MSG_CONFIG:" + json);
    }
}

void RemoteController::executeCommand(const std::string& clientId, const std::string& command) {
    Client* client = getClientById(clientId);
    if (!client) { std::cout << "Client '" << clientId << "' not found" << std::endl; return; }
    SSHManager::getInstance().executeCommand(*client, command, true);
}

void RemoteController::executeCommandByTag(const std::string& tag, const std::string& command) {
    bool found = false;
    for (const auto& c : m_clients) {
        if (c.hasTag(tag)) {
            SSHManager::getInstance().executeCommand(c, command, true);
            found = true;
        }
    }
    if (!found)
        std::cout << "No clients found with tag '" << tag << "'" << std::endl;
}

void RemoteController::executeCommandOnAll(const std::string& command) {
    for (const auto& c : m_clients)
        SSHManager::getInstance().executeCommand(c, command, true);
}

void RemoteController::uploadFile(const std::string& clientId, const std::string& localPath, const std::string& remotePath) {
    Client* client = getClientById(clientId);
    if (!client) { std::cout << "Client '" << clientId << "' not found" << std::endl; return; }
    SCPManager::getInstance().uploadFile(*client, localPath, remotePath, false);
}

void RemoteController::downloadFile(const std::string& clientId, const std::string& remotePath, const std::string& localPath) {
    Client* client = getClientById(clientId);
    if (!client) { std::cout << "Client '" << clientId << "' not found" << std::endl; return; }
    SCPManager::getInstance().downloadFile(*client, remotePath, localPath);
}

void RemoteController::openInteractiveShell(const std::string& clientId) {
    Client* client = getClientById(clientId);
    if (!client) { std::cout << "Client '" << clientId << "' not found" << std::endl; return; }
    ShellManager::getInstance().executeInteractiveShell(*client);
}

std::vector<Client> RemoteController::getClientsByTag(const std::string& tag) const {
    std::vector<Client> result;
    for (const auto& c : m_clients)
        if (c.hasTag(tag))
            result.push_back(c);
    return result;
}

Client* RemoteController::getClientById(const std::string& id) {
    for (auto& c : m_clients)
        if (c.getId() == id)
            return &c;
    return nullptr;
}

std::vector<std::string> RemoteController::tokenize(const std::string& str, char delimiter) const {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        token = trim(token);
        if (!token.empty())
            tokens.push_back(token);
    }
    return tokens;
}

std::string RemoteController::trim(const std::string& str) const {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

bool RemoteController::isBlank(const std::string& str) const {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) {
        return std::isspace(c);
    });
}

void RemoteController::printHelp() const {
    std::cout <<
        "============================================================\n"
        "                 Remote SSH/SCP Controller\n"
        "============================================================\n"
        "Available Commands:\n"
        "  run <id> <command>               Execute shell command on remote (async)\n"
        "  run tag <tag> <command>          Execute on all clients with tag (async)\n"
        "  run all <command>                Execute on all clients (async)\n"
        "  scp <id> <local> <remote>        Upload local -> remote (sync)\n"
        "  scp get <id> <remote> <local>    Download remote -> local (sync)\n"
        "  scp tag <tag> <local> <remote>   Upload for all with tag (async)\n"
        "  scp all <local> <remote>         Upload for all clients (async)\n"
        "  shell <id>                       Open interactive ssh shell (sync)\n"
        "\n"
        "  Server commands (prefix with 'server'):\n"
        "    server status                    Show server and client status\n"
        "    server clients                   List all configured clients\n"
        "    server connected                 List currently connected clients\n"
        "    server msg <id> <text>           Send message to a client\n"
        "    server broadcast <text>          Broadcast to all connected clients\n"
        "    server tag <tag> <text>          Send to all clients with a given tag\n"
        "    server rules list <id>           List dbus_filters for client\n"
        "    server rules add <id> <n> <bus> <log> <forward> [types] [match]\n"
        "    server rules remove <id> <n>  Remove filter\n"
        "    server rules enable <id> <n>  Enable filter\n"
        "    server rules disable <id> <n> Disable filter\n"
        "    server rules settings <id>       Show global settings\n"
        "    server rules set-log <id> <true|false>\n"
        "    server rules set-fwd <id> <true|false>\n"
        "    server rules push <id>           Push current config to client\n"
        "\n"
        "  rules-help                         Detailed rules syntax\n"
        "  help / ?                          Show this help\n"
        "  quit / exit                       Quit\n"
        "============================================================\n"
        "Loaded " << m_clients.size() << " clients\n"
        "Server port: " << SERVER_PORT << "\n";
}

void RemoteController::printRulesHelp() const {
    std::cout << R"(
Rules add syntax:
  server rules add <clientId> <n> <bus> <log> <forward> [types] [match]

  <n>     Unique filter name, use quotes for multi-word: "My Filter"
  <bus>      system | session | both
  <log>      true | false
  <forward>  true | false
  [types]    Comma-separated list: method_call,method_return,signal,error
             or "all" for all types.
  [match]    D-Bus match rule string (optional)

Examples:
  server rules add client1 "SystemD Services" system true true method_call,method_return,signal "sender='org.freedesktop.systemd1'"
  server rules add client1 "All Traffic" both false false all
  server rules remove client1 "All Traffic"
  server rules enable client1 "SystemD Services"
  server rules settings client1
  server rules set-log client1 false
  server rules push client1
)";
}
