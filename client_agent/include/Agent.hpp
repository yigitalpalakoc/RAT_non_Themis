// client_agent/include/Agent.hpp
#ifndef AGENT_HPP
#define AGENT_HPP

#include "TCPClient.hpp"
#include "MessageHandler.hpp"
#include "LockfileManager.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <shared_mutex>
#include <dbus/dbus.h>
#include <nlohmann/json.hpp> 

struct ServerConfig {
    std::string ip;
    int port;
    bool isValid() const { return !ip.empty() && port > 0; }
};

// D-Bus filter structure
struct DBusFilter {
    std::string name;
    std::string bus;           // "system", "session", or "both"
    std::string match;         // D-Bus match rule
    std::vector<std::string> types;  // Message types to capture
    bool log;                  // Whether to log this message
    bool forward;              // Whether to forward to server
    bool enabled;              // Whether this filter is active

    bool matches(DBusMessage* msg, const std::string& busType) const;
};

// Global filter settings
struct FilterSettings {
    bool defaultLog = false;
    bool defaultForward = false;
    std::string logFile;
    size_t maxMessageSize = 4096;
};

// D-Bus message structure
struct DBusMessageInfo {
    std::string type;
    std::string sender;
    std::string destination;
    std::string path;
    std::string interface;
    std::string member;
    std::string signature;
    std::vector<std::string> args;
    std::string errorName;
    std::string serial;
    std::string busType;

    std::string serialize() const;
    static DBusMessageInfo fromDBusMessage(DBusMessage* msg, const std::string& busType);
};

class Agent {
public:
    Agent();
    ~Agent();

    bool initialize(const std::string& configPath, const std::string& msgConfigPath, const std::string& clientId);
    void start();
    void stop();
    bool isRunning() const { return m_running; }
    void sendTextMessage(const std::string& text);
    void startStdinReader();
    void stopStdinReader();

private:
    using json = nlohmann::json;
    
    bool loadServerConfig(const std::string& configPath);
    bool loadMsgConfig(const std::string& configPath);

    // D-Bus
    bool initDBus();
    void addDBusMatches();
    void removeDBusMatches(const std::vector<DBusFilter>& filters);
    void dbusMonitorThread();
    void handleDBusMessage(DBusMessage* msg, const std::string& busType);
    bool parseAndInjectDBusMessage(const std::string& dbusCommand);
    bool parseFiltersFromJson(const nlohmann::json& root, std::vector<DBusFilter>& filters, FilterSettings& settings);
    void printFilterStats() const;

    // TCP incoming
    void onTCPMessageReceived(const std::string& message);

    // MSG_CONFIG handling
    bool applyMsgConfig(const std::string& jsonContent);
    bool saveMsgConfig(const std::string& jsonContent);

    bool m_receivingMsgConfig{false};
    std::string m_msgConfigBuffer;
    int m_msgConfigBraceDepth{0};   // kept for potential multi-line parsing
    std::string m_msgConfigPath;

    std::unique_ptr<TCPClient> m_tcpClient;
    LockfileManager m_lockfileManager;
    MessageHandler& m_messageHandler;

    std::atomic<bool> m_running;
    std::thread       m_dbusMonitorThread;
    void stdinReaderThread();
    std::thread m_stdinReaderThread;
    int m_inputPipe[2];

    std::string  m_clientId;
    ServerConfig m_serverConfig;

    std::vector<DBusFilter> m_filters;
    FilterSettings m_filterSettings;
    std::atomic<uint64_t>   m_matchedCount;
    std::atomic<uint64_t>   m_forwardedCount;
    std::atomic<uint64_t>   m_loggedCount;

    DBusConnection* m_systemBus;
    DBusConnection* m_sessionBus;
    DBusError       m_dbusError;
    mutable std::shared_mutex m_filtersMutex;
};

#endif // AGENT_HPP