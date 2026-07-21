// remote_access_tool/src/Client.cpp
#include "Client.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <nlohmann/json.hpp>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;

Client::Client() 
    : m_port(22), m_isConnected(false),
        m_lastChecked(std::chrono::system_clock::now()) {}

Client::Client(const std::string& id, const std::string& user, 
    const std::string& ip, int port, const std::vector<std::string>& tags)
        : m_id(id), m_user(user), m_ip(ip), m_port(port), m_tags(tags),
            m_isConnected(false), m_lastChecked(std::chrono::system_clock::now()) {}

void Client::loadFromJson(const json& j) {
    if (j.contains("id"))   m_id   = j["id"].get<std::string>();
    if (j.contains("user")) m_user = j["user"].get<std::string>();
    if (j.contains("ip"))   m_ip   = j["ip"].get<std::string>();
    if (j.contains("port")) m_port = j["port"].get<int>();

    if (j.contains("tags") && j["tags"].is_array()) {
        m_tags.clear();
        for (const auto& tag : j["tags"]) {
            m_tags.push_back(tag.get<std::string>());
        }
    }
}

bool Client::hasTag(const std::string& tag) const {
    return std::find(m_tags.begin(), m_tags.end(), tag) != m_tags.end();
}

void Client::setConnected(bool connected) {
    if (connected != m_isConnected) {
        m_isConnected = connected;
        updateLastSeen();
    }
}

void Client::updateLastSeen() {
    m_lastChecked = std::chrono::system_clock::now();
}