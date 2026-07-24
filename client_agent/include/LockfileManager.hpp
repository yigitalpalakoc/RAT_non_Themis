#pragma once
// client_agent/src/LockfileManager.hpp

#include <string>

class LockfileManager {
public:
    LockfileManager();
    ~LockfileManager();
    bool acquireLock();
    void releaseLock();

private:
    bool tryLockPath(const std::string& path);
    bool isLockStale(const std::string& path);
    void writePid();
    bool createLock(const std::string& path);
    bool m_locked = false;
    std::string m_lockfile_path;
};