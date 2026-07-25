// remote_access_tool/src/main.cpp
#include "RemoteController.hpp"
#include "SSHManager.hpp"
#include "ShellManager.hpp"
#include "SCPManager.hpp"
#include "LockfileManager.hpp"
#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static volatile sig_atomic_t g_shutdown = 0;
static int g_pipe[2] = {-1, -1};

static void onSignal(int) {
    if (g_shutdown) return;
    g_shutdown = 1;
    if (g_pipe[1] != -1) { char x = 0; write(g_pipe[1], &x, 1); }
}

static void onSigchld(int) {
    while (waitpid(-1, nullptr, WNOHANG) > 0);
}

int main(int argc, char* argv[]) {
    signal(SIGINT,  onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP,  onSignal);
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa_chld{};
    sa_chld.sa_handler = onSigchld;
    sa_chld.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, nullptr);

    if (pipe(g_pipe) == 0) {
        int fl = fcntl(g_pipe[0], F_GETFL, 0);
        fcntl(g_pipe[0], F_SETFL, fl | O_NONBLOCK);
    }

    LockfileManager lock;
    if (!lock.acquire()) return 1;

    const std::string sshKey = "/home/yeet/.ssh/id_ed25519";
    SSHManager::getInstance().setSSHKeyPath(sshKey);
    ShellManager::getInstance().setSSHKeyPath(sshKey);
    SCPManager::getInstance().setSSHKeyPath(sshKey);

    const std::string configPath = (argc > 1) ? argv[1] : "../clients.json";

    RemoteController ctrl;
    if (!ctrl.loadClients(configPath)) return 1;

    ctrl.startTCPServer();
    ctrl.pushConfigToClients();
    ctrl.pushAgentBinaryToClients();
    ctrl.printHelp();

    // Restore sane line-mode terminal settings after any forkpty usage.
    {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_lflag |= ICANON | ECHO | ISIG;
            t.c_iflag |= ICRNL;
            t.c_cc[VINTR] = 3;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            tcflush(STDIN_FILENO, TCIFLUSH);
        }
    }

    std::string buf;
    char ch;
    while (!g_shutdown) {
        std::cout << "\n> " << std::flush;
        buf.clear();

        while (!g_shutdown) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (g_pipe[0] != -1) FD_SET(g_pipe[0], &fds);
            int nfds = std::max(STDIN_FILENO, g_pipe[0]) + 1;

            int ret = select(nfds, &fds, nullptr, nullptr, nullptr);
            if (g_shutdown) break;
            if (ret < 0) { if (errno == EINTR) continue; break; }
            if (g_pipe[0] != -1 && FD_ISSET(g_pipe[0], &fds)) { g_shutdown = 1; break; }
            if (!FD_ISSET(STDIN_FILENO, &fds)) continue;

            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) { g_shutdown = 1; break; }
            if (ch == '\n' || ch == '\r') break;
            buf += ch;
        }

        if (g_shutdown) break;
        auto line = ctrl.trim(buf);
        if (line.empty()) continue;
        if (line == "quit" || line == "exit") break;
        if (line == "help" || line == "?") { ctrl.printHelp(); continue; }
        ctrl.parseAndExecute(line);
    }

    std::cout << "\nShutting down...\n";
    if (ShellManager::isTerminalSaved()) {
        struct termios orig = ShellManager::getOriginalTerminalSettings();
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }

    ctrl.stopTCPServer();
    SSHManager::getInstance().killAllSessions();

    if (g_pipe[0] != -1) close(g_pipe[0]);
    if (g_pipe[1] != -1) close(g_pipe[1]);
    return 0;
}