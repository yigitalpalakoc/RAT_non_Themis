// remote_access_tool/src/RulesManager.cpp
#include "RulesManager.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

RulesManager::RulesManager(const std::string& rulesDir)
    : m_rulesDir(rulesDir) {
    ensureDir();
}

void RulesManager::ensureDir() const {
    mkdir(m_rulesDir.c_str(), 0755);
}

std::string RulesManager::configFilePath(const std::string& clientId) const {
    return m_rulesDir + "/" + clientId + ".json";
}

bool RulesManager::hasConfigFile(const std::string& clientId) const {
    struct stat st;
    return stat(configFilePath(clientId).c_str(), &st) == 0;
}

MsgConfig RulesManager::fromJson(const std::string& jsonStr) const {
    MsgConfig cfg;
    try {
        json j = json::parse(jsonStr);

        if (j.contains("dbus_filters") && j["dbus_filters"].is_array()) {
            for (const auto& item : j["dbus_filters"]) {
                DbusFilter f;
                f.name    = item.value("name", "");
                f.bus     = item.value("bus", "");
                f.match   = item.value("match", "");
                f.log     = item.value("log", false);
                f.forward = item.value("forward", false);
                f.enabled = item.value("enabled", true);

                if (item.contains("types") && item["types"].is_array()) {
                    for (const auto& t : item["types"])
                        f.types.push_back(t.get<std::string>());
                }

                if (!f.name.empty())
                    cfg.filters.push_back(f);
            }
        }

        if (j.contains("global_settings") && j["global_settings"].is_object()) {
            const auto& gs = j["global_settings"];
            cfg.settings.default_log      = gs.value("default_log", false);
            cfg.settings.default_forward  = gs.value("default_forward", false);
            cfg.settings.log_file         = gs.value("log_file", "");
            cfg.settings.max_message_size = gs.value("max_message_size", 0);
        }
    } catch (const json::exception& e) {
        std::cerr << "[RulesManager] JSON parse error: " << e.what() << std::endl;
    }
    return cfg;
}

std::string RulesManager::toJson(const MsgConfig& cfg) const {
    json j;

    json filters_arr = json::array();
    for (const auto& f : cfg.filters) {
        json obj;
        obj["name"]    = f.name;
        obj["bus"]     = f.bus;
        obj["match"]   = f.match;
        obj["log"]     = f.log;
        obj["forward"] = f.forward;
        obj["enabled"] = f.enabled;

        json types_arr = json::array();
        for (const auto& t : f.types)
            types_arr.push_back(t);
        obj["types"] = types_arr;

        filters_arr.push_back(obj);
    }
    j["dbus_filters"] = filters_arr;

    json gs;
    gs["default_log"]      = cfg.settings.default_log;
    gs["default_forward"]  = cfg.settings.default_forward;
    gs["log_file"]         = cfg.settings.log_file;
    gs["max_message_size"] = cfg.settings.max_message_size;
    j["global_settings"]   = gs;

    return j.dump(4);
}

void RulesManager::ensureCached(const std::string& clientId) {
    if (m_cache.find(clientId) == m_cache.end())
        loadConfigLocked(clientId);
}

bool RulesManager::loadConfigLocked(const std::string& clientId) {
    std::string path = configFilePath(clientId);
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[RulesManager] No config file for client '" << clientId << "' – starting with empty config.\n";
        m_cache[clientId] = MsgConfig{};
        return false;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    m_cache[clientId] = fromJson(buf.str());
    std::cout << "[RulesManager] Loaded " << m_cache[clientId].filters.size()
              << " filter(s) for client '" << clientId << "'\n";
    return true;
}

bool RulesManager::loadConfig(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return loadConfigLocked(clientId);
}

bool RulesManager::saveConfig(const std::string& clientId) {
    ensureDir();
    std::string path = configFilePath(clientId);
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[RulesManager] Cannot write to " << path << "\n";
        return false;
    }
    f << toJson(m_cache[clientId]);
    return true;
}

std::vector<DbusFilter> RulesManager::listFilters(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    return m_cache[clientId].filters;
}

bool RulesManager::addFilter(const std::string& clientId, const DbusFilter& filter) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    auto& filters = m_cache[clientId].filters;

    for (const auto& f : filters) {
        if (f.name == filter.name) {
            std::cerr << "[RulesManager] Filter '" << filter.name << "' already exists for client '" << clientId << "'\n";
            return false;
        }
    }

    filters.push_back(filter);
    return saveConfig(clientId);
}

bool RulesManager::removeFilter(const std::string& clientId, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    auto& filters = m_cache[clientId].filters;

    for (auto it = filters.begin(); it != filters.end(); ++it) {
        if (it->name == name) {
            filters.erase(it);
            return saveConfig(clientId);
        }
    }

    std::cerr << "[RulesManager] Filter '" << name << "' not found for client '" << clientId << "'\n";
    return false;
}

bool RulesManager::editFilter(const std::string& clientId, const std::string& name, const DbusFilter& updated) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    auto& filters = m_cache[clientId].filters;

    for (auto& f : filters) {
        if (f.name == name) {
            f      = updated;
            f.name = name;
            return saveConfig(clientId);
        }
    }

    std::cerr << "[RulesManager] Filter '" << name << "' not found for client '" << clientId << "'\n";
    return false;
}

bool RulesManager::setFilterEnabled(const std::string& clientId, const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    for (auto& f : m_cache[clientId].filters) {
        if (f.name == name) {
            f.enabled = enabled;
            return saveConfig(clientId);
        }
    }
    std::cerr << "[RulesManager] Filter '" << name << "' not found for client '" << clientId << "'\n";
    return false;
}

GlobalSettings RulesManager::getSettings(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    return m_cache[clientId].settings;
}

bool RulesManager::setSettings(const std::string& clientId, const GlobalSettings& s) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    m_cache[clientId].settings = s;
    return saveConfig(clientId);
}

std::string RulesManager::serializeConfig(const std::string& clientId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    ensureCached(clientId);
    return toJson(m_cache[clientId]);
}
