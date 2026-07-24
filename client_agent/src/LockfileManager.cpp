// client_agent/src/LockfileManager.cpp
#include "LockfileManager.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <signal.h>

bool LockfileManager::acquire() {
    if (m_locked) return true;

    // If a lockfile already exists, check whether its PID is still alive.
    {
        std::ifstream f(kPath);
        pid_t pid = 0;
        if (f >> pid && pid > 0 && kill(pid, 0) == 0) {
            std::cerr << "Another instance is running (PID " << pid << ")\n";
            return false;
        }
    }

    unlink(kPath);  // remove stale lockfile, if any

    // O_EXCL gives us atomic creation — fails if another process just created it.
    int fd = open(kPath, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        std::cerr << "Could not create lockfile " << kPath
                  << ": " << strerror(errno) << "\n";
        return false;
    }
    close(fd);
    std::ofstream(kPath) << getpid() << "\n";

    m_locked = true;
    std::cout << "Lockfile: " << kPath << "\n";
    return true;
}

void LockfileManager::release() {
    if (!m_locked) return;
    unlink(kPath);
    m_locked = false;
}
