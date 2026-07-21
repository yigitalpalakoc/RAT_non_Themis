// client_agent/src/MessageHandler.cpp
#include "MessageHandler.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sys/stat.h>
#include "Agent.hpp"

MessageHandler& MessageHandler::getInstance() {
    static MessageHandler instance;
    return instance;
}

MessageHandler::MessageHandler() {
    ensureLogsDirectory();
}

void MessageHandler::ensureLogsDirectory() {
    mkdir("logs", 0755);
    mkdir("logs/dbus", 0755);
}

std::ofstream& MessageHandler::getMessageLog(const std::string& clientId) {
    auto it = m_messageLogs.find(clientId);
    if (it != m_messageLogs.end())
        return it->second;

    std::string path = "logs/" + clientId + "_messages.log";
    m_messageLogs[clientId].open(path, std::ios::app);
    if (!m_messageLogs[clientId].is_open())
        std::cerr << "[MessageHandler] Cannot open log: " << path << "\n";
    return m_messageLogs[clientId];
}

std::ofstream& MessageHandler::getDbusLog(const std::string& clientId) {
    auto it = m_dbusLogs.find(clientId);
    if (it != m_dbusLogs.end())
        return it->second;

    std::string path = "logs/dbus/" + clientId + "_dbus.log";
    m_dbusLogs[clientId].open(path, std::ios::app);
    if (!m_dbusLogs[clientId].is_open())
        std::cerr << "[MessageHandler] Cannot open dbus log: " << path << "\n";
    return m_dbusLogs[clientId];
}

void MessageHandler::logToFile(const std::string& clientId, const std::string& direction, const std::string& content) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    auto& f = getMessageLog(clientId);
    if (f.is_open())
        f << "[" << getCurrentTime() << "] [" << direction << "] " << content << "\n" << std::flush;
}

void MessageHandler::logTextMessage(const std::string& clientId, const std::string& direction, const std::string& message) {
    logToFile(clientId, direction, message);
}

void MessageHandler::logDBusMessage(const std::string& clientId, const std::string& direction, const DBusMessageInfo& info) {
    std::lock_guard<std::mutex> lock(m_logMutex);

    std::string ts = getCurrentTime();
    std::cout << "[" << ts << "] [DBus:" << direction << "] " << clientId << ": " << info.type;
    if (!info.destination.empty()) std::cout << " dest:" << info.destination;
    if (!info.interface.empty())   std::cout << " iface:" << info.interface;
    if (!info.member.empty())      std::cout << " member:" << info.member;
    std::cout << "\n";

    auto& f = getDbusLog(clientId);
    if (f.is_open())
        f << "[" << ts << "] [" << direction << "] " << info.serialize() << "\n" << std::flush;
}

void MessageHandler::logDBusMessage(const std::string& clientId, const std::string& direction, const std::string& serializedMsg) {
    std::lock_guard<std::mutex> lock(m_logMutex);

    std::string ts = getCurrentTime();
    size_t previewLen = std::min(serializedMsg.size(), static_cast<size_t>(100));
    std::cout << "[" << ts << "] [DBus:" << direction << "] " << clientId << ": " << serializedMsg.substr(0, previewLen) << (serializedMsg.size() > 100 ? "..." : "") << "\n";

    auto& f = getDbusLog(clientId);
    if (f.is_open())
        f << "[" << ts << "] [" << direction << "] " << serializedMsg << "\n" << std::flush;
}

void MessageHandler::sendMessage(const std::string& message) {
    std::string ts = getCurrentTime();
    std::cout << "[" << ts << "] [Sent] " << message << "\n";
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        auto& f = getMessageLog("_outbound");
        if (f.is_open())
            f << "[" << ts << "] [SENT] " << message << "\n" << std::flush;
    }
    if (m_callback)
        m_callback(message);
}

void MessageHandler::receiveMessage(const std::string& message) {
    std::cout << "[" << getCurrentTime() << "] [Received] " << message << "\n";
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