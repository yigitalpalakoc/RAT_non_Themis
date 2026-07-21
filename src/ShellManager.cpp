// remote_access_tool/src/ShellManager.cpp
#include "ShellManager.hpp"
#include "SSHManager.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <cstring>
#include <sys/select.h>
#include <memory>
#include <algorithm>

struct termios ShellManager::s_orig_termios;
bool ShellManager::s_termiosSaved = false;

void ShellManager::saveTerminalSettings(const struct termios& t) {
    s_orig_termios = t;
    s_termiosSaved = true;
}

struct termios ShellManager::getOriginalTerminalSettings() {
    return s_orig_termios;
}

bool ShellManager::isTerminalSaved() {
    return s_termiosSaved;
}

ShellManager& ShellManager::getInstance() {
    static ShellManager instance;
    return instance;
}

ShellManager::ShellManager() : m_ssh_key_path("/home/yeet/.ssh/id_ed25519") {}
ShellManager::~ShellManager() {}

void ShellManager::executeInteractiveShell(const Client& client) {
    Client*   client_copy = new Client(client);
    pthread_t thread;

    if (pthread_create(&thread, nullptr, shellThread, client_copy) != 0) {
        std::cerr << "Failed to create shell thread" << std::endl;
        delete client_copy;
        return;
    }

    pthread_join(thread, nullptr);
}

void* ShellManager::shellThread(void* arg) {
    std::unique_ptr<Client> client(static_cast<Client*>(arg));
    SSHManager& sshMgr = SSHManager::getInstance();

    struct termios orig_term, raw_term;
    struct winsize winp;
    int master_fd;

    if (tcgetattr(STDIN_FILENO, &orig_term) == -1) {
        perror("tcgetattr");
        return nullptr;
    }
    ShellManager::saveTerminalSettings(orig_term);
    raw_term = orig_term;
    cfmakeraw(&raw_term);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_term) == -1) {
        perror("tcsetattr raw");
        return nullptr;
    }

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winp) == -1) {
        perror("TIOCGWINSZ");
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
        return nullptr;
    }

    pid_t pid = forkpty(&master_fd, nullptr, &orig_term, &winp);
    if (pid < 0) {
        perror("forkpty");
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
        return nullptr;
    }

    if (pid == 0) {
        std::string port = std::to_string(client->getPort());
        execlp("ssh", "ssh",
               "-tt",
               "-o", "StrictHostKeyChecking=no",
               "-i", "/home/yeet/.ssh/id_ed25519",
               "-p", port.c_str(),
               client->getSSHTarget().c_str(),
               nullptr);
        perror("exec ssh failed");
        _exit(1);
    }

    sshMgr.registerSSHPid(pid);

    auto forward_winsize = [&]() {
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
            ioctl(master_fd, TIOCSWINSZ, &ws);
    };

    signal(SIGWINCH, [](int){});
    forward_winsize();

    char   buffer[4096];
    fd_set fds;

    while (true) {
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(master_fd,    &fds);

        int ret = select(std::max(STDIN_FILENO, master_fd) + 1, &fds, nullptr, nullptr, nullptr);
        if (ret < 0) {
            if (errno == EINTR) { forward_winsize(); continue; }
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) break;
            write(master_fd, buffer, n);
        }

        if (FD_ISSET(master_fd, &fds)) {
            ssize_t n = read(master_fd, buffer, sizeof(buffer));
            if (n <= 0) break;
            write(STDOUT_FILENO, buffer, n);
        }
    }

    close(master_fd);
    waitpid(pid, nullptr, 0);
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);

    return nullptr;
}
