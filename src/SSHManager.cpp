// remote_access_tool/src/SSHManager.cpp
#include "SSHManager.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#include <memory>
#include <chrono>
#include <thread>

SSHManager& SSHManager::getInstance() {
    static SSHManager instance;
    return instance;
}

SSHManager::SSHManager()  : m_ssh_key_path("/home/yeet/.ssh/id_ed25519") {}
SSHManager::~SSHManager() { killAllSessions(); }

// ── shared helper ──────────────────────────────────────────────────────────

static std::string runSSH(const Client& client, const std::string& keyPath,
                           const std::string& command, SSHManager& mgr) {
    int pfd[2];
    if (pipe(pfd) < 0) { perror("[SSH] pipe"); return {}; }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[SSH] fork");
        close(pfd[0]); close(pfd[1]);
        return {};
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        setpgid(0, 0);

        std::string port = std::to_string(client.getPort());
        std::string rcmd = "bash -c 'set -f; " + command + "'";
        execlp("ssh", "ssh",
               "-T",
               "-o", "BatchMode=yes",
               "-o", "LogLevel=ERROR",
               "-o", "StrictHostKeyChecking=no",
               "-o", "UserKnownHostsFile=/dev/null",
               "-i", keyPath.c_str(),
               "-p", port.c_str(),
               client.getSSHTarget().c_str(),
               rcmd.c_str(),
               nullptr);
        _exit(1);
    }

    mgr.registerSSHPid(pid);
    close(pfd[1]);

    char buf[4096]; std::string out; ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) out.append(buf, n);
    close(pfd[0]);
    waitpid(pid, nullptr, 0);
    return out;
}

// ── public API ─────────────────────────────────────────────────────────────

void SSHManager::executeCommand(const Client& client, const std::string& command, bool async) {
    if (command.empty()) {
        std::cerr << "[SSH] Refusing empty command on " << client.getId() << "\n";
        return;
    }
    if (async) {
        auto* task = new ExecTask{client, command};
        pthread_t thread;
        if (pthread_create(&thread, nullptr, sshExecThread, task) != 0) {
            std::cerr << "[SSH] Failed to create thread\n";
            delete task;
            return;
        }
        pthread_detach(thread);
    } else {
        executeCommandSync(client, command);
    }
}

void SSHManager::executeCommandSync(const Client& client, const std::string& command) {
    std::string out = runSSH(client, m_ssh_key_path, command, *this);
    std::lock_guard<std::mutex> lock(m_print_mutex);
    std::cout << "===== BEGIN OUTPUT: " << client.getId() << " =====\n"
              << out
              << "===== END OUTPUT: "   << client.getId() << " =====\n";
}

void* SSHManager::sshExecThread(void* arg) {
    std::unique_ptr<ExecTask> task(static_cast<ExecTask*>(arg));
    SSHManager& mgr = SSHManager::getInstance();

    std::string out = runSSH(task->client, mgr.m_ssh_key_path, task->command, mgr);

    std::lock_guard<std::mutex> lock(mgr.m_print_mutex);
    std::cout << "===== BEGIN OUTPUT: " << task->client.getId() << " =====\n"
              << out
              << "===== END OUTPUT: "   << task->client.getId() << " =====\n"
              << "\n> " << std::flush;
    return nullptr;
}

// ── PID tracking ───────────────────────────────────────────────────────────

void SSHManager::registerSSHPid(pid_t pid) {
    std::lock_guard<std::mutex> lock(m_pid_mutex);
    // Reap any already-finished children before adding the new one.
    m_ssh_pids.erase(
        std::remove_if(m_ssh_pids.begin(), m_ssh_pids.end(),
            [](pid_t p) { return waitpid(p, nullptr, WNOHANG) != 0; }),
        m_ssh_pids.end());
    m_ssh_pids.push_back(pid);
}

void SSHManager::killAllSessions() {
    std::vector<pid_t> pids;
    {
        std::lock_guard<std::mutex> lock(m_pid_mutex);
        pids.swap(m_ssh_pids);
    }
    for (pid_t pid : pids)
        if (pid > 0) kill(-pid, SIGTERM);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (pid_t pid : pids) {
        if (pid > 0 && waitpid(pid, nullptr, WNOHANG) == 0)
            kill(-pid, SIGKILL);
    }
}