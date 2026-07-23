// client_agent/src/Agent.cpp
#include "Agent.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/select.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <csignal> 

using json = nlohmann::json;

extern volatile sig_atomic_t g_shutdown_flag;

Agent::Agent()
    : m_tcpClient(std::make_unique<TCPClient>())
    , m_messageHandler(MessageHandler::getInstance())
    , m_running(false) {}

Agent::~Agent() {
    stop();
}

void Agent::startStdinReader() {
    if (m_stdinReaderThread.joinable()) return;
    if (pipe(m_inputPipe) < 0) {
        std::cerr << "[Agent] Failed to create input pipe\n";
        return;
    }
    int flags = fcntl(m_inputPipe[0], F_GETFL, 0);
    fcntl(m_inputPipe[0], F_SETFL, flags | O_NONBLOCK);
    m_stdinReaderThread = std::thread(&Agent::stdinReaderThread, this);
}

void Agent::stopStdinReader() {
    if (m_stdinReaderThread.joinable()) {
        if (m_inputPipe[1] != -1) {
            char x = 'x';
            write(m_inputPipe[1], &x, 1);
        }
        m_stdinReaderThread.join();
    }
    if (m_inputPipe[0] != -1) { close(m_inputPipe[0]); m_inputPipe[0] = -1; }
    if (m_inputPipe[1] != -1) { close(m_inputPipe[1]); m_inputPipe[1] = -1; }
}

void Agent::stdinReaderThread() {
    std::cout << "[Agent] Stdin reader thread started\n";
    fd_set readfds;

    while (m_running && !g_shutdown_flag) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        if (m_inputPipe[0] != -1) FD_SET(m_inputPipe[0], &readfds);
        int maxFd = std::max(STDIN_FILENO, m_inputPipe[0]) + 1;

        int activity = select(maxFd, &readfds, nullptr, nullptr, nullptr);
        if (activity < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (m_inputPipe[0] != -1 && FD_ISSET(m_inputPipe[0], &readfds)) {
            char buf[16];
            read(m_inputPipe[0], buf, sizeof(buf));
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                if (std::cin.eof()) {
                    std::cout << "\nEOF received. Shutting down...\n";
                    g_shutdown_flag = 1;
                }
                break;
            }
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            auto last = line.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) line.erase(last + 1);

            if (!line.empty())
                sendTextMessage(line);
        }
    }

    std::cout << "[Agent] Stdin reader thread stopped\n";
}

void Agent::sendTextMessage(const std::string& text) {
    if (m_tcpClient->isConnected()) {
        m_tcpClient->sendMessage(text);
        m_messageHandler.logToFile(m_clientId, "SENT", text);
        std::cout << "[" << m_messageHandler.getCurrentTime() << "] Sent: " << text << "\n";
    } else {
        std::cout << "Not connected to server.\n";
    }
}

static std::string readFirstExisting(const std::vector<std::string>& candidates, std::string* usedPath = nullptr) {
    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::stringstream buf;
        buf << f.rdbuf();
        if (usedPath) *usedPath = path;
        return buf.str();
    }
    return {};
}

bool Agent::loadServerConfig(const std::string& configPath) {
    std::string usedPath;
    std::string content = readFirstExisting(
        { configPath, "./connection_config.json", "../connection_config.json" },
        &usedPath);

    if (content.empty()) {
        std::cerr << "[Agent] Could not open connection config\n";
        return false;
    }
    std::cout << "[Agent] Using config: " << usedPath << "\n";

    json root;
    try {
        root = json::parse(content);
    } catch (const json::parse_error& e) {
        std::cerr << "[Agent] Failed to parse connection config: " << e.what() << "\n";
        return false;
    }

    if (!root.contains("connection_config") ||
        !root["connection_config"].is_array() ||
        root["connection_config"].empty()) {
        std::cerr << "[Agent] No 'connection_config' entries\n";
        return false;
    }

    const auto& cfg = root["connection_config"][0];
    m_serverConfig.ip   = cfg.value("server_ip",   "");
    m_serverConfig.port = cfg.value("server_port",  0);

    if (!m_serverConfig.isValid()) {
        std::cerr << "[Agent] Invalid server configuration\n";
        return false;
    }

    std::cout << "[Agent] Server: " << m_serverConfig.ip
              << ":" << m_serverConfig.port << "\n";
    return true;
}


void Agent::onTCPMessageReceived(const std::string& message) {
    m_messageHandler.receiveMessage(message);
}

bool Agent::initialize(const std::string& configPath, const std::string& clientId) {
    std::cout << "========================================\n"
              << "              Client Agent\n"
              << "========================================\n";

    if (!m_lockfileManager.acquireLock()) {
        std::cerr << "[Agent] Another instance is already running\n";
        return false;
    }

    if (!loadServerConfig(configPath)) {
        m_lockfileManager.releaseLock();
        return false;
    }

    m_clientId      = clientId;

    if (m_clientId.empty()) {
        std::cerr << "[Agent] Client ID cannot be empty\n";
        m_lockfileManager.releaseLock();
        return false;
    }

    m_tcpClient->setMessageCallback([this](const std::string& msg) {
        onTCPMessageReceived(msg);
    });

    m_messageHandler.setMessageCallback([this](const std::string& msg) {
        if (m_tcpClient->isConnected())
            m_tcpClient->sendMessage(msg);
    });

    m_tcpClient->setConnectionCallback([this](bool connected) {
        std::cout << "\n[" << m_messageHandler.getCurrentTime() << "] "
                  << (connected ? "Connected to" : "Disconnected from")
                  << " server\n";
    });

    std::cout << "\n[Agent] Initialized — client ID: " << m_clientId
              << "  server: " << m_serverConfig.ip << ":" << m_serverConfig.port
              << "\n";
    return true;
}

void Agent::start() {
    if (m_running) return;
    m_running = true;

    if (!m_tcpClient->connect(m_serverConfig.ip, m_serverConfig.port, m_clientId)) {
        std::cerr << "[Agent] Failed to connect to server\n";
    }
    bool stdinIsTty    = isatty(STDIN_FILENO);
    bool stdoutIsTty   = isatty(STDOUT_FILENO);
    bool launchedByRat = (std::getenv("RAT_CLIENT_ID") != nullptr);

    if (stdinIsTty && stdoutIsTty && !launchedByRat) {
        startStdinReader();
        std::cout << "\n[Agent] Interactive mode — type messages and press Enter.\n";
    } else {
        std::cout << "\n[Agent] Background mode — stdin reader disabled.\n";
    }
}

void Agent::stop() {
    if (!m_running) return;
    std::cout << "\n[Agent] Stopping...\n";
    m_running = false;

    stopStdinReader();
    m_tcpClient->disconnect();

    m_lockfileManager.releaseLock();
    std::cout << "[Agent] Stopped\n";
}