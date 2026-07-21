// remote_access_tool/src/LockfileManager.cpp
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

    const char* paths[] = {
        "/var/run/rat.lock",
        "/tmp/rat.lock",
        nullptr
    };

    for (int i = 0; paths[i]; i++) {
        const std::string path = paths[i];

        if (tryLockPath(path)) {
            m_locked        = true;
            m_lockfile_path = path;
            writePid();
            std::cout << "Created lockfile " << m_lockfile_path << std::endl;
            return true;
        }

        if (!isLockStale(path)) {
            continue;
        }

        std::cout << "Removing stale lockfile " << path << std::endl;
        unlink(path.c_str());

        if (tryLockPath(path)) {
            m_locked        = true;
            m_lockfile_path = path;
            writePid();
            std::cout << "Created lockfile " << m_lockfile_path << std::endl;
            return true;
        }
    }

    std::cerr << "Another instance is running (lockfile exists or cannot create). Exiting.\n";
    return false;
}

void LockfileManager::releaseLock() {
    if (!m_locked || m_lockfile_path.empty())
        return;

    unlink(m_lockfile_path.c_str());
    m_locked = false;
    m_lockfile_path.clear();
}

bool LockfileManager::isLocked() const {
    return m_locked;
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
