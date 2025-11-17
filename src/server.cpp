#include <iostream>
#include <string>
#include <unordered_map>
#include <cstring>
#include <cstdlib>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_PORT 10000

int main() {
    int server_socket;
    int client_socket;
    int fd;
    int return_value;
    struct sockaddr_in my_addr{}, peer_addr{};
    socklen_t len_addr;

    fd_set client_socks, tests;
    int max_fd;

    // Create listening socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    // Optional: reuse address (avoids TIME_WAIT issues)
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_socket);
        return EXIT_FAILURE;
    }

    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(SERVER_PORT);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind to port
    return_value = bind(server_socket, reinterpret_cast<sockaddr*>(&my_addr),
                        sizeof(my_addr));
    if (return_value == 0) {
        std::cout << "Bind - OK\n";
    } else {
        perror("bind");
        close(server_socket);
        return EXIT_FAILURE;
    }

    // Start listening
    return_value = listen(server_socket, 5);
    if (return_value == 0) {
        std::cout << "Listen - OK\n";
    } else {
        perror("listen");
        close(server_socket);
        return EXIT_FAILURE;
    }

    // Master set of file descriptors, add listening socket
    FD_ZERO(&client_socks);
    FD_SET(server_socket, &client_socks);
    max_fd = server_socket;

    std::cout << "Server running on port " << SERVER_PORT << "\n";

    // Per-client string buffers (fd -> std::string)
    std::unordered_map<int, std::string> client_buffers;

    for (;;) {
        tests = client_socks;

        // Wait for activity on any socket 0..max_fd
        return_value = select(max_fd + 1, &tests, nullptr, nullptr, nullptr);
        if (return_value < 0) {
            perror("select");
            break;
        }

        // Check all file descriptors
        for (fd = 0; fd <= max_fd; ++fd) {
            if (!FD_ISSET(fd, &tests))
                continue;

            // New incoming connection on listening socket
            if (fd == server_socket) {
                len_addr = sizeof(peer_addr);
                client_socket = accept(server_socket,
                                       reinterpret_cast<sockaddr*>(&peer_addr),
                                       &len_addr);
                if (client_socket < 0) {
                    perror("accept");
                    continue;
                }

                FD_SET(client_socket, &client_socks);
                if (client_socket > max_fd)
                    max_fd = client_socket;

                // Create empty buffer for this client
                client_buffers[client_socket] = std::string{};

                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer_addr.sin_addr, ip, sizeof(ip));
                std::cout << "New client connected " << ip
                          << ":" << ntohs(peer_addr.sin_port)
                          << " (fd=" << client_socket << ")\n";
            }
            // Existing client socket – read data
            else {
                char buf[1024];
                ssize_t n = recv(fd, buf, sizeof(buf), 0);

                if (n > 0) {
                    // Append received bytes to this client's string buffer
                    std::string& buffer = client_buffers[fd];
                    buffer.append(buf, n);

                    // Process complete lines (terminated by '\n')
                    for (;;) {
                        std::size_t pos = buffer.find('\n');
                        if (pos == std::string::npos)
                            break;

                        // Extract one line (without '\n')
                        std::string line = buffer.substr(0, pos);
                        // Trim possible '\r'
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }

                        // Remove processed part from buffer
                        buffer.erase(0, pos + 1);

                        // Here you process the line (protocol logic)
                        std::cout << "Received line from fd=" << fd
                                  << ": \"" << line << "\"\n";

                        // Example: echo back (optional)
                        std::string response = "ECHO: " + line + "\n";
                        ssize_t sent = send(fd, response.data(), response.size(), 0);
                        if (sent < 0) {
                            perror("send");
                        }
                    }
                }
                else if (n == 0) {
                    // Client closed the connection
                    std::cout << "Client fd=" << fd
                              << " disconnected, removing from set\n";
                    close(fd);
                    FD_CLR(fd, &client_socks);
                    client_buffers.erase(fd);
                }
                else {
                    // recv() error
                    perror("recv");
                    std::cout << "Error on fd=" << fd
                              << ", closing and removing from set\n";
                    close(fd);
                    FD_CLR(fd, &client_socks);
                    client_buffers.erase(fd);
                }
            }
        }
    }

    // Cleanup if select() fails / server exits
    for (fd = 0; fd <= max_fd; ++fd) {
        if (FD_ISSET(fd, &client_socks)) {
            close(fd);
        }
    }

    return EXIT_FAILURE;
}