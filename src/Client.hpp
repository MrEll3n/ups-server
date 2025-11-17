#pragma once
#include <string>

enum class ClientState {
    WaitingForLogin,
    Idle,
    InGame,
    Disconnected
};

struct Client {
    int fd;
    std::string buffer;
    ClientState state = ClientState::WaitingForLogin;

    explicit Client(int fd) : fd(fd) {}
};
