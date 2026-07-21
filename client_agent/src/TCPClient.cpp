// client_agent/src/TCPClient.cpp
#include "TCPClient.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <thread>
#include <chrono>

TCPClient::TCPClient()
    : m_socket(-1), m_running(false), m_connected(false) {
    std::cout << "[Client] TCPClient created\n";
}

TCPClient::~TCPClient() {
    disconnect();
}

bool TCPClient::connect(const std::string& serverIp, int serverPort, const std::string& clientId) {
    if (m_connected)
        disconnect();
    m_clientId = clientId;
    m_serverIp = serverIp;
    m_serverPort = serverPort;
    return doConnect();
}

bool TCPClient::doConnect() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "[Client] Failed to create socket: " << strerror(errno) << "\n";
        return false;
    }

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(m_serverPort);

    if (inet_pton(AF_INET, m_serverIp.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "[Client] Invalid server IP: " << m_serverIp << "\n";
        close(sock);
        return false;
    }

    std::cout << "[Client] Connecting to " << m_serverIp << ":" << m_serverPort << "...\n";

    if (::connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "[Client] Connection failed: " << strerror(errno) << "\n";
        close(sock);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        m_socket = sock;
    }

    m_connected = true;
    m_running   = true;

    std::string idMsg = "ID:" + m_clientId + "\n";
    send(sock, idMsg.c_str(), idMsg.length(), MSG_NOSIGNAL);

    if (!m_receiveThread.joinable())
        m_receiveThread = std::thread(&TCPClient::receiveThread, this);

    std::cout << "[Client] Connected as '" << m_clientId << "'\n";

    if (m_connectionCallback)
        m_connectionCallback(true);

    return true;
}

void TCPClient::disconnect() {
    if (!m_connected && m_socket < 0 && !m_receiveThread.joinable())
        return;

    std::cout << "[Client] Disconnecting...\n";
    m_running   = false;
    m_connected = false;
    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_socket >= 0) {
            shutdown(m_socket, SHUT_RDWR);
            close(m_socket);
            m_socket = -1;
        }
    }
    if (m_receiveThread.joinable()) {
        std::cout << "[Client] Waiting for receive thread...\n";
        m_receiveThread.join();
    }

    if (m_connectionCallback)
        m_connectionCallback(false);

    std::cout << "[Client] Disconnected\n";
}

bool TCPClient::sendMessage(const std::string& message) {
    int fd;
    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (!m_connected || m_socket < 0)
            return false;
        fd = m_socket;
    }
    std::string formatted = message + "\n";
    ssize_t sent = send(fd, formatted.c_str(), formatted.length(), MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
            std::cout << "[Client] Connection lost while sending\n";
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_connected = false;
            }
        }
        return false;
    }
    return true;
}

void TCPClient::receiveThread() {
    char buffer[BUFFER_SIZE];
    std::string leftover;
    std::cout << "[Client] Receive thread started\n";

    while (m_running) {
        if (!m_connected && m_running) {
            int delay = 1;
            while (m_running && !m_connected) {
                std::cout << "[Client] Reconnecting in " << delay << "s...\n";
                for (int i = 0; i < delay * 10 && m_running; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!m_running) break;
                if (doConnect()) {
                    leftover.clear();
                    break;
                }
                delay = std::min(delay * 2, 30);
            }
            continue;
        }
        int fd;
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_socket < 0) {
                m_connected = false;
                continue;
            }
            fd = m_socket;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval timeout{ 0, 100'000 };
        int activity = select(fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (!m_running) break;
        if (activity < 0) {
            if (errno != EINTR)
                std::cerr << "[Client] select error: " << strerror(errno) << "\n";
            continue;
        }
        if (activity == 0) continue;
        ssize_t bytesRead = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) {
            std::cout << "[Client] Server disconnected\n";
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                if (m_socket >= 0) {
                    close(m_socket);
                    m_socket = -1;
                }
            }
            m_connected = false;
            if (m_connectionCallback)
                m_connectionCallback(false);
            continue;
        }
        buffer[bytesRead] = '\0';
        std::string data = leftover + std::string(buffer, bytesRead);
        size_t pos = 0, newline;

        while ((newline = data.find('\n', pos)) != std::string::npos) {
            std::string msg = data.substr(pos, newline - pos);
            if (!msg.empty() && msg.back() == '\r')
                msg.pop_back();
            if (!msg.empty() && m_messageCallback)
                m_messageCallback(msg);
            pos = newline + 1;
        }
        leftover = data.substr(pos);
    }
    std::cout << "[Client] Receive thread exiting\n";
}