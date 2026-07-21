#pragma once
// client_agent/src/TCPClient.hpp

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

class TCPClient {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    TCPClient();
    ~TCPClient();

    bool connect(const std::string& serverIp, int serverPort, const std::string& clientId);
    void disconnect();
    bool sendMessage(const std::string& message);
    void setMessageCallback(MessageCallback cb) { m_messageCallback = cb; }
    void setConnectionCallback(ConnectionCallback cb) { m_connectionCallback = cb; }
    bool isConnected() const { return m_connected; }

private:
    bool doConnect();
    void receiveThread();
    std::string m_serverIp;
    int m_serverPort = 0;
    std::string m_clientId;
    int m_socket = -1;
    mutable std::mutex m_socketMutex;
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_connected { false };
    std::thread m_receiveThread;
    MessageCallback m_messageCallback;
    ConnectionCallback m_connectionCallback;
    static constexpr int BUFFER_SIZE = 4096;
};