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

static constexpr const char* DBUS_PREFIX     = "DBUS:";
static constexpr size_t      DBUS_PREFIX_LEN = 5;

std::string DBusMessageInfo::serialize() const {
    std::stringstream ss;
    ss << "type:" << type << "|";
    ss << "sender:" << sender << "|";
    ss << "dest:" << destination << "|";
    ss << "path:" << path << "|";
    ss << "interface:" << interface << "|";
    ss << "member:" << member << "|";
    ss << "error:" << errorName << "|";
    ss << "sig:" << signature << "|";
    ss << "serial:" << serial << "|";
    ss << "args:";
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) ss << ",";
        ss << args[i];
    }
    return ss.str();
}

DBusMessageInfo DBusMessageInfo::deserialize(const std::string& data) {
    DBusMessageInfo info;
    std::stringstream ss(data);
    std::string token;

    while (std::getline(ss, token, '|')) {
        size_t colon = token.find(':');
        if (colon == std::string::npos) continue;

        std::string key   = token.substr(0, colon);
        std::string value = token.substr(colon + 1);

        if      (key == "type")   info.type        = value;
        else if (key == "sender") info.sender       = value;
        else if (key == "dest")   info.destination  = value;
        else if (key == "path")   info.path         = value;
        else if (key == "interface") info.interface = value;
        else if (key == "member") info.member       = value;
        else if (key == "error")  info.errorName    = value;
        else if (key == "sig")    info.signature    = value;
        else if (key == "serial") info.serial       = value;
        else if (key == "args") {
            std::stringstream argsStream(value);
            std::string arg;
            while (std::getline(argsStream, arg, ','))
                if (!arg.empty()) info.args.push_back(arg);
        }
    }
    return info;
}

DBusMessageInfo DBusMessageInfo::createHandwritten(
        const std::string& type,
        const std::string& dest,
        const std::string& path,
        const std::string& interface,
        const std::string& member,
        const std::string& args) {

    DBusMessageInfo info;
    info.type        = type;
    info.destination = dest;
    info.path        = path;
    info.interface   = interface;
    info.member      = member;
    info.sender      = "plugin-server";

    if (!args.empty()) {
        std::stringstream ss(args);
        std::string arg;
        while (std::getline(ss, arg, ','))
            if (!arg.empty()) info.args.push_back(arg);
    }

    return info;
}

std::string DBusMessageInfo::toDisplayString() const {
    std::stringstream ss;
    ss << type << " ";
    if (!destination.empty()) ss << "to:" << destination << " ";
    if (!path.empty())        ss << "path:" << path << " ";
    if (!interface.empty())   ss << "iface:" << interface << " ";
    if (!member.empty())      ss << "member:" << member << " ";
    if (!args.empty()) {
        ss << "args:[";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) ss << ",";
            ss << args[i];
        }
        ss << "]";
    }
    return ss.str();
}

MessageHandler::MessageHandler() : m_tcpHandler(nullptr) {
    ensureLogsDirectory();
}

MessageHandler& MessageHandler::getInstance() {
    static MessageHandler instance;
    return instance;
}

void MessageHandler::ensureLogsDirectory() {
    mkdir("logs",      0755);
    mkdir("logs/dbus", 0755);
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

std::ofstream& MessageHandler::getDbusLog(const std::string& clientId) {
    auto it = m_dbusLogs.find(clientId);
    if (it != m_dbusLogs.end())
        return it->second;

    std::string filename = "logs/dbus/" + clientId + "_dbus.log";
    m_dbusLogs[clientId].open(filename, std::ios::app);
    if (!m_dbusLogs[clientId].is_open())
        std::cerr << "[MessageHandler] Cannot open dbus log file: " << filename << "\n";
    return m_dbusLogs[clientId];
}

void MessageHandler::logToFile(const std::string& clientId, const std::string& direction, const std::string& content) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    auto& f = getMessageLog(clientId);
    if (f.is_open())
        f << "[" << getCurrentTime() << "] [" << direction << "] " << content << "\n" << std::flush;
}

void MessageHandler::logDBusMessage(const std::string& clientId, const std::string& direction, const DBusMessageInfo& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    auto& f = getDbusLog(clientId);
    if (f.is_open())
        f << "[" << getCurrentTime() << "] [" << direction << "] " << msg.serialize() << "\n" << std::flush;

    std::cout << "[" << getCurrentTime() << "] [DBus:" << direction << "] " << clientId << ": " << msg.toDisplayString() << std::endl;
}

void MessageHandler::rcvMsg(const std::string& clientId, const std::string& message) {
    if (message.compare(0, DBUS_PREFIX_LEN, DBUS_PREFIX) == 0) {
        rcvDBusMsg(clientId, message.substr(DBUS_PREFIX_LEN));
    } else {
        std::cout << "[" << getCurrentTime() << "] Received from " << clientId << ": " << message << std::endl;
        logToFile(clientId, "RECEIVED", message);
    }
}

void MessageHandler::rcvDBusMsg(const std::string& clientId, const std::string& serializedMsg) {
    DBusMessageInfo info = DBusMessageInfo::deserialize(serializedMsg);
    logDBusMessage(clientId, "RECEIVED", info);
}

void MessageHandler::sendMsg(const std::string& clientId, const std::string& text) {
    if (!m_tcpHandler) {
        std::cerr << "[" << getCurrentTime() << "] ERROR: TCP handler not set\n";
        return;
    }

    if (text.compare(0, 5, "dbus ") == 0) {
        std::istringstream iss(text);
        std::string cmd, type, dest, path, iface, member, args;
        iss >> cmd >> type >> dest >> path >> iface >> member;
        std::getline(iss, args);
        if (!args.empty() && args.front() == ' ')
            args = args.substr(1);

        DBusMessageInfo dbusMsg = DBusMessageInfo::createHandwritten(type, dest, path, iface, member, args);

        std::cout << "[" << getCurrentTime() << "] Sending DBus message to " << clientId << std::endl;
        logDBusMessage(clientId, "SENT", dbusMsg);

        m_tcpHandler->sendToClient(clientId, std::string(DBUS_PREFIX) + dbusMsg.serialize());
    } else {
        std::cout << "[" << getCurrentTime() << "] Sending to " << clientId << ": " << text << std::endl;
        logToFile(clientId, "SENT", text);
        m_tcpHandler->sendToClient(clientId, text);
    }
}

void MessageHandler::sendDBusMsg(const std::string& clientId, const DBusMessageInfo& dbusMsg) {
    if (!m_tcpHandler) {
        std::cerr << "[" << getCurrentTime() << "] ERROR: TCP handler not set\n";
        return;
    }
    logDBusMessage(clientId, "SENT", dbusMsg);
    m_tcpHandler->sendToClient(clientId, std::string(DBUS_PREFIX) + dbusMsg.serialize());
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
