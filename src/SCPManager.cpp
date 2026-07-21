// remote_access_tool/src/SCPManager.cpp
#include "SCPManager.hpp"
#include "SSHManager.hpp"
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <memory>

SCPManager& SCPManager::getInstance() {
    static SCPManager instance;
    return instance;
}

SCPManager::SCPManager() : m_ssh_key_path("/home/yeet/.ssh/id_ed25519") {}
SCPManager::~SCPManager() {}

void SCPManager::uploadFile(const Client& client, const std::string& localPath, const std::string& remotePath, bool async) {
    if (async) {
        ScpTask* task = new ScpTask{client, localPath, remotePath, true};
        pthread_t thread;
        if (pthread_create(&thread, nullptr, scpExecThread, task) != 0) {
            std::cerr << "Failed to create thread" << std::endl;
            delete task;
            return;
        }
        pthread_detach(thread);
    } else {
        uploadFileSync(client, localPath, remotePath);
    }
}

void SCPManager::downloadFile(const Client& client, const std::string& remotePath, const std::string& localPath) {
    downloadFileSync(client, remotePath, localPath);
}

void SCPManager::uploadToAll(const std::vector<Client>& clients, const std::string& localPath, const std::string& remotePath) {
    for (const auto& c : clients)
        uploadFile(c, localPath, remotePath, true);
}

void SCPManager::uploadToTagged(const std::vector<Client>& clients, const std::string& tag, const std::string& localPath, const std::string& remotePath) {
    for (const auto& c : clients)
        if (c.hasTag(tag))
            uploadFile(c, localPath, remotePath, true);
}

void SCPManager::uploadFileSync(const Client& client, const std::string& localPath, const std::string& remotePath) {
    std::string port = std::to_string(client.getPort());

    std::stringstream cmd;
    cmd << "scp -o StrictHostKeyChecking=no"
        << " -i " << m_ssh_key_path
        << " -P " << port
        << " "    << localPath
        << " "    << client.getSSHTarget() << ":" << remotePath;

    {
        std::lock_guard<std::mutex> lock(m_print_mutex);
        std::cout << "[*] " << client.getId() << " uploading "
                  << localPath << " → " << remotePath << std::endl;
    }

    int ret = system(cmd.str().c_str());
    if (ret == -1 || WEXITSTATUS(ret) != 0)
        std::cerr << "SCP upload failed for " << client.getId() << std::endl;
}

void SCPManager::downloadFileSync(const Client& client, const std::string& remotePath, const std::string& localPath) {
    std::string port = std::to_string(client.getPort());

    std::stringstream cmd;
    cmd << "scp -o StrictHostKeyChecking=no"
        << " -i " << m_ssh_key_path
        << " -P " << port
        << " "    << client.getSSHTarget() << ":" << remotePath
        << " "    << localPath;

    {
        std::lock_guard<std::mutex> lock(m_print_mutex);
        std::cout << "[*] " << client.getId() << " downloading "
                  << remotePath << " → " << localPath << std::endl;
    }

    int ret = system(cmd.str().c_str());
    if (ret == -1 || WEXITSTATUS(ret) != 0)
        std::cerr << "SCP download failed for " << client.getId() << std::endl;
}

void* SCPManager::scpExecThread(void* arg) {
    std::unique_ptr<ScpTask> task(static_cast<ScpTask*>(arg));
    SCPManager& mgr = SCPManager::getInstance();

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return nullptr;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        setpgid(0, 0);

        std::string port        = std::to_string(task->client.getPort());
        std::string remote_full = task->client.getSSHTarget() + ":" + task->remote;

        if (task->upload) {
            execlp("scp", "scp",
                   "-q",
                   "-o", "StrictHostKeyChecking=no",
                   "-i", mgr.m_ssh_key_path.c_str(),
                   "-P", port.c_str(),
                   task->local.c_str(),
                   remote_full.c_str(),
                   nullptr);
        } else {
            execlp("scp", "scp",
                   "-o", "StrictHostKeyChecking=no",
                   "-i", mgr.m_ssh_key_path.c_str(),
                   "-P", port.c_str(),
                   remote_full.c_str(),
                   task->local.c_str(),
                   nullptr);
        }

        perror("exec scp failed");
        _exit(1);
    }

    SSHManager::getInstance().registerSSHPid(pid);
    close(pipefd[1]);

    char buffer[4096];
    std::string output;
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        output.append(buffer, n);
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);

    {
        std::lock_guard<std::mutex> lock(mgr.m_print_mutex);
        std::cout << "===== SCP " << task->client.getId()
                  << " (" << task->client.getIp() << ") =====\n";
        if (!output.empty())
            std::cout << output;
        std::cout << "===== SCP DONE: " << task->client.getId()
                  << " =====\n\n> " << std::flush;
    }

    return nullptr;
}
