// client_agent/src/LockfileManager.cpp
#include "LockfileManager.hpp"
#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

LockfileManager::LockfileManager() : m_locked(false) {}

LockfileManager::~LockfileManager() {
    releaseLock();
}

bool LockfileManager::acquireLock() {
    if (m_locked)
        return true;

    const std::string path = "/tmp/client_agent.lock";

    if (createLock(path))
        return true;

    if (isLockStale(path)) {
        std::cout << "Removing stale lockfile " << path << "\n";
        unlink(path.c_str());

        if (createLock(path))
            return true;

    }
    
    std::cerr << "Another instance is running (lockfile exists). Exiting.\n";
    return false;
}

void LockfileManager::releaseLock() {
    if (!m_locked)
        return;

    unlink(m_lockfile_path.c_str());
    m_locked = false;
    m_lockfile_path.clear();
}

bool LockfileManager::createLock(const std::string& path) {
    if (!tryLockPath(path))
        return false;

    m_locked = true;
    m_lockfile_path = path;
    writePid();
    std::cout << "Created lockfile " << m_lockfile_path << "\n";
    return true;
}

bool LockfileManager::tryLockPath(const std::string& path) {
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

bool LockfileManager::isLockStale(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return true;

    pid_t pid = 0;
    f >> pid;
    if (pid <= 0)
        return true;

    if (kill(pid, 0) == 0)
        return false;

    return (errno == ESRCH);
}

void LockfileManager::writePid() {
    std::ofstream f(m_lockfile_path);
    if (f)
        f << getpid() << "\n";
}