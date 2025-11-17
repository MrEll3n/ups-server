#pragma once

#include <unordered_map>
#include <string>
#include <sys/select.h>

class Server {
public:
    explicit Server(int port);
    ~Server();

    void run();

private:
    int server_fd;
    int max_fd;
    fd_set master_set;

    // Per-client input buffers (fd -> string)
    std::unordered_map<int, std::string> client_buffers;

    void init_socket(int port);
    void accept_client();
    void handle_client_data(int fd);
    void remove_client(int fd);
};
