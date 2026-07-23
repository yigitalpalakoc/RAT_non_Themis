// client_agent/src/main.cpp
#include "Agent.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>

volatile sig_atomic_t g_shutdown_flag = 0;
static int g_pipe[2] = { -1, -1 };

static void signalHandler(int signum) {
    g_shutdown_flag = 1;
    if (g_pipe[1] != -1) {
        char x = (char)signum;
        write(g_pipe[1], &x, 1);
    }
}

static std::string getClientIdFromEnv() {
    const char* id = std::getenv("USER");
    if (id) {
        std::cout << "Warning: Using USER as client ID: " << id << std::endl;
        return id;
    }
    return "";
}

int main(int argc, char* argv[]) {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);

    struct sigaction sa_pipe{};
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, nullptr);

    if (pipe(g_pipe) < 0) {
        std::cerr << "Warning: Failed to create shutdown pipe\n";
        g_pipe[0] = g_pipe[1] = -1;
    } else {
        int flags = fcntl(g_pipe[0], F_GETFL, 0);
        fcntl(g_pipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::string clientId = getClientIdFromEnv();
    if (clientId.empty()) {
        std::cerr << "Error: No client ID found.\n";
        return 1;
    }

    std::string configPath    = "../connection_config.json";
    if (argc > 1) configPath    = argv[1];

    std::cout << "========================================\n"
              << "        Client Agent Starting\n"
              << "========================================\n"
              << "Client ID:  " << clientId    << "\n"
              << "Config:     " << configPath  << "\n";

    Agent agent;
    if (!agent.initialize(configPath, clientId)) {
        std::cerr << "Failed to initialize agent\n";
        return 1;
    }

    agent.start();

    if (g_pipe[0] != -1) {
        char buf[4];
        while (!g_shutdown_flag) {
            if (read(g_pipe[0], buf, sizeof(buf)) > 0)
                break;
            if (errno != EINTR && errno != EAGAIN)
                break;
        }
    } else {
        while (!g_shutdown_flag)
            pause();
    }
    std::cout << "\nShutting down...\n";
    agent.stop();
    if (g_pipe[0] != -1) close(g_pipe[0]);
    if (g_pipe[1] != -1) close(g_pipe[1]);

    std::cout << "========================================\n"
              << "        Client Agent Stopped\n"
              << "========================================\n";
    return 0;
}