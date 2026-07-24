// client_agent/src/LockfileManager.hpp
#pragma once

class LockfileManager {
public:
    LockfileManager()  = default;
    ~LockfileManager() { release(); }

    bool acquire();
    void release();

private:
    static constexpr const char* kPath = "/tmp/client_agent.lock";
    bool m_locked = false;
};
