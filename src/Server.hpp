#pragma once

#include <unordered_map>
#include <string>
#include <sys/select.h>

#include "Protocol.hpp"
#include "Game.hpp"

class Server {
public:
    explicit Server(int port);
    ~Server();

    void run();

private:
    int server_fd{};
    fd_set master_set{};
    int max_fd{};

    // Buffer for partial lines from clients (for assembling whole lines)
    std::unordered_map<int, std::string> client_buffers;

    // Game logic object
    Game game;

    // Map from socket fd to internal player id used by Game
    std::unordered_map<int, int> fd_to_player;

    enum class SessionPhase {
        NotLoggedIn,
        LoggedInNoLobby,
        InLobbyWaiting,
        InGame,
        AfterMatch
    };

    // Socket / IO handling
    void init_socket(int port);
    void accept_client();
    void remove_client(int fd);
    void handle_client_data(int fd);
    void handle_request(int fd, const Request& req);

    // Networking helper
    void send_line(int fd, const std::string& line);

    // Helpers for move parsing/formatting
    Move parse_move(const std::string& s);
    std::string move_to_string(Move m);

    // DFA helpers
    SessionPhase get_phase_for_fd(int fd) const;
    bool is_request_allowed(SessionPhase phase, RequestType type) const;

    void notify_lobby_peers_player_left(int playerId, const std::string& reason);
};
