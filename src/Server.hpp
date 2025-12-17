#pragma once

#include "Game.hpp"
#include "Protocol.hpp"

#include <unordered_map>
#include <string>
#include <sys/select.h>
#include <chrono>
#include <random>

class Server {
public:
    // Constructor also accepts 'host' (bind IP address)
    explicit Server(const std::string& host, int port, bool enable_heartbeat = true, bool hb_logs = false);
    void run();

private:
    int listen_fd{-1};

    fd_set master_set{};
    int max_fd{0};

    std::unordered_map<int, std::string> client_buffers;   // fd -> buffered incoming data
    std::unordered_map<int, int> fd_to_player;             // fd -> userId

    Game game;

    // --- Heartbeat ---
    struct Heartbeat {
        std::chrono::steady_clock::time_point last_ping;
        std::chrono::steady_clock::time_point last_pong;
        std::string last_nonce;
    };

    bool heartbeat_enabled{true};
    bool heartbeat_logs{false};

    std::unordered_map<int, Heartbeat> heartbeats;         // fd -> heartbeat state

    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> nonce_dist{100000, 999999};

    // --- Soft disconnect / reconnect ---
    std::unordered_map<int, std::chrono::steady_clock::time_point> disconnected_players; // userId -> disconnect time

    void init_socket(const std::string& host, int port);

    void accept_client();
    void remove_client(int fd);

    void handle_client_data(int fd);
    void handle_request(int fd, const Request& req);

    void send_line(int fd, const std::string& line);

    SessionPhase get_phase(int fd) const;
    bool is_request_allowed(SessionPhase phase, RequestType type) const;

    void notify_lobby_peers_player_left(int playerId, const std::string& reason);

    void check_disconnection_timeouts();

    int find_disconnected_player_by_name(const std::string& name);

    void disconnect_fd(int fd, const std::string& reason);

    void heartbeat_tick();
};
