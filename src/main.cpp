#include "Server.hpp"

#include <iostream>
#include <cstring>

int main(int argc, char** argv) {
    int port = 10000;
    bool enable_heartbeat = true;
    bool heartbeat_logs = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-heartbeat") == 0) {
            enable_heartbeat = false;
        } else if (strcmp(argv[i], "--with-hb-logs") == 0) {
            heartbeat_logs = true;
        } else {
            port = std::stoi(argv[i]);
        }
    }

    try {
        Server server(port, enable_heartbeat, heartbeat_logs);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[ERR] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
