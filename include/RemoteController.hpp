#pragma once
// remote_access_tool/src/RemoteController.hpp

#include "Client.hpp"
#include "TCPHandler.hpp"
#include <string>
#include <vector>
#include <memory>

class RemoteController {
public:
    RemoteController();
    ~RemoteController();

    bool loadClients(const std::string& configPath);
    void pushConfigToClients(const std::string& serverIp = "");
    void pushAgentBinaryToClients();
    void startTCPServer();
    void stopTCPServer();
    void parseAndExecute(const std::string& line);
    void printHelp() const;
    std::string trim(const std::string& str) const;

private:
    void handleRunCommandOriginal(const std::string& line);
    void handleScpCommandOriginal(const std::string& line);
    void handleShellCommandOriginal(const std::string& line);
    void handlePluginCommand(const std::string& line);
    void cmdStatus();
    void cmdMsg(const std::string& clientId, const std::string& text);
    void cmdBroadcast(const std::string& text);
    void cmdTag(const std::string& tag, const std::string& text);
    void executeCommand(const std::string& clientId, const std::string& command);
    void executeCommandByTag(const std::string& tag, const std::string& command);
    void executeCommandOnAll(const std::string& command);
    void uploadFile(const std::string& clientId, const std::string& localPath, const std::string& remotePath);
    void downloadFile(const std::string& clientId, const std::string& remotePath, const std::string& localPath);
    void openInteractiveShell(const std::string& clientId);

    Client* getClientById(const std::string& id);
    std::vector<Client> getClientsByTag(const std::string& tag) const;
    std::vector<std::string> tokenize(const std::string& str, char delimiter = ' ') const;
    bool isBlank(const std::string& str) const;

    std::vector<Client> m_clients;
    std::unique_ptr<TCPHandler> m_tcpHandler;
    std::string m_serverIp;
};
