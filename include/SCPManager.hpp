#pragma once
// remote_access_tool/src/SCPManager.hpp

#include "Client.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <pthread.h>

class SCPManager {
public:
    static SCPManager& getInstance();
    void setSSHKeyPath(const std::string& path) { m_ssh_key_path = path; }
    const std::string& getSSHKeyPath() const { return m_ssh_key_path; }
    void uploadFile(const Client& client, const std::string& localPath, const std::string& remotePath, bool async = true);
    void downloadFile(const Client& client, const std::string& remotePath, const std::string& localPath);
    void uploadToAll(const std::vector<Client>& clients, const std::string& localPath, const std::string& remotePath);
    void uploadToTagged(const std::vector<Client>& clients, const std::string& tag, const std::string& localPath, const std::string& remotePath);

private:
    SCPManager();
    ~SCPManager();

    SCPManager(const SCPManager&) = delete;
    SCPManager& operator=(const SCPManager&) = delete;

    void uploadFileSync(const Client& client, const std::string& localPath, const std::string& remotePath);
    void downloadFileSync(const Client& client, const std::string& remotePath, const std::string& localPath);

    static void* scpExecThread(void* arg);

    struct ScpTask {
        Client      client;
        std::string local;
        std::string remote;
        bool        upload;
    };

    std::string        m_ssh_key_path;
    mutable std::mutex m_print_mutex;
};
