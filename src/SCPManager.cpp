// remote_access_tool/src/SCPManager.cpp
#include "SCPManager.hpp"
#include "SSHManager.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <memory>

SCPManager& SCPManager::getInstance() {
    static SCPManager instance;
    return instance;
}

SCPManager::SCPManager()  : m_ssh_key_path("/home/yeet/.ssh/id_ed25519") {}
SCPManager::~SCPManager() {}

// ── shared helper ──────────────────────────────────────────────────────────

static std::string runSCP(const Client& client, const std::string& keyPath,
                           const std::string& localPath, const std::string& remotePath,
                           bool upload) {
    int pfd[2];
    if (pipe(pfd) < 0) { perror("[SCP] pipe"); return {}; }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[SCP] fork");
        close(pfd[0]); close(pfd[1]);
        return {};
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        setpgid(0, 0);

        std::string port   = std::to_string(client.getPort());
        std::string remote = client.getSSHTarget() + ":" + remotePath;

        if (upload) {
            execlp("scp", "scp", "-q",
                   "-o", "StrictHostKeyChecking=no",
                   "-i", keyPath.c_str(),
                   "-P", port.c_str(),
                   localPath.c_str(), remote.c_str(),
                   nullptr);
        } else {
            execlp("scp", "scp",
                   "-o", "StrictHostKeyChecking=no",
                   "-i", keyPath.c_str(),
                   "-P", port.c_str(),
                   remote.c_str(), localPath.c_str(),
                   nullptr);
        }
        _exit(1);
    }

    SSHManager::getInstance().registerSSHPid(pid);
    close(pfd[1]);

    char buf[4096]; std::string out; ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) out.append(buf, n);
    close(pfd[0]);
    waitpid(pid, nullptr, 0);
    return out;
}

// ── public API ─────────────────────────────────────────────────────────────

void SCPManager::uploadFile(const Client& client, const std::string& localPath,
                             const std::string& remotePath, bool async) {
    if (async) {
        auto* task = new ScpTask{client, localPath, remotePath, true};
        pthread_t thread;
        if (pthread_create(&thread, nullptr, scpExecThread, task) != 0) {
            std::cerr << "[SCP] Failed to create thread\n";
            delete task;
            return;
        }
        pthread_detach(thread);
    } else {
        std::lock_guard<std::mutex> lock(m_print_mutex);
        std::cout << "[*] " << client.getId() << " uploading "
                  << localPath << " → " << remotePath << "\n";
        runSCP(client, m_ssh_key_path, localPath, remotePath, true);
    }
}

void SCPManager::downloadFile(const Client& client, const std::string& remotePath,
                               const std::string& localPath) {
    std::lock_guard<std::mutex> lock(m_print_mutex);
    std::cout << "[*] " << client.getId() << " downloading "
              << remotePath << " → " << localPath << "\n";
    runSCP(client, m_ssh_key_path, localPath, remotePath, false);
}

void SCPManager::uploadToAll(const std::vector<Client>& clients,
                              const std::string& localPath, const std::string& remotePath) {
    for (const auto& c : clients)
        uploadFile(c, localPath, remotePath, true);
}

void SCPManager::uploadToTagged(const std::vector<Client>& clients, const std::string& tag,
                                 const std::string& localPath, const std::string& remotePath) {
    for (const auto& c : clients)
        if (c.hasTag(tag))
            uploadFile(c, localPath, remotePath, true);
}

void* SCPManager::scpExecThread(void* arg) {
    std::unique_ptr<ScpTask> task(static_cast<ScpTask*>(arg));
    SCPManager& mgr = SCPManager::getInstance();

    std::string out = runSCP(task->client, mgr.m_ssh_key_path,
                              task->local, task->remote, task->upload);

    std::lock_guard<std::mutex> lock(mgr.m_print_mutex);
    std::cout << "===== SCP "      << task->client.getId()
              << " ("              << task->client.getIp() << ") =====\n";
    if (!out.empty()) std::cout << out;
    std::cout << "===== SCP DONE: " << task->client.getId()
              << " =====\n\n> "     << std::flush;
    return nullptr;
}