cmake_minimum_required(VERSION 3.16)
project(client_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find or fetch nlohmann/json
find_package(nlohmann_json REQUIRED)

# Find dbus-1
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

set(SOURCES
    src/Agent.cpp
    src/MessageHandler.cpp
    src/TCPClient.cpp
    src/LockfileManager.cpp
    src/main.cpp
)

add_executable(client_agent ${SOURCES})

target_include_directories(client_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
    ${DBUS_INCLUDE_DIRS}
)

target_link_libraries(client_agent PRIVATE
    ${DBUS_LIBRARIES}
    pthread
)
