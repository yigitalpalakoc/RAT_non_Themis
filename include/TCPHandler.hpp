#pragma once
// remote_access_tool/src/TCPHandler.hpp

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>

struct ClientConnection {
    int         socketFd       = -1;
    std::string ipAddress;
    int         port           = 0;
    std::string clientId;
    bool        isAuthenticated = false;
};


class TCPHandler {
public:
    using MessageCallback = std::function<void(const std::string& clientId, const std::string& message)>;
    using ConnectionCallback = std::function<void(const std::string& clientId, bool connected)>;
    using ValidationCallback = std::function<bool(const std::string& clientId)>;

    TCPHandler(int port, MessageCallback msgCb, ConnectionCallback connCb);
    ~TCPHandler();

    bool start();
    void stop();
    void setValidationCallback(ValidationCallback cb) { m_validationCallback = cb; }
    bool sendToClient(const std::string& clientId, const std::string& message);
    void broadcastToAll(const std::string& message);
    void disconnectClient(const std::string& clientId);
    std::vector<std::string> getConnectedClients() const;
    bool isClientConnected(const std::string& clientId) const;
    static bool isPortInUse(int port);
    static bool freePort   (int port);
    static void cleanupPort(int port);

private:
    void serverAcceptThread();
    void serverClientThread(int clientFd, const std::string& ip, int port);
    void safeLog(const std::string& msg);
    int  m_serverPort;
    int  m_serverFd;
    std::atomic<bool> m_running;
    MessageCallback    m_messageCallback;
    ConnectionCallback m_connectionCallback;
    ValidationCallback m_validationCallback;
    mutable std::mutex m_clientsMutex;
    std::map<int,         std::shared_ptr<ClientConnection>> m_clientsByFd;
    std::map<std::string, std::shared_ptr<ClientConnection>> m_clientsById;
    std::map<int,         std::thread>                       m_clientThreads;
    std::thread m_acceptThread;
    mutable std::mutex m_logMutex;
};
