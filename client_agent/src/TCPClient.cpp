// client_agent/src/TCPClient.cpp
#include "TCPClient.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <cerrno>

TCPClient::TCPClient()  = default;
TCPClient::~TCPClient() { disconnect(); }

bool TCPClient::connect(const std::string& host, int port, const std::string& clientId) {
    disconnect();
    m_host = host; m_port = port; m_clientId = clientId;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "[TCP] socket: " << strerror(errno) << "\n";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[TCP] Invalid address: " << host << "\n";
        close(sock); return false;
    }

    std::cout << "[TCP] Connecting to " << host << ":" << port << "...\n";
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[TCP] Connection failed: " << strerror(errno) << "\n";
        close(sock); return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        m_socket = sock;
    }
    m_running   = true;
    m_connected = true;

    std::string hello = "ID:" + m_clientId + "\n";
    ::send(sock, hello.c_str(), hello.size(), MSG_NOSIGNAL);

    m_recvThread = std::thread(&TCPClient::receiveLoop, this);
    std::cout << "[TCP] Connected as '" << m_clientId << "'\n";
    if (m_connectionCallback) m_connectionCallback(true);
    return true;
}

void TCPClient::disconnect() {
    // Exchange returns the old value — lets us fire the callback exactly once.
    bool wasConnected = m_connected.exchange(false);
    m_running = false;

    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_socket >= 0) {
            shutdown(m_socket, SHUT_RDWR);
            close(m_socket);
            m_socket = -1;
        }
    }
    if (m_recvThread.joinable()) m_recvThread.join();
    if (wasConnected && m_connectionCallback) m_connectionCallback(false);
}

bool TCPClient::sendMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_socketMutex);
    if (!m_connected || m_socket < 0) return false;
    std::string data = msg + "\n";
    ssize_t sent = ::send(m_socket, data.c_str(), data.size(), MSG_NOSIGNAL);
    if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        m_connected = false;
        return false;
    }
    return sent > 0;
}

void TCPClient::receiveLoop() {
    char        buf[kBufSize];
    std::string leftover;

    while (m_running) {
        int fd;
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_socket < 0) break;
            fd = m_socket;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv{0, 100'000};  // 100 ms — keeps the loop responsive to m_running
        int n = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (!m_running) break;
        if (n <= 0) continue;   // timeout or EINTR

        ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
        if (r <= 0) {
            std::cout << "[TCP] Server disconnected\n";
            m_connected = false;
            if (m_connectionCallback) m_connectionCallback(false);
            break;
        }

        // Split the byte stream on newlines and deliver each complete message.
        std::string data = leftover + std::string(buf, r);
        size_t pos = 0, nl;
        while ((nl = data.find('\n', pos)) != std::string::npos) {
            std::string line = data.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && m_messageCallback) m_messageCallback(line);
            pos = nl + 1;
        }
        leftover = data.substr(pos);
    }
}
