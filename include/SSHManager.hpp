#pragma once
// remote_access_tool/src/SSHManager.hpp

#include "Client.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <pthread.h>
#include <sys/types.h>

class SSHManager {
public:
    static SSHManager& getInstance();
    void setSSHKeyPath(const std::string& path) { m_ssh_key_path = path; }
    const std::string& getSSHKeyPath() const { return m_ssh_key_path; }
    void executeCommand(const Client& client, const std::string& command, bool async = true);
    void executeCommandSync(const Client& client, const std::string& command);
    void registerSSHPid(pid_t pid);
    void killAllSessions();

private:
    SSHManager();
    ~SSHManager();

    SSHManager(const SSHManager&) = delete;
    SSHManager& operator=(const SSHManager&) = delete;

    static void* sshExecThread(void* arg);

    struct ExecTask {
        Client      client;
        std::string command;
    };

    std::string        m_ssh_key_path;
    mutable std::mutex m_print_mutex;
    mutable std::mutex m_pid_mutex;
    std::vector<pid_t> m_ssh_pids;
};
