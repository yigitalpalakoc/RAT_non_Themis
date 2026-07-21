// client_agent/include/MessageHandler.hpp
#ifndef MESSAGEHANDLER_HPP
#define MESSAGEHANDLER_HPP

#include <string>
#include <functional>
#include <mutex>
#include <map>
#include <fstream>
#include <sstream>

struct DBusMessageInfo;

class MessageHandler {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    
    static MessageHandler& getInstance();
    
    void setMessageCallback(MessageCallback callback) { m_callback = callback; }
    void sendMessage(const std::string& message);
    void receiveMessage(const std::string& message);
    void logTextMessage(const std::string& clientId, const std::string& direction, const std::string& message);
    void logDBusMessage(const std::string& clientId, const std::string& direction, const DBusMessageInfo& info);
    void logDBusMessage(const std::string& clientId, const std::string& direction, const std::string& serializedMsg);
    
    std::string getCurrentTime() const;
    
private:
    MessageHandler();
    void ensureLogsDirectory();
    void logToFile(const std::string& clientId, const std::string& direction, const std::string& message);
    std::ofstream& getMessageLog(const std::string& clientId);
    std::ofstream& getDbusLog(const std::string& clientId);
    
    MessageCallback m_callback;
    std::mutex m_logMutex;
    std::map<std::string, std::ofstream> m_messageLogs;
    std::map<std::string, std::ofstream> m_dbusLogs;
};

#endif // MESSAGEHANDLER_HPP