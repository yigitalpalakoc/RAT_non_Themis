// remote_access_tool/src/MessageHandler.cpp
#include "MessageHandler.hpp"
#include "TCPHandler.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

MessageHandler::MessageHandler() : m_tcpHandler(nullptr) {
    ensureLogsDirectory();
}

MessageHandler& MessageHandler::getInstance() {
    static MessageHandler instance;
    return instance;
}

void MessageHandler::ensureLogsDirectory() {
    mkdir("logs",      0755);
}

std::ofstream& MessageHandler::getMessageLog(const std::string& clientId) {
    auto it = m_messageLogs.find(clientId);
    if (it != m_messageLogs.end())
        return it->second;

    std::string filename = "logs/" + clientId + "_messages.log";
    m_messageLogs[clientId].open(filename, std::ios::app);
    if (!m_messageLogs[clientId].is_open())
        std::cerr << "[MessageHandler] Cannot open log file: " << filename << "\n";
    return m_messageLogs[clientId];
}

void MessageHandler::logToFile(const std::string& clientId, const std::string& direction, const std::string& content) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    auto& f = getMessageLog(clientId);
    if (f.is_open())
        f << "[" << getCurrentTime() << "] [" << direction << "] " << content << "\n" << std::flush;
}

void MessageHandler::rcvMsg(const std::string& clientId, const std::string& message) {
    std::cout << "[" << getCurrentTime() << "] Received from " << clientId << ": " << message << std::endl;
    logToFile(clientId, "RECEIVED", message);
}

void MessageHandler::sendMsg(const std::string& clientId, const std::string& text) {
    if (!m_tcpHandler) {
        std::cerr << "[" << getCurrentTime() << "] ERROR: TCP handler not set\n";
        return;
    }
    std::cout << "[" << getCurrentTime() << "] Sending to " << clientId << ": " << text << std::endl;
    logToFile(clientId, "SENT", text);
    m_tcpHandler->sendToClient(clientId, text);
}

std::string MessageHandler::getCurrentTime() const {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}
