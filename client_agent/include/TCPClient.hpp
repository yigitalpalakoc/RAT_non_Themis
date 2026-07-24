// client_agent/src/TCPClient.hpp
#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

class TCPClient {
public:
    using MessageCb    = std::function<void(const std::string&)>;
    using ConnectionCb = std::function<void(bool)>;

    TCPClient();
    ~TCPClient();

    bool connect(const std::string& host, int port, const std::string& clientId);
    void disconnect();
    bool sendMessage(const std::string& msg);
    bool isConnected() const { return m_connected; }

    void setMessageCallback(MessageCb cb)       { m_messageCallback    = std::move(cb); }
    void setConnectionCallback(ConnectionCb cb) { m_connectionCallback = std::move(cb); }

private:
    void receiveLoop();

    static constexpr int kBufSize = 4096;

    int              m_socket{-1};
    std::atomic_bool m_running{false};
    std::atomic_bool m_connected{false};
    std::string      m_clientId;
    std::string      m_host;
    int              m_port{0};

    std::thread m_recvThread;
    std::mutex  m_socketMutex;

    MessageCb    m_messageCallback;
    ConnectionCb m_connectionCallback;
};
