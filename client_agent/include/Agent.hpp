// client_agent/include/Agent.hpp
#ifndef AGENT_HPP
#define AGENT_HPP

#include "TCPClient.hpp"
#include "MessageHandler.hpp"
#include "LockfileManager.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <shared_mutex>
#include <nlohmann/json.hpp> 

struct ServerConfig {
    std::string ip;
    int port;
    bool isValid() const { return !ip.empty() && port > 0; }
};

class Agent {
public:
    Agent();
    ~Agent();

    bool initialize(const std::string& configPath, const std::string& clientId);
    void start();
    void stop();
    bool isRunning() const { return m_running; }
    void sendTextMessage(const std::string& text);
    void startStdinReader();
    void stopStdinReader();

private:
    using json = nlohmann::json;
    
    bool loadServerConfig(const std::string& configPath);
    bool loadMsgConfig(const std::string& configPath);

    // TCP incoming
    void onTCPMessageReceived(const std::string& message);

    std::unique_ptr<TCPClient> m_tcpClient;
    LockfileManager m_lockfileManager;
    MessageHandler& m_messageHandler;

    std::atomic<bool> m_running;
    void stdinReaderThread();
    std::thread m_stdinReaderThread;
    int m_inputPipe[2];

    std::string  m_clientId;
    ServerConfig m_serverConfig;
    mutable std::shared_mutex m_filtersMutex;
};

#endif // AGENT_HPP