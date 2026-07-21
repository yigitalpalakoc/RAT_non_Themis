// client_agent/src/Agent.cpp
#include "Agent.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/select.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <csignal> 

using json = nlohmann::json;

extern volatile sig_atomic_t g_shutdown_flag;

bool DBusFilter::matches(DBusMessage* msg, const std::string& busType) const {
    if (!enabled) return false;
    if (bus != "both" && bus != busType) return false;
    std::string typeStr;
    switch (dbus_message_get_type(msg)) {
        case DBUS_MESSAGE_TYPE_METHOD_CALL:   typeStr = "method_call";   break;
        case DBUS_MESSAGE_TYPE_METHOD_RETURN: typeStr = "method_return"; break;
        case DBUS_MESSAGE_TYPE_ERROR:         typeStr = "error";         break;
        case DBUS_MESSAGE_TYPE_SIGNAL:        typeStr = "signal";        break;
        default: return false;
    }
    if (std::find(types.begin(), types.end(), typeStr) == types.end())
        return false;
    return true;
}

std::string DBusMessageInfo::serialize() const {
    std::stringstream ss;
    ss << "type:"      << type        << "|"
       << "sender:"    << sender      << "|"
       << "dest:"      << destination << "|"
       << "path:"      << path        << "|"
       << "interface:" << interface   << "|"
       << "member:"    << member      << "|"
       << "error:"     << errorName   << "|"
       << "sig:"       << signature   << "|"
       << "serial:"    << serial      << "|"
       << "bus:"       << busType     << "|"
       << "args:";
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) ss << ",";
        ss << args[i];
    }
    return ss.str();
}

DBusMessageInfo DBusMessageInfo::fromDBusMessage(DBusMessage* msg, const std::string& bt) {
    DBusMessageInfo info;
    info.busType = bt;

    switch (dbus_message_get_type(msg)) {
        case DBUS_MESSAGE_TYPE_METHOD_CALL:   info.type = "method_call";   break;
        case DBUS_MESSAGE_TYPE_METHOD_RETURN: info.type = "method_return"; break;
        case DBUS_MESSAGE_TYPE_ERROR:         info.type = "error";         break;
        case DBUS_MESSAGE_TYPE_SIGNAL:        info.type = "signal";        break;
        default:                              info.type = "unknown";
    }

    auto safe = [](const char* s) -> std::string { return s ? s : ""; };
    info.sender      = safe(dbus_message_get_sender(msg));
    info.destination = safe(dbus_message_get_destination(msg));
    info.path        = safe(dbus_message_get_path(msg));
    info.interface   = safe(dbus_message_get_interface(msg));
    info.member      = safe(dbus_message_get_member(msg));
    info.errorName   = safe(dbus_message_get_error_name(msg));
    info.signature   = safe(dbus_message_get_signature(msg));
    info.serial      = std::to_string(dbus_message_get_serial(msg));

    DBusMessageIter iter;
    dbus_message_iter_init(msg, &iter);
    while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
        std::stringstream argStr;
        int argType = dbus_message_iter_get_arg_type(&iter);
        switch (argType) {
            case DBUS_TYPE_STRING:  { const char*    v; dbus_message_iter_get_basic(&iter, &v); argStr << "string:" << v; break; }
            case DBUS_TYPE_INT32:   { dbus_int32_t   v; dbus_message_iter_get_basic(&iter, &v); argStr << "int32:"  << v; break; }
            case DBUS_TYPE_UINT32:  { dbus_uint32_t  v; dbus_message_iter_get_basic(&iter, &v); argStr << "uint32:" << v; break; }
            case DBUS_TYPE_BOOLEAN: { dbus_bool_t    v; dbus_message_iter_get_basic(&iter, &v); argStr << "bool:"   << (v ? "true" : "false"); break; }
            case DBUS_TYPE_DOUBLE:  { double         v; dbus_message_iter_get_basic(&iter, &v); argStr << "double:" << v; break; }
            case DBUS_TYPE_BYTE:    { unsigned char  v; dbus_message_iter_get_basic(&iter, &v); argStr << "byte:"   << (int)v; break; }
            default:                argStr << "type:" << (char)argType;
        }
        info.args.push_back(argStr.str());
        dbus_message_iter_next(&iter);
    }

    return info;
}

Agent::Agent()
    : m_tcpClient(std::make_unique<TCPClient>())
    , m_messageHandler(MessageHandler::getInstance())
    , m_running(false)
    , m_matchedCount(0), m_forwardedCount(0), m_loggedCount(0)
    , m_systemBus(nullptr), m_sessionBus(nullptr)
    , m_receivingMsgConfig(false), m_msgConfigBraceDepth(0)
    , m_inputPipe{-1, -1} {
    dbus_error_init(&m_dbusError);
}

Agent::~Agent() {
    stop();
    if (m_systemBus)  dbus_connection_unref(m_systemBus);
    if (m_sessionBus) dbus_connection_unref(m_sessionBus);
    dbus_error_free(&m_dbusError);
}

void Agent::startStdinReader() {
    if (m_stdinReaderThread.joinable()) return;
    if (pipe(m_inputPipe) < 0) {
        std::cerr << "[Agent] Failed to create input pipe\n";
        return;
    }
    int flags = fcntl(m_inputPipe[0], F_GETFL, 0);
    fcntl(m_inputPipe[0], F_SETFL, flags | O_NONBLOCK);
    m_stdinReaderThread = std::thread(&Agent::stdinReaderThread, this);
}

void Agent::stopStdinReader() {
    if (m_stdinReaderThread.joinable()) {
        if (m_inputPipe[1] != -1) {
            char x = 'x';
            write(m_inputPipe[1], &x, 1);
        }
        m_stdinReaderThread.join();
    }
    if (m_inputPipe[0] != -1) { close(m_inputPipe[0]); m_inputPipe[0] = -1; }
    if (m_inputPipe[1] != -1) { close(m_inputPipe[1]); m_inputPipe[1] = -1; }
}

void Agent::stdinReaderThread() {
    std::cout << "[Agent] Stdin reader thread started\n";
    fd_set readfds;

    while (m_running && !g_shutdown_flag) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        if (m_inputPipe[0] != -1) FD_SET(m_inputPipe[0], &readfds);
        int maxFd = std::max(STDIN_FILENO, m_inputPipe[0]) + 1;

        int activity = select(maxFd, &readfds, nullptr, nullptr, nullptr);
        if (activity < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (m_inputPipe[0] != -1 && FD_ISSET(m_inputPipe[0], &readfds)) {
            char buf[16];
            read(m_inputPipe[0], buf, sizeof(buf));
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                if (std::cin.eof()) {
                    std::cout << "\nEOF received. Shutting down...\n";
                    g_shutdown_flag = 1;
                }
                break;
            }
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            auto last = line.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) line.erase(last + 1);

            if (!line.empty())
                sendTextMessage(line);
        }
    }

    std::cout << "[Agent] Stdin reader thread stopped\n";
}

void Agent::sendTextMessage(const std::string& text) {
    if (m_tcpClient->isConnected()) {
        m_tcpClient->sendMessage(text);
        m_messageHandler.logTextMessage(m_clientId, "SENT", text);
        std::cout << "[" << m_messageHandler.getCurrentTime() << "] Sent: " << text << "\n";
    } else {
        std::cout << "Not connected to server.\n";
    }
}

static std::string readFirstExisting(const std::vector<std::string>& candidates, std::string* usedPath = nullptr) {
    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::stringstream buf;
        buf << f.rdbuf();
        if (usedPath) *usedPath = path;
        return buf.str();
    }
    return {};
}

bool Agent::parseFiltersFromJson(const json& root, std::vector<DBusFilter>& filters, FilterSettings& settings) {
    if (root.contains("dbus_filters") && root["dbus_filters"].is_array()) {
        for (const auto& entry : root["dbus_filters"]) {
            DBusFilter f;
            f.name    = entry.value("name",    "");
            f.bus     = entry.value("bus",     "");
            f.match   = entry.value("match",   "");
            f.log     = entry.value("log",     false);
            f.forward = entry.value("forward", false);
            f.enabled = entry.value("enabled", true);

            if (entry.contains("types") && entry["types"].is_array()) {
                for (const auto& t : entry["types"])
                    f.types.push_back(t.get<std::string>());
            }
            filters.push_back(f);
        }
    }

    if (root.contains("global_settings") && root["global_settings"].is_object()) {
        const auto& gs = root["global_settings"];
        settings.defaultLog     = gs.value("default_log",      settings.defaultLog);
        settings.defaultForward = gs.value("default_forward",  settings.defaultForward);
        settings.logFile        = gs.value("log_file",         settings.logFile);
        settings.maxMessageSize = gs.value("max_message_size", settings.maxMessageSize);
    }

    return true;
}

bool Agent::loadServerConfig(const std::string& configPath) {
    std::string usedPath;
    std::string content = readFirstExisting(
        { configPath, "./connection_config.json", "../connection_config.json" },
        &usedPath);

    if (content.empty()) {
        std::cerr << "[Agent] Could not open connection config\n";
        return false;
    }
    std::cout << "[Agent] Using config: " << usedPath << "\n";

    json root;
    try {
        root = json::parse(content);
    } catch (const json::parse_error& e) {
        std::cerr << "[Agent] Failed to parse connection config: " << e.what() << "\n";
        return false;
    }

    if (!root.contains("connection_config") ||
        !root["connection_config"].is_array() ||
        root["connection_config"].empty()) {
        std::cerr << "[Agent] No 'connection_config' entries\n";
        return false;
    }

    const auto& cfg = root["connection_config"][0];
    m_serverConfig.ip   = cfg.value("server_ip",   "");
    m_serverConfig.port = cfg.value("server_port",  0);

    if (!m_serverConfig.isValid()) {
        std::cerr << "[Agent] Invalid server configuration\n";
        return false;
    }

    std::cout << "[Agent] Server: " << m_serverConfig.ip
              << ":" << m_serverConfig.port << "\n";
    return true;
}

bool Agent::loadMsgConfig(const std::string& configPath) {
    std::string usedPath;
    std::string content = readFirstExisting(
        { configPath, "./msg_config.json", "../msg_config.json" },
        &usedPath);

    if (content.empty()) {
        std::cout << "[Agent] No msg config found — using defaults (no filtering)\n";
        return true;
    }
    std::cout << "[Agent] Using msg config: " << usedPath << "\n";

    json root;
    try {
        root = json::parse(content);
    } catch (const json::parse_error& e) {
        std::cerr << "[Agent] Failed to parse msg config: " << e.what() << "\n";
        return false;
    }

    parseFiltersFromJson(root, m_filters, m_filterSettings);
    std::cout << "[Agent] Loaded " << m_filters.size() << " filters\n";
    return true;
}

bool Agent::initDBus() {
    m_systemBus = dbus_bus_get(DBUS_BUS_SYSTEM, &m_dbusError);
    if (dbus_error_is_set(&m_dbusError)) {
        std::cerr << "[DBus] System bus error: " << m_dbusError.message << "\n";
        dbus_error_free(&m_dbusError);
    }

    m_sessionBus = dbus_bus_get(DBUS_BUS_SESSION, &m_dbusError);
    if (dbus_error_is_set(&m_dbusError)) {
        std::cerr << "[DBus] Session bus error: " << m_dbusError.message << "\n";
        dbus_error_free(&m_dbusError);
    }

    if (!m_systemBus && !m_sessionBus) {
        std::cerr << "[DBus] Failed to connect to any D-Bus\n";
        return false;
    }

    addDBusMatches();
    if (m_systemBus)  dbus_connection_flush(m_systemBus);
    if (m_sessionBus) dbus_connection_flush(m_sessionBus);

    std::cout << "[DBus] Monitoring with " << m_filters.size() << " filters\n";
    return true;
}

void Agent::addDBusMatches() {
    for (const auto& f : m_filters) {
        if (!f.enabled || f.match.empty()) continue;
        if ((f.bus == "system" || f.bus == "both") && m_systemBus) {
            dbus_bus_add_match(m_systemBus, f.match.c_str(), &m_dbusError);
            if (dbus_error_is_set(&m_dbusError)) dbus_error_free(&m_dbusError);
        }
        if ((f.bus == "session" || f.bus == "both") && m_sessionBus) {
            dbus_bus_add_match(m_sessionBus, f.match.c_str(), &m_dbusError);
            if (dbus_error_is_set(&m_dbusError)) dbus_error_free(&m_dbusError);
        }
    }
}

void Agent::removeDBusMatches(const std::vector<DBusFilter>& filters) {
    for (const auto& f : filters) {
        if (!f.enabled || f.match.empty()) continue;
        if ((f.bus == "system" || f.bus == "both") && m_systemBus) {
            dbus_bus_remove_match(m_systemBus, f.match.c_str(), &m_dbusError);
            if (dbus_error_is_set(&m_dbusError)) dbus_error_free(&m_dbusError);
        }
        if ((f.bus == "session" || f.bus == "both") && m_sessionBus) {
            dbus_bus_remove_match(m_sessionBus, f.match.c_str(), &m_dbusError);
            if (dbus_error_is_set(&m_dbusError)) dbus_error_free(&m_dbusError);
        }
    }
}

void Agent::handleDBusMessage(DBusMessage* msg, const std::string& busType) {
    bool shouldLog     = false;
    bool shouldForward = false;
    bool anyMatch      = false;
    {
        std::shared_lock<std::shared_mutex> lock(m_filtersMutex);
        shouldLog     = m_filterSettings.defaultLog;
        shouldForward = m_filterSettings.defaultForward;
        for (const auto& f : m_filters) {
            if (!f.enabled || !f.matches(msg, busType)) continue;
            anyMatch = true;
            if (f.log)     shouldLog     = true;
            if (f.forward) shouldForward = true;
        }
    }

    if (!anyMatch && !shouldLog && !shouldForward)
        return;

    m_matchedCount++;
    DBusMessageInfo info = DBusMessageInfo::fromDBusMessage(msg, busType);

    if (shouldLog) {
        m_loggedCount++;
        m_messageHandler.logDBusMessage(m_clientId, "CAPTURED", info);
    }

    if (shouldForward && m_tcpClient->isConnected()) {
        m_forwardedCount++;
        m_tcpClient->sendMessage("DBUS:" + info.serialize());
    }
}

void Agent::dbusMonitorThread() {
    std::cout << "[DBus] Monitor thread started\n";

    while (m_running) {
        if (m_systemBus) {
            dbus_connection_read_write(m_systemBus, 50);
            DBusMessage* msg;
            while ((msg = dbus_connection_pop_message(m_systemBus))) {
                handleDBusMessage(msg, "system");
                dbus_message_unref(msg);
            }
        }
        if (m_sessionBus) {
            dbus_connection_read_write(m_sessionBus, 50);
            DBusMessage* msg;
            while ((msg = dbus_connection_pop_message(m_sessionBus))) {
                handleDBusMessage(msg, "session");
                dbus_message_unref(msg);
            }
        }
    }

    std::cout << "[DBus] Monitor thread stopped\n";
}

bool Agent::parseAndInjectDBusMessage(const std::string& dbusCommand) {
    std::istringstream iss(dbusCommand);
    std::string type, dest, path, iface, member;
    iss >> type >> dest >> path >> iface >> member;

    std::string rest;
    std::getline(iss, rest);
    if (!rest.empty() && rest.front() == ' ')
        rest = rest.substr(1);

    std::string argsPart;
    std::string busType = "session";
    size_t lastSpace = rest.rfind(' ');
    if (lastSpace != std::string::npos) {
        std::string tail = rest.substr(lastSpace + 1);
        if (tail == "system" || tail == "session") {
            busType  = tail;
            argsPart = rest.substr(0, lastSpace);
        } else {
            argsPart = rest;
        }
    } else {
        argsPart = rest;
    }

    if (type.empty() || path.empty() || iface.empty() || member.empty()) {
        std::cerr << "[DBus] Injection: missing required fields\n";
        return false;
    }

    DBusConnection* conn = (busType == "system") ? m_systemBus : m_sessionBus;
    if (!conn) {
        std::cerr << "[DBus] Cannot inject: " << busType << " bus not available\n";
        return false;
    }

    DBusMessage* msg = nullptr;
    if (type == "method_call") {
        msg = dbus_message_new_method_call(
            dest.empty() ? nullptr : dest.c_str(),
            path.c_str(), iface.c_str(), member.c_str());
    } else if (type == "signal") {
        msg = dbus_message_new_signal(path.c_str(), iface.c_str(), member.c_str());
    } else {
        std::cerr << "[DBus] Unsupported injection type: " << type << "\n";
        return false;
    }

    if (!msg) {
        std::cerr << "[DBus] Failed to create message\n";
        return false;
    }

    if (!argsPart.empty()) {
        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);

        std::stringstream argsStream(argsPart);
        std::string arg;
        while (std::getline(argsStream, arg, ',')) {
            if (arg.empty()) continue;
            size_t colon = arg.find(':');
            if (colon == std::string::npos) continue;
            std::string atype = arg.substr(0, colon);
            std::string aval  = arg.substr(colon + 1);

            try {
                if      (atype == "string")  { const char* s = aval.c_str(); dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,  &s); }
                else if (atype == "int32")   { dbus_int32_t  v = std::stoi(aval);  dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32,   &v); }
                else if (atype == "uint32")  { dbus_uint32_t v = std::stoul(aval); dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32,  &v); }
                else if (atype == "boolean") { dbus_bool_t   v = (aval == "true" || aval == "1") ? TRUE : FALSE; dbus_message_iter_append_basic(&iter, DBUS_TYPE_BOOLEAN, &v); }
                else if (atype == "double")  { double        v = std::stod(aval);  dbus_message_iter_append_basic(&iter, DBUS_TYPE_DOUBLE,  &v); }
                else if (atype == "byte")    { unsigned char v = (unsigned char)std::stoi(aval); dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE, &v); }
            } catch (const std::exception& e) {
                std::cerr << "[DBus] Bad arg value '" << aval << "': " << e.what() << "\n";
                dbus_message_unref(msg);
                return false;
            }
        }
    }

    dbus_connection_send(conn, msg, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);

    std::cout << "[DBus] Injected " << type << " on " << busType << " bus\n";

    DBusMessageInfo info;
    info.type        = type;
    info.destination = dest;
    info.path        = path;
    info.interface   = iface;
    info.member      = member;
    info.busType     = busType;
    info.sender      = "plugin";
    m_messageHandler.logDBusMessage(m_clientId, "INJECTED", info);

    return true;
}

void Agent::printFilterStats() const {
    std::cout << "\n[DBus Filter Stats]\n"
              << "  Matched:   " << m_matchedCount.load()   << "\n"
              << "  Logged:    " << m_loggedCount.load()    << "\n"
              << "  Forwarded: " << m_forwardedCount.load() << "\n";
}

void Agent::onTCPMessageReceived(const std::string& message) {
    if (m_receivingMsgConfig) {
        m_msgConfigBuffer += message + "\n";

        json probe = json::parse(m_msgConfigBuffer, nullptr, /*exceptions=*/false);
        if (!probe.is_discarded()) {
            m_receivingMsgConfig = false;
            applyMsgConfig(m_msgConfigBuffer);
            m_msgConfigBuffer.clear();
        }
        return;
    }

    if (message.rfind("MSG_CONFIG:", 0) == 0) {
        m_msgConfigBuffer    = message.substr(11) + "\n";
        m_receivingMsgConfig = true;

        json probe = json::parse(m_msgConfigBuffer, nullptr, false);
        if (!probe.is_discarded()) {
            m_receivingMsgConfig = false;
            applyMsgConfig(m_msgConfigBuffer);
            m_msgConfigBuffer.clear();
        }
        return;
    }

    if (message.rfind("DBUS:", 0) == 0) {
        parseAndInjectDBusMessage(message.substr(5));
    } else if (message == "STATS") {
        printFilterStats();
    } else {
        m_messageHandler.receiveMessage(message);
    }
}

bool Agent::saveMsgConfig(const std::string& jsonContent) {
    std::string target;
    for (const auto& p : std::vector<std::string>{
             m_msgConfigPath, "./msg_config.json", "../msg_config.json" }) {
        std::ifstream probe(p);
        if (probe.is_open()) { target = p; break; }
    }
    if (target.empty())
        target = "./msg_config.json";

    std::ofstream out(target, std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[Agent] Failed to open " << target << " for writing\n";
        return false;
    }
    out << jsonContent;
    std::cout << "[Agent] MSG_CONFIG saved to " << target << "\n";
    return true;
}

bool Agent::applyMsgConfig(const std::string& jsonContent) {
    json root = json::parse(jsonContent, nullptr, false);
    if (root.is_discarded()) {
        std::cerr << "[Agent] MSG_CONFIG: received invalid JSON — ignoring\n";
        return false;
    }
    std::vector<DBusFilter> newFilters;
    FilterSettings          newSettings;
    {
        std::shared_lock<std::shared_mutex> rlock(m_filtersMutex);
        newSettings = m_filterSettings;
    }
    parseFiltersFromJson(root, newFilters, newSettings);
    {
        std::unique_lock<std::shared_mutex> wlock(m_filtersMutex);
        removeDBusMatches(m_filters);
        m_filters        = std::move(newFilters);
        m_filterSettings = newSettings;
        addDBusMatches();
    }
    if (m_systemBus) dbus_connection_flush(m_systemBus);
    if (m_sessionBus) dbus_connection_flush(m_sessionBus);
    std::cout << "[Agent] MSG_CONFIG applied: " << m_filters.size() << " filters loaded and active\n";
    saveMsgConfig(jsonContent);
    return true;
}

bool Agent::initialize(const std::string& configPath, const std::string& msgConfigPath, const std::string& clientId) {
    std::cout << "========================================\n"
              << "   Client Agent (D-Bus Monitor)\n"
              << "========================================\n";

    if (!m_lockfileManager.acquireLock()) {
        std::cerr << "[Agent] Another instance is already running\n";
        return false;
    }

    if (!loadServerConfig(configPath)) {
        m_lockfileManager.releaseLock();
        return false;
    }

    if (!loadMsgConfig(msgConfigPath)) {
        m_lockfileManager.releaseLock();
        return false;
    }

    m_msgConfigPath = msgConfigPath;
    m_clientId      = clientId;

    if (m_clientId.empty()) {
        std::cerr << "[Agent] Client ID cannot be empty\n";
        m_lockfileManager.releaseLock();
        return false;
    }

    if (!initDBus()) {
        m_lockfileManager.releaseLock();
        return false;
    }

    m_tcpClient->setMessageCallback([this](const std::string& msg) {
        onTCPMessageReceived(msg);
    });

    m_messageHandler.setMessageCallback([this](const std::string& msg) {
        if (m_tcpClient->isConnected())
            m_tcpClient->sendMessage(msg);
    });

    m_tcpClient->setConnectionCallback([this](bool connected) {
        std::cout << "\n[" << m_messageHandler.getCurrentTime() << "] "
                  << (connected ? "Connected to" : "Disconnected from")
                  << " server\n";
    });

    std::cout << "\n[Agent] Initialized — client ID: " << m_clientId
              << "  server: " << m_serverConfig.ip << ":" << m_serverConfig.port
              << "  filters: " << m_filters.size() << "\n";
    return true;
}

void Agent::start() {
    if (m_running) return;
    m_running = true;

    if (!m_tcpClient->connect(m_serverConfig.ip, m_serverConfig.port, m_clientId)) {
        std::cerr << "[Agent] Failed to connect to server\n";
    }
    m_dbusMonitorThread = std::thread(&Agent::dbusMonitorThread, this);
    bool stdinIsTty    = isatty(STDIN_FILENO);
    bool stdoutIsTty   = isatty(STDOUT_FILENO);
    bool launchedByRat = (std::getenv("RAT_CLIENT_ID") != nullptr);

    if (stdinIsTty && stdoutIsTty && !launchedByRat) {
        startStdinReader();
        std::cout << "\n[Agent] Interactive mode — type messages and press Enter.\n";
    } else {
        std::cout << "\n[Agent] Background mode — stdin reader disabled.\n";
    }
}

void Agent::stop() {
    if (!m_running) return;
    std::cout << "\n[Agent] Stopping...\n";
    m_running = false;

    printFilterStats();
    stopStdinReader();
    m_tcpClient->disconnect();

    if (m_dbusMonitorThread.joinable())
        m_dbusMonitorThread.join();

    m_lockfileManager.releaseLock();
    std::cout << "[Agent] Stopped\n";
}