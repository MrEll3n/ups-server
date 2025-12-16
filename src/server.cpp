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
#include <map>
#include <set>

// Pomocná funkce pro ladění stavů
static std::string phase_to_debug(SessionPhase ph) {
    switch (ph) {
        case SessionPhase::NotLoggedIn:      return "NotLoggedIn";
        case SessionPhase::LoggedInNoLobby:  return "LoggedInNoLobby";
        case SessionPhase::InLobby:          return "InLobby";
        case SessionPhase::InGame:           return "InGame";
        case SessionPhase::AFTER_GAME:       return "AfterGame";
        case SessionPhase::INVALID:          return "Invalid";
    }
    return "Unknown";
}

// Globální registry pro unikátnost jmen
static std::map<int, std::string> g_online_users;
static std::set<std::string> g_active_lobbies;

// Funkce pro uvolnění jména lobby, pokud zaniká
void try_free_lobby_name(Game& game, int userId) {
    auto lobbyOpt = game.getLobbyOf(userId);
    if (lobbyOpt.has_value()) {
        Lobby* lobby = lobbyOpt.value();
        // Pokud je hráč poslední v lobby (size <= 1), lobby zanikne -> uvolníme jméno
        if (lobby->players.size() <= 1) {
            g_active_lobbies.erase(lobby->name);
            std::cerr << "[SYS] Lobby '" << lobby->name << "' is empty and destroyed. Name released.\n";
        }
    }
}

// Konstruktor
Server::Server(const std::string& host, int port, bool enable_heartbeat, bool hb_logs)
    : heartbeat_enabled(enable_heartbeat), heartbeat_logs(hb_logs) {

    init_socket(host, port);

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

// Inicializace socketu s IP a Portem
void Server::init_socket(const std::string& host, int port) {
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

    // Převod stringu IP adresy na binární formu
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[ERR] Invalid address/ Address not supported: " << host << "\n";
        std::exit(1);
    }

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

    std::cerr << "[SYS] Listening on " << host << ":" << port << "\n";
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
    hb.last_ping = now - std::chrono::seconds(2);
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
                    type == RequestType::LEAVE_LOBBY ||
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

    std::vector<int> peerIds;
    for (auto& p : lobby->players) {
        if (p.userId == playerId) continue;
        peerIds.push_back(p.userId);
    }

    for (int peerId : peerIds) {
        for (auto& kv : fd_to_player) {
            if (kv.second == peerId) {
                send_line(kv.first, Responses::game_cannot_continue(reason));
                send_line(kv.first, Responses::lobby_left());
            }
        }

        // Zkusit uvolnit jméno lobby, pokud peer byl poslední
        try_free_lobby_name(game, peerId);
        game.leaveLobby(peerId);
    }
}

void Server::disconnect_fd(int fd, const std::string& reason) {
    heartbeats.erase(fd);
    client_buffers.erase(fd);

    auto it = fd_to_player.find(fd);
    if (it != fd_to_player.end()) {
        int userId = it->second;

        // Uvolnit jméno hráče
        g_online_users.erase(userId);

        auto lobbyOpt = game.getLobbyOf(userId);
        if (lobbyOpt.has_value() && lobbyOpt.value()->inGame) {
            // SOFT DISCONNECT (ve hře)
            std::cerr << "[SYS] User " << userId << " disconnected during game. Waiting for reconnect...\n";
            disconnected_players[userId] = std::chrono::steady_clock::now();

            Lobby* lobby = lobbyOpt.value();
            for (auto& p : lobby->players) {
                if (p.userId == userId) continue;
                for (auto& kv : fd_to_player) {
                    if (kv.second == p.userId) {
                        send_line(kv.first, Responses::opponent_disconnected(15));
                    }
                }
            }
        } else {
            // HARD DISCONNECT (v lobby/menu)
            try_free_lobby_name(game, userId);
            game.removePlayer(userId);
        }

        fd_to_player.erase(it);
    }

    close(fd);
    FD_CLR(fd, &master_set);
}

void Server::check_disconnection_timeouts() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    std::vector<int> timed_out_users;

    for (auto& kv : disconnected_players) {
        int userId = kv.first;
        auto disconnect_time = kv.second;

        if (duration_cast<seconds>(now - disconnect_time).count() > 15) {
            std::cerr << "[SYS] Reconnect timeout for user " << userId << ". Ending match.\n";

            notify_lobby_peers_player_left(userId, "Opponent timed out");
            try_free_lobby_name(game, userId);

            // Pojistka: vyhodit z lobby a odstranit hráče
            game.leaveLobby(userId);
            game.removePlayer(userId);

            timed_out_users.push_back(userId);
        }
    }

    for (int uid : timed_out_users) {
        disconnected_players.erase(uid);
    }
}

int Server::find_disconnected_player_by_name(const std::string& name) {
    for (auto& kv : disconnected_players) {
        int uid = kv.first;
        auto lobbyOpt = game.getLobbyOf(uid);
        if (lobbyOpt.has_value()) {
            Lobby* lobby = lobbyOpt.value();
            for (auto& p : lobby->players) {
                if (p.userId == uid && p.username == name) {
                    return uid;
                }
            }
        }
    }
    return -1;
}

void Server::heartbeat_tick() {
    using namespace std::chrono;
    const auto now = steady_clock::now();

    constexpr auto PING_INTERVAL = seconds(2);
    constexpr auto PONG_TIMEOUT  = seconds(5);

    std::vector<int> to_disconnect;

    for (auto& [fd, hb] : heartbeats) {
        if (now - hb.last_pong > PONG_TIMEOUT) {
            std::cerr << "[SYS] Heartbeat timeout fd=" << fd << "\n";
            to_disconnect.push_back(fd);
            continue;
        }

        if (now - hb.last_ping >= PING_INTERVAL) {
            hb.last_ping = now;
            hb.last_nonce = std::to_string(nonce_dist(rng));
            std::string data = Responses::ping(hb.last_nonce) + "\n";
            if (send(fd, data.data(), data.size(), 0) < 0) {
                 to_disconnect.push_back(fd);
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
        disconnect_fd(fd, "DISCONNECTED");
        return;
    }

    std::string& buffer = client_buffers[fd];
    buffer.append(buf, buf + n);

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
        // Ošetření dvojitého Rematch, aby nepadal na Error
        if (req.type == RequestType::REMATCH && ph == SessionPhase::InGame) {
             send_line(fd, Responses::error("Game already started"));
             return;
        }
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

            // 1. Kontrola duplicit
            bool nameTaken = false;
            for (const auto& kv : g_online_users) {
                if (kv.second == username) { nameTaken = true; break; }
            }
            if (nameTaken) {
                send_line(fd, Responses::error("Name already in use"));
                break;
            }

            // 2. Reconnect
            int oldUserId = find_disconnected_player_by_name(username);
            if (oldUserId != -1) {
                std::cerr << "[SYS] User " << username << " reconnected (ID: " << oldUserId << ")\n";
                fd_to_player[fd] = oldUserId;
                disconnected_players.erase(oldUserId);
                g_online_users[oldUserId] = username;

                send_line(fd, Responses::login_ok(oldUserId));
                send_line(fd, Responses::game_started());

                auto lobbyOpt = game.getLobbyOf(oldUserId);
                if (lobbyOpt.has_value()) {
                    Lobby* lobby = lobbyOpt.value();
                    lobby->p1Move = MoveType::NONE;
                    lobby->p2Move = MoveType::NONE;
                    for (auto& p : lobby->players) {
                        if (p.userId == oldUserId) continue;
                        for (auto& kv : fd_to_player) {
                            if (kv.second == p.userId) send_line(kv.first, Responses::game_resumed());
                        }
                    }
                }
                break;
            }

            // 3. Nový hráč
            if (fd_to_player.find(fd) != fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                break;
            }
            int userId = game.addPlayer(username);
            fd_to_player[fd] = userId;
            g_online_users[userId] = username;
            send_line(fd, Responses::login_ok(userId));
            break;
        }

        case RequestType::LOGOUT: {
            auto it = fd_to_player.find(fd);
            if (it != fd_to_player.end()) {
                int userId = it->second;
                try_free_lobby_name(game, userId);
                game.leaveLobby(userId);
                game.removePlayer(userId);
                // NEVYMAZÁVAT fd_to_player ZDE! Nechat na disconnect_fd.
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
            std::string lobbyName = req.params[0];

            if (g_active_lobbies.find(lobbyName) != g_active_lobbies.end()) {
                send_line(fd, Responses::error("Lobby name already taken"));
                break;
            }

            auto lobbyIdOpt = game.createLobby(userId, lobbyName);
            if (!lobbyIdOpt.has_value()) {
                send_line(fd, Responses::error("Cannot create lobby"));
                break;
            }
            g_active_lobbies.insert(lobbyName);
            send_line(fd, Responses::lobby_created(*lobbyIdOpt));
            break;
        }

        case RequestType::JOIN_LOBBY: {
            if (req.params.size() != 1) {
                send_line(fd, Responses::error_malformed_request());
                break;
            }
            int userId = fd_to_player[fd];
            std::string lobbyName = req.params[0];

            if (!game.joinLobby(userId, lobbyName)) {
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
                        if (kv.second == p.userId) send_line(kv.first, Responses::game_started());
                    }
                }
            }
            break;
        }

        case RequestType::LEAVE_LOBBY: {
            int userId = fd_to_player[fd];
            notify_lobby_peers_player_left(userId, "Opponent left the lobby");
            try_free_lobby_name(game, userId);
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

            int rw = 0, mw = 0, p1w = 0, p2w = 0;
            // !!! Fix: Inicializace na NONE !!!
            MoveType m1 = MoveType::NONE;
            MoveType m2 = MoveType::NONE;
            bool me = false;

            if (!game.submitMove(userId, mv, rw, m1, m2, me, mw, p1w, p2w)) {
                send_line(fd, Responses::error_not_in_game());
                break;
            }
            send_line(fd, Responses::move_accepted(move_to_string(mv)));

            auto lobbyOpt = game.getLobbyOf(userId);
            if (lobbyOpt.has_value()) {
                Lobby* lobby = lobbyOpt.value();
                if (m1 != MoveType::NONE && m2 != MoveType::NONE) {
                    for (auto& p : lobby->players) {
                        for (auto& kv : fd_to_player) {
                            if (kv.second == p.userId)
                                send_line(kv.first, Responses::round_result(rw, move_to_string(m1), move_to_string(m2)));
                        }
                    }
                }
                if (me) {
                    for (auto& p : lobby->players) {
                        for (auto& kv : fd_to_player) {
                            if (kv.second == p.userId)
                                send_line(kv.first, Responses::match_result(mw, p1w, p2w));
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
                        if (kv.second == p.userId) send_line(kv.first, Responses::game_started());
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
            if (userId >= 0) oss << "playerId=" << userId << ";";
            send_line(fd, Responses::state(oss.str()));
            break;
        }

        case RequestType::PONG: {
            auto it = heartbeats.find(fd);
            if (it != heartbeats.end()) it->second.last_pong = std::chrono::steady_clock::now();
            break;
        }

        default:
            send_line(fd, Responses::error_unknown_request());
            break;
    }
}

void Server::send_line(int fd, const std::string& line) {
    std::string data = line + "\n";
    if (send(fd, data.data(), data.size(), 0) < 0) {
        perror("send");
    }
}

void Server::run() {
    while (true) {
        if (heartbeat_enabled) heartbeat_tick();
        check_disconnection_timeouts();

        fd_set read_fds = master_set;
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        for (int fd = 0; fd <= max_fd; fd++) {
            if (FD_ISSET(fd, &read_fds)) {
                if (fd == listen_fd) accept_client();
                else handle_client_data(fd);
            }
        }
    }
}
