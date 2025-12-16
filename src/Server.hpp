#pragma once

#include "Game.hpp"
#include "Protocol.hpp"

#include <unordered_map>
#include <string>
#include <sys/select.h>
#include <chrono>
#include <random>

enum class SessionPhase {
    NotLoggedIn,
    LoggedInNoLobby,
    InLobby,
    InGame
};

class Server {
public:
    explicit Server(int port, bool enable_heartbeat = true, bool hb_logs = false);
    void run();

private:
    int listen_fd{-1};
    fd_set master_set{};
    int max_fd{0};

    Game game;

    std::unordered_map<int, std::string> client_buffers; // fd -> buffered data

    // Map from socket fd to internal player id used by Game
    std::unordered_map<int, int> fd_to_player;

    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> nonce_dist{0, 999999};

    bool heartbeat_enabled{true};
    bool heartbeat_logs{false};

    // --- Heartbeat (server->client ping, client->server pong) ---
    struct Heartbeat {
        std::chrono::steady_clock::time_point last_pong{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point last_ping{std::chrono::steady_clock::now()};
        std::string last_nonce{};
    };
    std::unordered_map<int, Heartbeat> heartbeats;

    void init_socket(int port);
    void accept_client();
    void remove_client(int fd);

    void handle_client_data(int fd);
    void handle_request(int fd, const Request& req);

    void send_line(int fd, const std::string& line);

    SessionPhase get_phase(int fd) const;
    bool is_request_allowed(SessionPhase phase, RequestType type) const;

    void notify_lobby_peers_player_left(int playerId, const std::string& reason);

    // Unified cleanup path for unexpected disconnects / timeouts
    void disconnect_fd(int fd, const std::string& reason);

    void heartbeat_tick();
};
