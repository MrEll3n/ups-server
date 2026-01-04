#include "Server.hpp"

#include <iostream>
#include <cstring>
#include <string>
#include <stdexcept>

namespace {
    // Keep ports within a sensible and valid TCP range.
    // 0-1023 are privileged on Unix systems; using 1024+ avoids requiring elevated privileges.
    constexpr int MIN_PORT = 1024;
    constexpr int MAX_PORT = 65535;

    int parse_port_or_throw(const std::string& s) {
        size_t idx = 0;
        int p = 0;
        try {
            p = std::stoi(s, &idx);
        } catch (const std::exception&) {
            throw std::runtime_error("Port must be a number");
        }
        if (idx != s.size()) {
            throw std::runtime_error("Port must be a number");
        }
        if (p < MIN_PORT || p > MAX_PORT) {
            throw std::runtime_error("Port must be in range " + std::to_string(MIN_PORT) + ".." + std::to_string(MAX_PORT));
        }
        return p;
    }
}

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  --ip <address>       IP address to bind (default: 0.0.0.0)\n"
              << "  --port <number>      Port to listen on (default: 10000)\n"
              << "  --no-heartbeat       Disable heartbeat mechanism\n"
              << "  --with-hb-logs       Enable verbose heartbeat logs\n";
}

int main(int argc, char** argv) {
    std::string ip_address = "0.0.0.0";
    int port = 10000;
    bool enable_heartbeat = true;
    bool heartbeat_logs = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--ip") {
            if (i + 1 < argc) {
                ip_address = argv[++i];
            } else {
                std::cerr << "[ERR] Missing value for --ip\n";
                return 1;
            }
        } else if (arg == "--port") {
            if (i + 1 < argc) {
                try {
                    port = parse_port_or_throw(argv[++i]);
                } catch (const std::exception& e) {
                    std::cerr << "[ERR] " << e.what() << "\n";
                    return 1;
                }
            } else {
                std::cerr << "[ERR] Missing value for --port\n";
                return 1;
            }
        } else if (arg == "--no-heartbeat") {
            enable_heartbeat = false;
        } else if (arg == "--with-hb-logs") {
            heartbeat_logs = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            try {
                port = parse_port_or_throw(arg);
            } catch (...) {
                std::cerr << "[ERR] Unknown argument: " << arg << "\n";
                print_usage(argv[0]);
                return 1;
            }
        }
    }

    try {
        Server server(ip_address, port, enable_heartbeat, heartbeat_logs);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[ERR] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
