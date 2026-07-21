// remote_access_tool/src/TCPHandler.cpp
#include "TCPHandler.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>

static constexpr int MAX_CLIENTS = 64;

bool TCPHandler::isPortInUse(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    bool inUse = (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno == EADDRINUSE);
    close(sock);
    return inUse;
}

bool TCPHandler::freePort(int port) {
    pid_t currentPid = getpid();
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "lsof -t -i :%d 2>/dev/null | while read p; do "
             "[ \"$p\" != \"%d\" ] && kill -9 \"$p\" 2>/dev/null; "
             "done",
             port, (int)currentPid);
    system(cmd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return !isPortInUse(port);
}

void TCPHandler::cleanupPort(int port) {
    if (isPortInUse(port)) {
        std::cout << "[TCPHandler] Port " << port << " is in use. Attempting to free it..." << std::endl;
        freePort(port);
    }
}

TCPHandler::TCPHandler(int port, MessageCallback msgCb, ConnectionCallback connCb)
    : m_serverPort(port)
    , m_serverFd(-1)
    , m_running(false)
    , m_messageCallback(msgCb)
    , m_connectionCallback(connCb) {
    safeLog("[TCPHandler] Created in SERVER mode on port " + std::to_string(port));
}

TCPHandler::~TCPHandler() {
    stop();
}

void TCPHandler::safeLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::cout << msg << std::endl;
}

bool TCPHandler::start() {
    if (m_running) return true;
    m_running = true;

    cleanupPort(m_serverPort);

    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd < 0) {
        safeLog("[TCPHandler] Failed to create server socket: " + std::string(strerror(errno)));
        m_running = false;
        return false;
    }

    int opt = 1;
    if (setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        safeLog("[TCPHandler] Failed to set socket options: " + std::string(strerror(errno)));
        close(m_serverFd);
        m_running = false;
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_serverPort);

    if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        safeLog("[TCPHandler] Failed to bind to port " + std::to_string(m_serverPort) + ": " + strerror(errno));
        close(m_serverFd);
        m_running = false;
        return false;
    }

    if (listen(m_serverFd, 10) < 0) {
        safeLog("[TCPHandler] Failed to listen: " + std::string(strerror(errno)));
        close(m_serverFd);
        m_running = false;
        return false;
    }

    int flags = fcntl(m_serverFd, F_GETFL, 0);
    fcntl(m_serverFd, F_SETFL, flags | O_NONBLOCK);

    m_acceptThread = std::thread(&TCPHandler::serverAcceptThread, this);
    safeLog("[TCPHandler] Server started on port " + std::to_string(m_serverPort));
    return true;
}

void TCPHandler::stop() {
    if (!m_running) return;
    safeLog("[TCPHandler] Stopping...");

    m_running = false;

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_serverFd >= 0) {
            shutdown(m_serverFd, SHUT_RDWR);
            close(m_serverFd);
            m_serverFd = -1;
        }
        for (auto& pair : m_clientsByFd) {
            shutdown(pair.first, SHUT_RDWR);
            close(pair.first);
        }
    }

    if (m_acceptThread.joinable())
        m_acceptThread.join();

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clientThreads) {
            if (pair.second.joinable())
                pair.second.join();
        }
        m_clientsByFd.clear();
        m_clientsById.clear();
        m_clientThreads.clear();
    }

    safeLog("[TCPHandler] Stopped");
}

void TCPHandler::serverAcceptThread() {
    safeLog("[TCPHandler] Accept thread started");

    while (m_running) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &addrLen);

        if (clientFd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && m_running) {
                safeLog("[TCPHandler] Accept failed: " + std::string(strerror(errno)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!m_running) {
            close(clientFd);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            if ((int)m_clientsByFd.size() >= MAX_CLIENTS) {
                safeLog("[TCPHandler] Connection limit reached (" + std::to_string(MAX_CLIENTS) + "), rejecting new connection");
                std::string msg = "ERROR: Server full\n";
                send(clientFd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
                close(clientFd);
                continue;
            }
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr, INET_ADDRSTRLEN);
        int clientPort = ntohs(clientAddr.sin_port);

        safeLog("[TCPHandler] New connection from " + std::string(ipStr) + ":" + std::to_string(clientPort));

        auto conn = std::make_shared<ClientConnection>();
        conn->socketFd = clientFd;
        conn->ipAddress = ipStr;
        conn->port = clientPort;
        conn->isAuthenticated = false;

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clientsByFd[clientFd] = conn;
            m_clientThreads[clientFd] = std::thread(&TCPHandler::serverClientThread, this, clientFd, std::string(ipStr), clientPort);
        }
    }

    safeLog("[TCPHandler] Accept thread stopped");
}

void TCPHandler::serverClientThread(int clientFd, const std::string& ip, int port) {
    safeLog("[TCPHandler] Client thread started for " + ip + ":" + std::to_string(port));
    std::string authenticatedClientId;

    try {
        char buffer[4096];
        std::string leftover;
        bool forceDisconnect = false;

        while (m_running && !forceDisconnect) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(clientFd, &readfds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;

            int activity = select(clientFd + 1, &readfds, NULL, NULL, &tv);

            if (!m_running) break;
            if (activity < 0) {
                if (errno != EINTR)
                    safeLog("[TCPHandler] Select error: " + std::string(strerror(errno)));
                continue;
            }
            if (activity == 0) continue;

            ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) {
                safeLog("[TCPHandler] Client " + ip + ":" + std::to_string(port) + " disconnected");
                break;
            }

            buffer[bytesRead] = '\0';
            std::string data = leftover + std::string(buffer, bytesRead);
            size_t pos = 0;
            size_t newline;

            while (!forceDisconnect &&
                   (newline = data.find('\n', pos)) != std::string::npos) {
                std::string message = data.substr(pos, newline - pos);
                if (!message.empty() && message.back() == '\r')
                    message.pop_back();
                pos = newline + 1;

                if (message.empty()) continue;

                bool justAuthenticated = false;
                bool isAuthenticated   = false;
                std::string currentClientId;
                bool shouldReject = false;

                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    auto it = m_clientsByFd.find(clientFd);
                    if (it == m_clientsByFd.end()) break;

                    auto conn = it->second;
                    if (!conn->isAuthenticated) {
                        if (message.find("ID:") == 0) {
                            currentClientId = message.substr(3);
                            currentClientId.erase(currentClientId.find_last_not_of(" \t\r\n") + 1);
                            currentClientId.erase(0, currentClientId.find_first_not_of(" \t\r\n"));

                            if (m_validationCallback && !m_validationCallback(currentClientId)) {
                                safeLog("[TCPHandler] Rejected unknown client: '" + currentClientId + "'");
                                shouldReject = true;
                            } else {
                                conn->clientId       = currentClientId;
                                conn->isAuthenticated = true;
                                m_clientsById[currentClientId] = conn;
                                authenticatedClientId = currentClientId;
                                justAuthenticated = true;
                                safeLog("[TCPHandler] Client authenticated as: " + currentClientId);
                            }
                        }
                    } else {
                        currentClientId = conn->clientId;
                        isAuthenticated = true;
                    }
                }

                if (shouldReject) {
                    std::string rejectMsg = "ERROR: Unknown client ID - not in configuration\n";
                    send(clientFd, rejectMsg.c_str(), rejectMsg.length(), MSG_NOSIGNAL);
                    forceDisconnect = true;
                    break;
                }

                if (justAuthenticated) {
                    if (m_connectionCallback) {
                        try { m_connectionCallback(currentClientId, true); }
                        catch (const std::exception& e) {
                            safeLog("[TCPHandler] Exception in connection callback: " + std::string(e.what()));
                        } catch (...) {
                            safeLog("[TCPHandler] Unknown exception in connection callback");
                        }
                    }
                    std::string welcome = "Welcome " + currentClientId + "!\n";
                    send(clientFd, welcome.c_str(), welcome.length(), MSG_NOSIGNAL);
                }

                if (isAuthenticated && m_messageCallback) {
                    try { m_messageCallback(currentClientId, message); }
                    catch (const std::exception& e) {
                        safeLog("[TCPHandler] Exception in message callback: " + std::string(e.what()));
                    } catch (...) {
                        safeLog("[TCPHandler] Unknown exception in message callback");
                    }
                }
            }
            leftover = data.substr(pos);
        }

    } catch (const std::exception& e) {
        safeLog("[TCPHandler] Unhandled exception in client thread for " +
                ip + ":" + std::to_string(port) + ": " + e.what());
    } catch (...) {
        safeLog("[TCPHandler] Unknown unhandled exception in client thread for " +
                ip + ":" + std::to_string(port));
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clientsByFd.find(clientFd);
        if (it != m_clientsByFd.end()) {
            m_clientsById.erase(it->second->clientId);
            m_clientsByFd.erase(it);
        }
    }

    if (!authenticatedClientId.empty() && m_connectionCallback) {
        try { m_connectionCallback(authenticatedClientId, false); }
        catch (const std::exception& e) {
            safeLog("[TCPHandler] Exception in disconnection callback: " + std::string(e.what()));
        } catch (...) {
            safeLog("[TCPHandler] Unknown exception in disconnection callback");
        }
    }

    close(clientFd);
    safeLog("[TCPHandler] Client thread finished for " + ip + ":" + std::to_string(port));
}

void TCPHandler::disconnectClient(const std::string& clientId) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clientsById.find(clientId);
        if (it == m_clientsById.end()) return;
        fd = it->second->socketFd;
        m_clientsByFd.erase(fd);
        m_clientsById.erase(it);
    }
    safeLog("[TCPHandler] Forcefully disconnecting client: " + clientId);
    std::string rejectMsg = "ERROR: Unknown client ID - not in configuration. Goodbye.\n";
    send(fd, rejectMsg.c_str(), rejectMsg.length(), MSG_NOSIGNAL);
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

bool TCPHandler::sendToClient(const std::string& clientId, const std::string& message) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clientsById.find(clientId);
        if (it == m_clientsById.end()) return false;
        fd = it->second->socketFd;
    }
    std::string formatted = message + "\n";
    ssize_t sent = send(fd, formatted.c_str(), formatted.length(), MSG_NOSIGNAL);
    return (sent > 0);
}

void TCPHandler::broadcastToAll(const std::string& message) {
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        fds.reserve(m_clientsByFd.size());
        for (auto& pair : m_clientsByFd)
            fds.push_back(pair.first);
    }
    std::string formatted = message + "\n";
    for (int fd : fds)
        send(fd, formatted.c_str(), formatted.length(), MSG_NOSIGNAL);
}

std::vector<std::string> TCPHandler::getConnectedClients() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    std::vector<std::string> clients;
    for (auto& pair : m_clientsById)
        clients.push_back(pair.first);
    return clients;
}

bool TCPHandler::isClientConnected(const std::string& clientId) const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return m_clientsById.find(clientId) != m_clientsById.end();
}
