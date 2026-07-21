cmake_minimum_required(VERSION 3.16)
project(remote_access_tool LANGUAGES CXX)

# Require C++14 (std::make_unique, chrono, threading)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann/json (header‑only)
find_package(nlohmann_json REQUIRED)

# All source files for the unified tool
set(SOURCES
    src/main.cpp
    src/Client.cpp
    src/SSHManager.cpp
    src/ShellManager.cpp
    src/SCPManager.cpp
    src/LockfileManager.cpp
    src/RemoteController.cpp
    src/MessageHandler.cpp
    src/RulesManager.cpp
    src/TCPHandler.cpp
)

add_executable(remote_access_tool ${SOURCES})

target_include_directories(remote_access_tool
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${nlohmann_json_INCLUDE_DIR}
)

target_link_libraries(remote_access_tool
    PRIVATE
        pthread
)

# Copy the final binary to a bin/ folder (optional)
add_custom_command(TARGET remote_access_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:remote_access_tool>"
            "${CMAKE_SOURCE_DIR}/bin/remote_access_tool"
    COMMENT "Copying remote access tool to ./bin/"
)