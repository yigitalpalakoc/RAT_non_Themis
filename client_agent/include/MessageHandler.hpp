// client_agent/include/MessageHandler.hpp
#ifndef MESSAGEHANDLER_HPP
#define MESSAGEHANDLER_HPP

#include <string>
#include <functional>
#include <mutex>
#include <map>
#include <fstream>
#include <sstream>

class MessageHandler {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    
    static MessageHandler& getInstance();
    void setMessageCallback(MessageCallback callback) { m_callback = callback; }
    void sendMessage(const std::string& message);
    void receiveMessage(const std::string& message);
    void logToFile(const std::string& clientId, const std::string& direction, const std::string& message);
    std::string getCurrentTime() const;
    
private:
    MessageHandler();
    void ensureLogsDirectory();
    std::ofstream& getMessageLog(const std::string& clientId);
    
    MessageCallback m_callback;
    std::mutex m_logMutex;
    std::map<std::string, std::ofstream> m_messageLogs;
};

#endif // MESSAGEHANDLER_HPP