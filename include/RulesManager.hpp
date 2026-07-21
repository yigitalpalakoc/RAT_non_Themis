#pragma once
// remote_access_tool/src/RulesManager.hpp

#include <string>
#include <vector>
#include <map>
#include <mutex>

struct DbusFilter {
    std::string name;
    std::string bus;
    std::string match;
    std::vector<std::string> types;
    bool log = false;
    bool forward = false;
    bool enabled = true;
};

struct GlobalSettings {
    bool default_log = false;
    bool default_forward = false;
    std::string log_file;
    int max_message_size = 0;
};

struct MsgConfig {
    std::vector<DbusFilter> filters;
    GlobalSettings settings;
};

class RulesManager {
public:
    explicit RulesManager(const std::string& rulesDir = "../rules");
    std::vector<DbusFilter> listFilters(const std::string& clientId);
    bool addFilter(const std::string& clientId, const DbusFilter& filter);
    bool removeFilter(const std::string& clientId, const std::string& name);
    bool editFilter(const std::string& clientId, const std::string& name, const DbusFilter& updated);
    bool setFilterEnabled(const std::string& clientId, const std::string& name, bool enabled);
    GlobalSettings getSettings(const std::string& clientId);
    bool setSettings(const std::string& clientId, const GlobalSettings& s);
    bool loadConfig(const std::string& clientId);
    std::string serializeConfig(const std::string& clientId);
    bool hasConfigFile(const std::string& clientId) const;

private:
    void ensureDir()const;
    std::string configFilePath(const std::string& clientId) const;
    void ensureCached(const std::string& clientId);
    bool loadConfigLocked(const std::string& clientId);
    bool saveConfig(const std::string& clientId);
    MsgConfig fromJson(const std::string& jsonStr) const;
    std::string toJson (const MsgConfig& cfg) const;
    std::string m_rulesDir;
    std::map<std::string, MsgConfig> m_cache;
    mutable std::mutex m_cacheMutex;
};
