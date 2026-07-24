// client_agent/src/main.cpp
#include "Agent.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

volatile sig_atomic_t g_shutdown_flag = 0;
static void onSignal(int) { g_shutdown_flag = 1; }

int main(int argc, char* argv[]) {
    signal(SIGINT,  onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP,  onSignal);
    signal(SIGPIPE, SIG_IGN);

    const char* user = std::getenv("USER");
    if (!user) { std::cerr << "Error: USER env var not set\n"; return 1; }

    const std::string configPath = (argc > 1) ? argv[1] : "./connection_config.json";
    std::cout << "Client: " << user << "  Config: " << configPath << "\n";

    Agent agent;
    if (!agent.initialize(configPath, user)) return 1;
    agent.start();

    while (!g_shutdown_flag) pause();

    agent.stop();
    return 0;
}
