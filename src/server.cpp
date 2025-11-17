#include "Server.hpp"
#include "Protocol.hpp"

#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

Server::Server(int port) {
    init_socket(port);
}

Server::~Server() {
    // Close all client sockets
    for (auto& [fd, _] : client_buffers) {
        close(fd);
    }
    // Close listening socket
    close(server_fd);
}

void Server::init_socket(int port) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        std::exit(1);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        std::exit(1);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        std::exit(1);
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        std::exit(1);
    }

    FD_ZERO(&master_set);
    FD_SET(server_fd, &master_set);
    max_fd = server_fd;

    std::cout << "Server listening on port " << port << "\n";
}

void Server::accept_client() {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);

    int client_fd = accept(server_fd, (sockaddr*)&addr, &len);
    if (client_fd < 0) {
        perror("accept");
        return;
    }

    FD_SET(client_fd, &master_set);
    if (client_fd > max_fd) {
        max_fd = client_fd;
    }

    client_buffers[client_fd] = std::string{};

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    std::cout << "New client: " << ip << ":" << ntohs(addr.sin_port)
              << " fd=" << client_fd << "\n";
}

void Server::remove_client(int fd) {
    std::cout << "Client disconnected fd=" << fd << "\n";
    close(fd);
    FD_CLR(fd, &master_set);
    client_buffers.erase(fd);

    if (fd == max_fd) {
        while (max_fd >= 0 && !FD_ISSET(max_fd, &master_set)) {
            --max_fd;
        }
    }
}

void Server::handle_client_data(int fd) {
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);

    if (n <= 0) {
        // error or client closed connection
        if (n < 0) {
            perror("recv");
        }
        remove_client(fd);
        return;
    }

    std::string& buffer = client_buffers[fd];
    buffer.append(buf, static_cast<std::size_t>(n));

    // Process complete lines terminated by '\n'
    for (;;) {
        std::size_t pos = buffer.find('\n');
        if (pos == std::string::npos) {
            break;
        }

        std::string line = buffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        buffer.erase(0, pos + 1);

        Request req = parse_request_line(line);
        handle_request(fd, req);
    }
}

void Server::run() {
    while (true) {
        fd_set read_set = master_set;

        int rv = select(max_fd + 1, &read_set, nullptr, nullptr, nullptr);
        if (rv < 0) {
            if (errno == EINTR) {
                continue; // interrupted by signal, retry
            }
            perror("select");
            break;
        }

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (!FD_ISSET(fd, &read_set)) {
                continue;
            }

            if (fd == server_fd) {
                accept_client();
            } else {
                handle_client_data(fd);
            }
        }
    }
}
