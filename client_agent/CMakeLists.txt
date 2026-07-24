cmake_minimum_required(VERSION 3.16)
project(client_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json QUIET)

set(SOURCES
    src/Agent.cpp
    src/TCPClient.cpp
    src/LockfileManager.cpp
    src/main.cpp
)

add_executable(client_agent ${SOURCES})

target_include_directories(client_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(client_agent PRIVATE
    nlohmann_json::nlohmann_json
    pthread
)
