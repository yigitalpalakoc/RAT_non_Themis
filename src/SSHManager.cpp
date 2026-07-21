// remote_access_tool/src/SSHManager.cpp
#include "SSHManager.hpp"
#include "ShellManager.hpp"
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <memory>
#include <chrono>
#include <thread>

SSHManager& SSHManager::getInstance() {
    static SSHManager instance;
    return instance;
}

SSHManager::SSHManager() : m_ssh_key_path("/home/yeet/.ssh/id_ed25519") {}

SSHManager::~SSHManager() {
    killAllSessions();
}

void SSHManager::executeCommand(const Client& client, const std::string& command, bool async) {
    if (command.empty()) {
        std::cerr << "Refusing to run empty command on " << client.getId() << std::endl;
        return;
    }

    if (async) {
        ExecTask* task = new ExecTask{client, command};
        pthread_t thread;
        if (pthread_create(&thread, nullptr, sshExecThread, task) != 0) {
            std::cerr << "Failed to create thread" << std::endl;
            delete task;
            return;
        }
        pthread_detach(thread);
    } else {
        executeCommandSync(client, command);
    }
}

void SSHManager::executeCommandSync(const Client& client, const std::string& command) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("[SSHManager] pipe");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[SSHManager] fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        setpgid(0, 0);

        std::string port       = std::to_string(client.getPort());
        std::string remote_cmd = "bash -c 'set -f; " + command + "'";

        execlp("ssh", "ssh",
               "-T",
               "-o", "BatchMode=yes",
               "-o", "LogLevel=ERROR",
               "-o", "StrictHostKeyChecking=no",
               "-o", "UserKnownHostsFile=/dev/null",
               "-i", m_ssh_key_path.c_str(),
               "-p", port.c_str(),
               client.getSSHTarget().c_str(),
               remote_cmd.c_str(),
               nullptr);
        _exit(1);
    }

    registerSSHPid(pid);
    close(pipefd[1]);

    {
        std::lock_guard<std::mutex> lock(m_print_mutex);
        std::cout << "===== BEGIN OUTPUT: " << client.getId() << " =====\n";
        char buffer[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
            std::cout.write(buffer, n);
        std::cout << "===== END OUTPUT: " << client.getId() << " =====\n";
    }

    close(pipefd[0]);
    waitpid(pid, nullptr, 0);
}

void* SSHManager::sshExecThread(void* arg) {
    std::unique_ptr<ExecTask> task(static_cast<ExecTask*>(arg));
    SSHManager& mgr = SSHManager::getInstance();

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

        std::string port       = std::to_string(task->client.getPort());
        std::string remote_cmd = "bash -c 'set -f; " + task->command + "'";

        execlp("ssh", "ssh",
               "-T",
               "-o", "BatchMode=yes",
               "-o", "LogLevel=ERROR",
               "-o", "StrictHostKeyChecking=no",
               "-o", "UserKnownHostsFile=/dev/null",
               "-i", mgr.getSSHKeyPath().c_str(),
               "-p", port.c_str(),
               task->client.getSSHTarget().c_str(),
               remote_cmd.c_str(),
               nullptr);
        _exit(1);
    }

    mgr.registerSSHPid(pid);
    close(pipefd[1]);

    char        buffer[4096];
    std::string output;
    ssize_t     n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        output.append(buffer, n);
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);

    {
        std::lock_guard<std::mutex> lock(mgr.m_print_mutex);
        std::cout << "===== BEGIN OUTPUT: " << task->client.getId() << " =====\n"
                  << output
                  << "===== END OUTPUT: "   << task->client.getId() << " =====\n"
                  << "\n> " << std::flush;
    }

    return nullptr;
}

void SSHManager::registerSSHPid(pid_t pid) {
    std::lock_guard<std::mutex> lock(m_pid_mutex);

    m_ssh_pids.erase(
        std::remove_if(m_ssh_pids.begin(), m_ssh_pids.end(),
            [](pid_t p) {
                return waitpid(p, nullptr, WNOHANG) != 0;
            }),
        m_ssh_pids.end());

    m_ssh_pids.push_back(pid);
}

void SSHManager::killAllSessions() {
    std::vector<pid_t> pids;
    {
        std::lock_guard<std::mutex> lock(m_pid_mutex);
        pids.swap(m_ssh_pids);
    }

    for (pid_t pid : pids) {
        if (pid > 0)
            kill(-pid, SIGTERM);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (pid_t pid : pids) {
        if (pid > 0) {
            if (waitpid(pid, nullptr, WNOHANG) == 0)
                kill(-pid, SIGKILL);
            waitpid(pid, nullptr, WNOHANG);
        }
    }
}
