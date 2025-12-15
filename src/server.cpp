#include "Server.hpp"
#include "Protocol.hpp"

#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sstream>

Server::Server(int port) {
    init_socket(port);
}

Server::~Server() {
    for (auto& [fd, _] : client_buffers) {
        close(fd);
    }
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
        if (n < 0) perror("recv");
        remove_client(fd);
        return;
    }

    std::string& buffer = client_buffers[fd];
    buffer.append(buf, static_cast<std::size_t>(n));

    for (;;) {
        std::size_t pos = buffer.find('\n');
        if (pos == std::string::npos) break;

        std::string line = buffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buffer.erase(0, pos + 1);

        Request req = parse_request_line(line);

        // MAGIC CHECK
        if (!req.valid_magic) {
            std::cerr << "Invalid protocol magic from fd=" << fd
                      << ", sending error and closing connection\n";

            auto it = fd_to_player.find(fd);
            if (it != fd_to_player.end()) {
                int playerId = it->second;
                notify_lobby_peers_player_left(playerId, "INVALID_MAGIC");
                game.removePlayer(playerId);
                fd_to_player.erase(it);
            }

            send_line(fd, Responses::error_invalid_magic());
            remove_client(fd);
            return;
        }

        handle_request(fd, req);
    }
}

Server::SessionPhase Server::get_phase_for_fd(int fd) const {
    auto it = fd_to_player.find(fd);
    if (it == fd_to_player.end()) return SessionPhase::NotLoggedIn;

    int playerId = it->second;
    const Player* p = game.getPlayer(playerId);
    if (!p) return SessionPhase::NotLoggedIn;

    if (p->lobbyId < 0) return SessionPhase::LoggedInNoLobby;

    const Lobby* lobby = game.getLobby(p->lobbyId);
    if (!lobby) return SessionPhase::LoggedInNoLobby;

    switch (lobby->phase) {
        case LobbyPhase::WaitingForOpponent:
            return SessionPhase::InLobbyWaiting;
        case LobbyPhase::InGame:
            return SessionPhase::InGame;
        case LobbyPhase::AfterMatch:
            return SessionPhase::AfterMatch;
    }

    return SessionPhase::LoggedInNoLobby;
}

bool Server::is_request_allowed(SessionPhase phase, RequestType type) const {
    switch (phase) {
        case SessionPhase::NotLoggedIn:
            return (type == RequestType::LOGIN  ||
                    type == RequestType::LOGOUT ||
                    type == RequestType::PING   ||
                    type == RequestType::STATE);

        case SessionPhase::LoggedInNoLobby:
            return (type == RequestType::LOGOUT       ||
                    type == RequestType::CREATE_LOBBY ||
                    type == RequestType::JOIN_LOBBY   ||
                    type == RequestType::PING         ||
                    type == RequestType::STATE);

        case SessionPhase::InLobbyWaiting:
            return (type == RequestType::LOGOUT      ||
                    type == RequestType::LEAVE_LOBBY ||
                    type == RequestType::PING        ||
                    type == RequestType::STATE);

        case SessionPhase::InGame:
            // IMPORTANT: no REMATCH here
            return (type == RequestType::LOGOUT      ||
                    type == RequestType::MOVE        ||
                    type == RequestType::LEAVE_LOBBY ||
                    type == RequestType::PING        ||
                    type == RequestType::STATE);

        case SessionPhase::AfterMatch:
            return (type == RequestType::REMATCH     ||
                    type == RequestType::LEAVE_LOBBY ||
                    type == RequestType::LOGOUT      ||
                    type == RequestType::PING        ||
                    type == RequestType::STATE);
    }
    return false;
}

void Server::handle_request(int fd, const Request& req) {
    SessionPhase phase = get_phase_for_fd(fd);

    if (!is_request_allowed(phase, req.type)) {
        send_line(fd, Responses::error_unexpected_state());
        return;
    }

    switch (req.type) {
        case RequestType::LOGIN: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::login_fail());
                return;
            }

            int playerId = game.addPlayer(req.params[0]);
            fd_to_player[fd] = playerId;

            send_line(fd, Responses::login_ok(playerId));
            break;
        }

        case RequestType::LOGOUT: {
            auto it = fd_to_player.find(fd);
            if (it != fd_to_player.end()) {
                notify_lobby_peers_player_left(it->second, "LOGOUT");
                game.removePlayer(it->second);
                fd_to_player.erase(it);
            }
            send_line(fd, Responses::logout_ok());
            remove_client(fd);
            break;
        }

        case RequestType::CREATE_LOBBY: {
            auto it = fd_to_player.find(fd);
            if (it == fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                return;
            }
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                return;
            }

            int lobbyId = game.createLobby(it->second, req.params[0]);
            send_line(fd, Responses::lobby_created(lobbyId));
            break;
        }

        case RequestType::JOIN_LOBBY: {
            auto it = fd_to_player.find(fd);
            if (it == fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                return;
            }
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                return;
            }

            const std::string& lobbyName = req.params[0];

            bool ok = game.joinLobby(it->second, lobbyName);
            if (!ok) {
                send_line(fd, Responses::error("Lobby join failed"));
                return;
            }

            // Ack for joining client
            send_line(fd, Responses::lobby_joined(lobbyName));

            // If lobby now has 2 players and phase is InGame, notify both
            const Player* p = game.getPlayer(it->second);
            if (!p) return;

            Lobby* lobby = game.getLobby(p->lobbyId);
            if (!lobby) return;

            if (lobby->players.size() == 2 && lobby->phase == LobbyPhase::InGame) {
                for (int pid : lobby->players) {
                    int playerFd = -1;
                    for (auto& [sock, mappedId] : fd_to_player) {
                        if (mappedId == pid) { playerFd = sock; break; }
                    }
                    if (playerFd != -1) {
                        send_line(playerFd, Responses::game_started());
                    }
                }
            }
            break;
        }

        case RequestType::LEAVE_LOBBY: {
            auto it = fd_to_player.find(fd);
            if (it == fd_to_player.end()) {
                send_line(fd, Responses::error_not_in_lobby());
                return;
            }

            notify_lobby_peers_player_left(it->second, "LEAVE_LOBBY");
            game.leaveLobby(it->second);
            send_line(fd, Responses::lobby_left());
            break;
        }

        case RequestType::MOVE: {
            auto it = fd_to_player.find(fd);
            if (it == fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                return;
            }
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_invalid_move());
                return;
            }

            Player* p = game.getPlayer(it->second);
            if (!p || p->lobbyId < 0) {
                send_line(fd, Responses::error_not_in_lobby());
                return;
            }

            Lobby* lobby = game.getLobby(p->lobbyId);
            if (!lobby) {
                send_line(fd, Responses::error_not_in_lobby());
                return;
            }

            // Explicit flow guards (A)
            if (lobby->players.size() != 2) {
                send_line(fd, Responses::error("Waiting for opponent"));
                return;
            }
            if (lobby->phase != LobbyPhase::InGame) {
                send_line(fd, Responses::error("Match not in progress"));
                return;
            }
            if (p->currentMove != Move::None) {
                send_line(fd, Responses::error("Already played this round"));
                return;
            }

            Move m = parse_move(req.params[0]);
            if (m == Move::None) {
                send_line(fd, Responses::error_invalid_move());
                return;
            }

            // Ack
            send_line(fd, Responses::move_accepted(move_to_string(m)));

            // Submit to game
            auto result = game.submitMove(it->second, m);
            if (!result.has_value()) {
                // Still waiting for opponent's move
                return;
            }

            const RoundResult& rr = *result;

            // Find socket of both players
            int fd1 = -1, fd2 = -1;
            for (auto& [sock, pid] : fd_to_player) {
                if (pid == rr.p1UserId) fd1 = sock;
                if (pid == rr.p2UserId) fd2 = sock;
            }

            std::string roundMsg = Responses::round_result(
                rr.winnerUserId,
                move_to_string(rr.p1Move),
                move_to_string(rr.p2Move)
            );

            if (fd1 != -1) send_line(fd1, roundMsg);
            if (fd2 != -1 && fd2 != fd1) send_line(fd2, roundMsg);

            // Check match end (best-of-3)
            if (p->lobbyId >= 0) {
                auto matchResult = game.checkMatchEnd(p->lobbyId);
                if (matchResult.has_value()) {
                    const MatchResult& mr = *matchResult;

                    std::string matchMsg = Responses::match_result(
                        mr.winnerUserId,
                        mr.p1Wins,
                        mr.p2Wins
                    );

                    if (fd1 != -1) send_line(fd1, matchMsg);
                    if (fd2 != -1 && fd2 != fd1) send_line(fd2, matchMsg);
                }
            }

            break;
        }

        case RequestType::REMATCH: {
            auto it = fd_to_player.find(fd);
            if (it == fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                return;
            }

            // Allowed only in AfterMatch phase by is_request_allowed,
            // but we still keep a strong error response for clarity.
            Player* p = game.getPlayer(it->second);
            if (!p || p->lobbyId < 0) {
                send_line(fd, Responses::error_not_in_game());
                return;
            }

            Lobby* lobby = game.getLobby(p->lobbyId);
            if (!lobby || lobby->players.size() != 2) {
                send_line(fd, Responses::error_not_in_game());
                return;
            }

            if (lobby->phase != LobbyPhase::AfterMatch) {
                send_line(fd, Responses::error("Rematch not allowed now"));
                return;
            }

            bool bothReady = game.requestRematch(it->second);
            if (!bothReady) {
                // Optional: you can also send an info/error here, but not required.
                return;
            }

            int fd1 = -1, fd2 = -1;
            for (auto& [sock, pid] : fd_to_player) {
                if (pid == lobby->players[0]) fd1 = sock;
                if (pid == lobby->players[1]) fd2 = sock;
            }

            std::string msg = Responses::rematch_ready();
            if (fd1 != -1) send_line(fd1, msg);
            if (fd2 != -1 && fd2 != fd1) send_line(fd2, msg);

            // Many clients like to treat this as "match started again"
            // You can also emit RES_GAME_STARTED here if you want:
            // if (fd1 != -1) send_line(fd1, Responses::game_started());
            // if (fd2 != -1 && fd2 != fd1) send_line(fd2, Responses::game_started());

            break;
        }

        case RequestType::STATE: {
            SessionPhase ph = get_phase_for_fd(fd);
            std::string phaseStr;
            switch (ph) {
                case SessionPhase::NotLoggedIn:     phaseStr = "NotLoggedIn"; break;
                case SessionPhase::LoggedInNoLobby: phaseStr = "LoggedInNoLobby"; break;
                case SessionPhase::InLobbyWaiting:  phaseStr = "InLobbyWaiting"; break;
                case SessionPhase::InGame:          phaseStr = "InGame"; break;
                case SessionPhase::AfterMatch:      phaseStr = "AfterMatch"; break;
            }

            auto it = fd_to_player.find(fd);
            if (it == fd_to_player.end()) {
                send_line(fd, Responses::state("phase=" + phaseStr + ";noPlayer=1"));
                break;
            }

            int playerId = it->second;
            const Player* p = game.getPlayer(playerId);
            if (!p) {
                send_line(fd, Responses::state("phase=" + phaseStr + ";missingPlayer=1"));
                break;
            }

            std::ostringstream ss;
            ss << "phase=" << phaseStr
               << ";playerId=" << p->id
               << ";username=" << p->username
               << ";lobbyId=" << p->lobbyId;

            if (p->lobbyId >= 0) {
                const Lobby* lobby = game.getLobby(p->lobbyId);
                if (lobby) {
                    ss << ";lobbyName=" << lobby->name
                       << ";lobbyPhase=" << (lobby->phase == LobbyPhase::WaitingForOpponent ? "WaitingForOpponent" :
                                            lobby->phase == LobbyPhase::InGame ? "InGame" : "AfterMatch")
                       << ";roundsPlayed=" << lobby->roundsPlayed
                       << ";p1Wins=" << lobby->p1Wins
                       << ";p2Wins=" << lobby->p2Wins;

                    if (lobby->players.size() == 2) {
                        int p1Id = lobby->players[0];
                        int p2Id = lobby->players[1];
                        ss << ";p1Id=" << p1Id
                           << ";p2Id=" << p2Id;

                        const Player* p1 = game.getPlayer(p1Id);
                        const Player* p2 = game.getPlayer(p2Id);
                        if (p1) ss << ";p1Move=" << move_to_string(p1->currentMove);
                        if (p2) ss << ";p2Move=" << move_to_string(p2->currentMove);
                    }
                }
            }

            send_line(fd, Responses::state(ss.str()));
            break;
        }

        case RequestType::PING: {
            send_line(fd, Responses::pong());
            break;
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
        remove_client(fd);
    }
}

Move Server::parse_move(const std::string& s) {
    if (s == "R") return Move::Rock;
    if (s == "P") return Move::Paper;
    if (s == "S") return Move::Scissors;
    return Move::None;
}

std::string Server::move_to_string(Move m) {
    switch (m) {
        case Move::Rock:     return "R";
        case Move::Paper:    return "P";
        case Move::Scissors: return "S";
        default:             return "?";
    }
}

void Server::notify_lobby_peers_player_left(int playerId, const std::string& reason) {
    const Player* p = game.getPlayer(playerId);
    if (!p || p->lobbyId < 0) return;

    const Lobby* lobby = game.getLobby(p->lobbyId);
    if (!lobby) return;

    for (int otherId : lobby->players) {
        if (otherId == playerId) continue;

        int otherFd = -1;
        for (auto& [sock, pid] : fd_to_player) {
            if (pid == otherId) { otherFd = sock; break; }
        }

        if (otherFd != -1) {
            send_line(otherFd, Responses::game_cannot_continue(reason));
        }
    }
}

void Server::run() {
    while (true) {
        fd_set read_set = master_set;

        int rv = select(max_fd + 1, &read_set, nullptr, nullptr, nullptr);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (!FD_ISSET(fd, &read_set)) continue;

            if (fd == server_fd) {
                accept_client();
            } else {
                handle_client_data(fd);
            }
        }
    }
}
