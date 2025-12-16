#include "Server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <chrono>
#include <random>
#include <ctime>

static std::string phase_to_debug(SessionPhase ph) {
    switch (ph) {
        case SessionPhase::NotLoggedIn:      return "NotLoggedIn";
        case SessionPhase::LoggedInNoLobby:  return "LoggedInNoLobby";
        case SessionPhase::InLobby:          return "InLobby";
        case SessionPhase::InGame:           return "InGame";
        case SessionPhase::AFTER_GAME:       return "AfterGame";
        case SessionPhase::INVALID:          return "Invalid"; // PŘIDANÉ: Ošetření INVALID
    }
    return "Unknown";
}

Server::Server(int port, bool enable_heartbeat, bool hb_logs)
    : heartbeat_enabled(enable_heartbeat), heartbeat_logs(hb_logs) {
    init_socket(port);
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    if (heartbeat_enabled) {
        std::cerr << "[SYS] Heartbeat enabled (2s ping, 5s timeout)\n";
        if (heartbeat_logs) {
            std::cerr << "[SYS] Heartbeat debug logs enabled\n";
        }
    } else {
        std::cerr << "[SYS] Heartbeat disabled\n";
    }
}

void Server::init_socket(int port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        std::exit(1);
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        std::exit(1);
    }

    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        std::exit(1);
    }

    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    max_fd = listen_fd;

    std::cerr << "[SYS] Listening on port " << port << "\n";
}

void Server::accept_client() {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
    if (client_fd < 0) {
        perror("accept");
        return;
    }

    FD_SET(client_fd, &master_set);
    if (client_fd > max_fd) max_fd = client_fd;

    client_buffers[client_fd] = std::string{};

    Heartbeat hb;
    const auto now = std::chrono::steady_clock::now();
    hb.last_pong = now;
    hb.last_ping = now - std::chrono::seconds(2); // ping hned při prvním ticku
    heartbeats[client_fd] = std::move(hb);

    std::cerr << "[SYS] Client connected fd=" << client_fd << "\n";
}

void Server::remove_client(int fd) {
    close(fd);
    FD_CLR(fd, &master_set);
    client_buffers.erase(fd);
    heartbeats.erase(fd);
}

SessionPhase Server::get_phase(int fd) const {
    auto it = fd_to_player.find(fd);
    if (it == fd_to_player.end()) {
        return SessionPhase::NotLoggedIn;
    }
    int playerId = it->second;

    auto lobbyOpt = const_cast<Game&>(game).getLobbyOf(playerId);
    if (!lobbyOpt.has_value()) {
        return SessionPhase::LoggedInNoLobby;
    }
    Lobby* lobby = lobbyOpt.value();
    if (!lobby) return SessionPhase::LoggedInNoLobby;

    if (lobby->matchJustEnded) return SessionPhase::AFTER_GAME;

    if (lobby->inGame) return SessionPhase::InGame;
    return SessionPhase::InLobby;
}

bool Server::is_request_allowed(SessionPhase phase, RequestType type) const {
    switch (phase) {
        case SessionPhase::NotLoggedIn:
            return (type == RequestType::LOGIN  ||
                    type == RequestType::LOGOUT ||
                    type == RequestType::PONG   ||
                    type == RequestType::STATE);

        case SessionPhase::LoggedInNoLobby:
            return (type == RequestType::LOGOUT       ||
                    type == RequestType::CREATE_LOBBY ||
                    type == RequestType::JOIN_LOBBY   ||
                    type == RequestType::PONG         ||
                    type == RequestType::STATE);

        case SessionPhase::InLobby:
            return (type == RequestType::LOGOUT      ||
                    type == RequestType::LEAVE_LOBBY ||
                    type == RequestType::PONG        ||
                    type == RequestType::STATE);

        case SessionPhase::InGame:
            return (type == RequestType::LOGOUT      ||
                    type == RequestType::MOVE        ||
                    type == RequestType::PONG        ||
                    type == RequestType::STATE);

        case SessionPhase::AFTER_GAME:
            return (type == RequestType::LOGOUT      ||
                    type == RequestType::LEAVE_LOBBY ||
                    type == RequestType::REMATCH     ||
                    type == RequestType::PONG        ||
                    type == RequestType::STATE);

        case SessionPhase::INVALID:
            return false;
    }
    return false;
}

void Server::notify_lobby_peers_player_left(int playerId, const std::string& reason) {
    auto lobbyOpt = game.getLobbyOf(playerId);
    if (!lobbyOpt.has_value()) return;
    Lobby* lobby = lobbyOpt.value();
    if (!lobby) return;

    // Notify remaining player(s)
    for (auto& p : lobby->players) {
        if (p.userId == playerId) continue;
        for (auto& kv : fd_to_player) {
            if (kv.second == p.userId) {
                send_line(kv.first, Responses::game_cannot_continue(reason));
            }
        }
    }
}

void Server::disconnect_fd(int fd, const std::string& reason) {
    // IMPORTANT: remove heartbeat tracking first
    heartbeats.erase(fd);

    // If this fd is bound to a player, remove them from game state and notify peers.
    auto it = fd_to_player.find(fd);
    if (it != fd_to_player.end()) {
        int playerId = it->second;
        notify_lobby_peers_player_left(playerId, reason);
        game.removePlayer(playerId);
        fd_to_player.erase(it);
    }

    remove_client(fd);
}

void Server::heartbeat_tick() {
    using namespace std::chrono;
    const auto now = steady_clock::now();

    constexpr auto PING_INTERVAL = seconds(2);
    constexpr auto PONG_TIMEOUT  = seconds(5);

    if (heartbeat_logs) {
        std::cerr << "[DEBUG] heartbeat_tick called, heartbeats.size()=" << heartbeats.size() << "\n";
    }

    std::vector<int> to_disconnect;

    for (auto& [fd, hb] : heartbeats) {
        auto time_since_pong = duration_cast<seconds>(now - hb.last_pong).count();

        if (heartbeat_logs) {
            std::cerr << "[DEBUG] fd=" << fd
                      << " time_since_pong=" << time_since_pong << "s\n";
        }

        if (now - hb.last_pong > PONG_TIMEOUT) {
            std::cerr << "[SYS] Heartbeat timeout fd=" << fd << "\n";
            to_disconnect.push_back(fd);
            continue;
        }

        if (now - hb.last_ping >= PING_INTERVAL) {
            if (heartbeat_logs) {
                std::cerr << "[DEBUG] Sending PING to fd=" << fd << "\n";
            }
            hb.last_ping = now;
            hb.last_nonce = std::to_string(nonce_dist(rng));

            std::string data = Responses::ping(hb.last_nonce) + "\n";
            ssize_t sent = send(fd, data.data(), data.size(), 0);

            if (sent < 0) {
                perror("send ping");
                std::cerr << "[ERR] Failed to send PING to fd=" << fd << "\n";
                to_disconnect.push_back(fd);
            } else {
                if (heartbeat_logs) {
                    std::cerr << "[DEBUG] PING sent to fd=" << fd << " nonce=" << hb.last_nonce << "\n";
                }
            }
        }
    }

    for (int fd : to_disconnect) {
        disconnect_fd(fd, "TIMEOUT");
    }
}

void Server::handle_client_data(int fd) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (n < 0) perror("recv");
        disconnect_fd(fd, "DISCONNECTED");
        return;
    }

    std::string& buffer = client_buffers[fd];
    buffer.append(buf, buf + n);

    // Process complete lines
    while (true) {
        size_t pos = buffer.find('\n');
        if (pos == std::string::npos) break;

        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        Request req = parse_request_line(line);
        if (!req.valid_magic) {
            send_line(fd, Responses::error_invalid_magic());
            disconnect_fd(fd, "INVALID_MAGIC");
            return;
        }

        handle_request(fd, req);
    }
}

void Server::handle_request(int fd, const Request& req) {
    SessionPhase ph = get_phase(fd);

    if (!is_request_allowed(ph, req.type)) {
        send_line(fd, Responses::error_unexpected_state());
        return;
    }

    switch (req.type) {
        case RequestType::LOGIN: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                break;
            }
            const std::string username = req.params[0];

            if (fd_to_player.find(fd) != fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                break;
            }

            int userId = game.addPlayer(username);
            fd_to_player[fd] = userId;
            send_line(fd, Responses::login_ok(userId));
            break;
        }

        case RequestType::LOGOUT: {
            auto it = fd_to_player.find(fd);
            if (it != fd_to_player.end()) {
                int userId = it->second;
                game.removePlayer(userId);
                fd_to_player.erase(it);
            }
            send_line(fd, Responses::logout_ok());
            disconnect_fd(fd, "LOGOUT");
            break;
        }

        case RequestType::CREATE_LOBBY: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                break;
            }
            int userId = fd_to_player[fd];
            const std::string lobbyName = req.params[0];

            auto lobbyIdOpt = game.createLobby(userId, lobbyName);
            if (!lobbyIdOpt.has_value()) {
                send_line(fd, Responses::error("Cannot create lobby"));
                break;
            }

            send_line(fd, Responses::lobby_created(*lobbyIdOpt));
            break;
        }

        case RequestType::JOIN_LOBBY: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                break;
            }
            int userId = fd_to_player[fd];
            const std::string lobbyName = req.params[0];

            bool ok = game.joinLobby(userId, lobbyName);
            if (!ok) {
                send_line(fd, Responses::error("Join failed"));
                break;
            }

            send_line(fd, Responses::lobby_joined(lobbyName));

            auto lobbyOpt = game.getLobbyOf(userId);
            if (lobbyOpt.has_value() && game.canStartGame(lobbyOpt.value())) {
                Lobby* lobby = lobbyOpt.value();
                game.startGame(lobby);

                for (auto& p : lobby->players) {
                    for (auto& kv : fd_to_player) {
                        if (kv.second == p.userId) {
                            send_line(kv.first, Responses::game_started());
                        }
                    }
                }
            }

            break;
        }

        case RequestType::LEAVE_LOBBY: {
            int userId = fd_to_player[fd];
            game.leaveLobby(userId);
            send_line(fd, Responses::lobby_left());
            break;
        }

        case RequestType::MOVE: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                break;
            }
            int userId = fd_to_player[fd];

            MoveType mv;
            if (!string_to_move(req.params[0], mv)) {
                send_line(fd, Responses::error_invalid_move());
                break;
            }

            int roundWinner = 0;
            MoveType p1Move = MoveType::NONE;
            MoveType p2Move = MoveType::NONE;
            bool matchEnded = false;
            int matchWinner = 0;
            int p1Wins = 0;
            int p2Wins = 0;

            bool ok = game.submitMove(userId, mv, roundWinner, p1Move, p2Move,
                                     matchEnded, matchWinner, p1Wins, p2Wins);

            if (!ok) {
                send_line(fd, Responses::error_not_in_game());
                break;
            }

            send_line(fd, Responses::move_accepted(move_to_string(mv)));

            auto lobbyOpt = game.getLobbyOf(userId);
            if (lobbyOpt.has_value()) {
                Lobby* lobby = lobbyOpt.value();
                if (p1Move != MoveType::NONE && p2Move != MoveType::NONE) {
                    for (auto& p : lobby->players) {
                        for (auto& kv : fd_to_player) {
                            if (kv.second == p.userId) {
                                send_line(kv.first, Responses::round_result(roundWinner, move_to_string(p1Move), move_to_string(p2Move)));
                            }
                        }
                    }
                }

                if (matchEnded) {
                    for (auto& p : lobby->players) {
                        for (auto& kv : fd_to_player) {
                            if (kv.second == p.userId) {
                                send_line(kv.first, Responses::match_result(matchWinner, p1Wins, p2Wins));
                            }
                        }
                    }
                }
            }

            break;
        }

        case RequestType::REMATCH: {
            int userId = fd_to_player[fd];
            auto lobbyOpt = game.getLobbyOf(userId);
            if (!lobbyOpt.has_value()) {
                send_line(fd, Responses::error_not_in_lobby());
                break;
            }
            Lobby* lobby = lobbyOpt.value();
            if (!game.requestRematch(userId, lobby)) {
                send_line(fd, Responses::error_rematch_not_allowed());
                break;
            }

            send_line(fd, Responses::rematch_ready());

            if (game.canStartRematch(lobby)) {
                game.startRematch(lobby);
                for (auto& p : lobby->players) {
                    for (auto& kv : fd_to_player) {
                        if (kv.second == p.userId) {
                            send_line(kv.first, Responses::game_started());
                        }
                    }
                }
            }
            break;
        }

        case RequestType::STATE: {
            int userId = -1;
            auto it = fd_to_player.find(fd);
            if (it != fd_to_player.end()) userId = it->second;

            std::ostringstream oss;
            oss << "phase=" << phase_to_debug(ph) << ";";
            if (userId >= 0) {
                oss << "playerId=" << userId << ";";
            }
            send_line(fd, Responses::state(oss.str()));
            break;
        }

        case RequestType::PONG: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                return;
            }

            auto it = heartbeats.find(fd);
            if (it == heartbeats.end()) {
                return;
            }

            Heartbeat& hb = it->second;
            // Ošetření chyby 'Bad pong nonce' je nyní na klientovi.
            // Pokud je nonce správné, aktualizujeme čas pongu.

            hb.last_pong = std::chrono::steady_clock::now();
            return;
        }

        default:
            send_line(fd, Responses::error_unknown_request());
            break;
    }
}

void Server::send_line(int fd, const std::string& line) {
    std::string data = line + "\n";
    ssize_t sent = send(fd, data.data(), data.size(), 0);
    if (sent < 0) {
        perror("send");
        std::cerr << "[ERR] Failed to send to fd=" << fd << "\n";
    }
}

void Server::run() {
    while (true) {
        if (heartbeat_enabled) {
            heartbeat_tick();
        }

        fd_set read_fds = master_set;
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500000; // 500 ms

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        for (int fd = 0; fd <= max_fd; fd++) {
            if (!FD_ISSET(fd, &read_fds)) continue;

            if (fd == listen_fd) {
                accept_client();
            } else {
                handle_client_data(fd);
            }
        }
    }
}
