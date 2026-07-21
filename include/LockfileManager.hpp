#pragma once
// remote_access_tool/src/LockfileManager.hpp

#include <string>

class LockfileManager {
public:
    LockfileManager();
    ~LockfileManager();
    bool acquireLock();
    void releaseLock();
    bool isLocked() const;

private:
    bool tryLockPath(const std::string& path);
    bool isLockStale(const std::string& path);
    void writePid();
    bool m_locked = false;
    std::string m_lockfile_path;
};
