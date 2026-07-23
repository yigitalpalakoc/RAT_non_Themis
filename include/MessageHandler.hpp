#pragma once
// remote_access_tool/src/MessageHandler.hpp

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>

class TCPHandler;

class MessageHandler {
public:
    static MessageHandler& getInstance();
    void setTCPHandler(TCPHandler* handler) { m_tcpHandler = handler; }
    void rcvMsg(const std::string& clientId, const std::string& message);
    void sendMsg(const std::string& clientId, const std::string& text);
    std::string getCurrentTime() const;

private:
    MessageHandler();
    MessageHandler(const MessageHandler&) = delete;
    MessageHandler& operator=(const MessageHandler&) = delete;

    void ensureLogsDirectory();
    std::ofstream& getMessageLog(const std::string& clientId);
    void logToFile(const std::string& clientId, const std::string& direction, const std::string& content);
    TCPHandler* m_tcpHandler = nullptr;
    mutable std::mutex m_logMutex;
    std::map<std::string, std::ofstream> m_messageLogs;
};
