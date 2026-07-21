#pragma once
// remote_access_tool/src/Client.hpp

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

class Client {
public:
    Client();
    Client(const std::string& id, const std::string& user, const std::string& ip, int port, const std::vector<std::string>& tags);

    void loadFromJson(const nlohmann::json& j);
    const std::string& getId() const { return m_id; }
    const std::string& getUser() const { return m_user; }
    const std::string& getIp() const { return m_ip; }
    int getPort() const { return m_port; }
    const std::vector<std::string>& getTags() const { return m_tags; }
    std::string getSSHTarget() const { return m_user + "@" + m_ip; }
    std::string getDisplayName() const { return m_id + " (" + m_user + "@" + m_ip + ":" + std::to_string(m_port) + ")"; }
    bool hasTag(const std::string& tag) const;
    bool isConnected()  const { return m_isConnected; }
    void setConnected(bool connected);
    void updateLastSeen();
    std::chrono::system_clock::time_point getLastChecked() const { return m_lastChecked; }

private:
    std::string m_id;
    std::string m_user;
    std::string m_ip;
    int m_port = 22;
    std::vector<std::string> m_tags;
    bool m_isConnected = false;
    std::chrono::system_clock::time_point m_lastChecked;
};
