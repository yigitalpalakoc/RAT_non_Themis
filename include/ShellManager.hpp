#pragma once
// remote_access_tool/src/ShellManager.hpp

#include "Client.hpp"
#include <string>
#include <termios.h>
#include <pthread.h>

class ShellManager {
public:
    static ShellManager& getInstance();
    void setSSHKeyPath(const std::string& path) { m_ssh_key_path = path; }
    const std::string& getSSHKeyPath() const { return m_ssh_key_path; }
    void executeInteractiveShell(const Client& client);
    static void saveTerminalSettings(const struct termios& t);
    static struct termios getOriginalTerminalSettings();
    static bool isTerminalSaved();

private:
    ShellManager();
    ~ShellManager();

    ShellManager(const ShellManager&) = delete;
    ShellManager& operator=(const ShellManager&) = delete;

    static void* shellThread(void* arg);
    std::string m_ssh_key_path;
    static struct termios s_orig_termios;
    static bool           s_termiosSaved;
};
