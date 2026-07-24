// client_agent/src/Agent.hpp
#pragma once
#include "TCPClient.hpp"
#include "LockfileManager.hpp"
#include <string>
#include <thread>
#include <mutex>
#include <fstream>
#include <memory>
#include <csignal>

extern volatile sig_atomic_t g_shutdown_flag;

class Agent {
public:
    Agent();
    ~Agent();

    bool initialize(const std::string& configPath, const std::string& clientId);
    void start();
    void stop();

private:
    void stdinLoop();
    void sendText(const std::string& text);
    void logMessage(const std::string& dir, const std::string& msg);
    bool loadConfig();

    std::unique_ptr<TCPClient> m_tcp;
    LockfileManager            m_lock;

    std::string m_clientId;
    std::string m_host;
    int         m_port    = 0;
    bool        m_running = false;

    std::thread   m_stdinThread;
    int           m_pipe[2] = {-1, -1};

    std::mutex    m_logMutex;
    std::ofstream m_logFile;
};
