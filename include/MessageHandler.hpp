#pragma once
// remote_access_tool/src/MessageHandler.hpp

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>

class TCPHandler;

struct DBusMessageInfo {
    std::string type;
    std::string sender;
    std::string destination;
    std::string path;
    std::string interface;
    std::string member;
    std::string errorName;
    std::string signature;
    std::string serial;
    std::vector<std::string> args;
    std::string serialize() const;
    std::string toDisplayString() const;

    static DBusMessageInfo deserialize(const std::string& data);
    static DBusMessageInfo createHandwritten(
        const std::string& type,
        const std::string& dest,
        const std::string& path,
        const std::string& interface,
        const std::string& member,
        const std::string& args);
};
class MessageHandler {
public:
    static MessageHandler& getInstance();
    void setTCPHandler(TCPHandler* handler) { m_tcpHandler = handler; }
    void rcvMsg(const std::string& clientId, const std::string& message);
    void rcvDBusMsg(const std::string& clientId, const std::string& serializedMsg);
    void sendMsg(const std::string& clientId, const std::string& text);
    void sendDBusMsg(const std::string& clientId, const DBusMessageInfo& dbusMsg);
    std::string getCurrentTime() const;

private:
    MessageHandler();
    MessageHandler(const MessageHandler&) = delete;
    MessageHandler& operator=(const MessageHandler&) = delete;

    void ensureLogsDirectory();
    std::ofstream& getMessageLog(const std::string& clientId);
    std::ofstream& getDbusLog(const std::string& clientId);
    void logToFile(const std::string& clientId, const std::string& direction, const std::string& content);
    void logDBusMessage(const std::string& clientId, const std::string& direction, const DBusMessageInfo& msg);
    TCPHandler* m_tcpHandler = nullptr;
    mutable std::mutex m_logMutex;
    std::map<std::string, std::ofstream> m_messageLogs;
    std::map<std::string, std::ofstream> m_dbusLogs;
};
