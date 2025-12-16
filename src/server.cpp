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

static std::map<int, std::string> g_online_users;

static std::set<std::string> g_active_lobbies;

void try_free_lobby_name(Game& game, int userId) {
    auto lobbyOpt = game.getLobbyOf(userId);
    if (lobbyOpt.has_value()) {
        Lobby* lobby = lobbyOpt.value();
        // Pokud je hráč poslední v lobby (size == 1), lobby po jeho odchodu zanikne.
        // Musíme tedy uvolnit její jméno.
        if (lobby->players.size() <= 1) {
            // Předpokládáme, že struktura Lobby má atribut 'name'.
            // Většinou se to jmenuje 'name' nebo 'lobbyName'.
            // Pokud vám to zde hodí chybu, podívejte se do Game.hpp, jak se ta proměnná jmenuje.
            g_active_lobbies.erase(lobby->name);
            std::cerr << "[SYS] Lobby '" << lobby->name << "' is empty and destroyed. Name released.\n";
        }
    }
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

    // 1. Najdeme všechny OSTATNÍ hráče v lobby (soupeře)
    std::vector<int> peerIds;
    for (auto& p : lobby->players) {
        if (p.userId == playerId) continue;
        peerIds.push_back(p.userId);
    }

    // 2. Projdeme nalezené soupeře a vyhodíme je
    for (int peerId : peerIds) {
        // A) Pošleme jim zprávy přes socket
        for (auto& kv : fd_to_player) {
            if (kv.second == peerId) {
                // Informujeme, proč hra končí
                send_line(kv.first, Responses::game_cannot_continue(reason));
                // Informujeme, že je server vyhazuje z lobby
                send_line(kv.first, Responses::lobby_left());
            }
        }

        // B) !!! DŮLEŽITÉ !!!
        // Zkusíme uvolnit název lobby.
        // Pokud je tento 'peer' posledním hráčem v lobby, lobby po jeho odchodu zanikne.
        // Proto musíme jméno lobby smazat z 'g_active_lobbies' ještě předtím, než ho 'leaveLobby' zničí.
        try_free_lobby_name(game, peerId);

        // C) Vyhodíme hráče z lobby v herní logice
        game.leaveLobby(peerId);
    }
}

void Server::disconnect_fd(int fd, const std::string& reason) {
    // 1. Vyčistit buffery a heartbeats
    heartbeats.erase(fd);
    client_buffers.erase(fd);

    auto it = fd_to_player.find(fd);
    if (it != fd_to_player.end()) {
        int userId = it->second;

        // 2. Hráč je offline -> smazat ze seznamu aktivních jmen
        // (Aby se toto jméno dalo hned použít pro Reconnect nebo Login)
        g_online_users.erase(userId);

        // 3. Zjistíme, jestli je hráč ve hře
        auto lobbyOpt = game.getLobbyOf(userId);

        if (lobbyOpt.has_value() && lobbyOpt.value()->inGame) {
            // --- VARIANTA A: HRÁČ JE VE HŘE (SOFT DISCONNECT) ---
            // Nerušíme lobby, neuvolňujeme jméno lobby, jen pozastavíme.

            std::cerr << "[SYS] User " << userId << " disconnected during game. Waiting for reconnect...\n";

            // Uložíme čas odpojení pro 15s timeout
            disconnected_players[userId] = std::chrono::steady_clock::now();

            // Informujeme soupeře, že čekáme
            Lobby* lobby = lobbyOpt.value();
            for (auto& p : lobby->players) {
                if (p.userId == userId) continue;
                // Najdeme FD soupeře
                for (auto& kv : fd_to_player) {
                    if (kv.second == p.userId) {
                        send_line(kv.first, Responses::opponent_disconnected(15));
                    }
                }
            }
        } else {
            // --- VARIANTA B: HRÁČ NENÍ VE HŘE (HARD DISCONNECT) ---
            // Je v menu nebo v lobby (ale nehraje).

            // !!! DŮLEŽITÉ: Pokud je v lobby, zkusíme uvolnit jméno lobby !!!
            // (Pokud tam byl poslední, lobby zanikne a název musí být volný)
            try_free_lobby_name(game, userId);

            // Úplně odstraníme hráče ze hry
            game.removePlayer(userId);
        }

        // Smazat mapování FD -> Player
        fd_to_player.erase(it);
    }

    // 4. Zavřít socket
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

        // Pokud uběhlo více než 15 sekund od odpojení
        if (duration_cast<seconds>(now - disconnect_time).count() > 15) {
            std::cerr << "[SYS] Reconnect timeout for user " << userId << ". Ending match.\n";

            // 1. Informovat soupeře (že vyhrál kontumačně) a vyhodit ho z lobby
            // (Soupeř dostane zprávu RES_LOBBY_LEFT a přepne se do menu)
            notify_lobby_peers_player_left(userId, "Opponent timed out");

            // 2. Uvolnit název lobby (protože v ní zůstal už jen tento odpojený hráč)
            try_free_lobby_name(game, userId);

            // 3. !!! POJISTKA: Vyhodit odpojeného hráče z lobby !!!
            // Tím zajistíme, že v systému nezůstane viset vazba "Hráč <-> Lobby".
            game.leaveLobby(userId);

            // 4. Smazat hráče z paměti serveru
            // Tím se z něj stane "neznámý". Pokud se znovu připojí, bude to nový login.
            game.removePlayer(userId);

            timed_out_users.push_back(userId);
        }
    }

    // Vyčistit seznam timeoutů
    for (int uid : timed_out_users) {
        disconnected_players.erase(uid);
    }
}

int Server::find_disconnected_player_by_name(const std::string& name) {
    // Projdeme všechny hráče, kteří čekají na reconnect
    for (auto& kv : disconnected_players) {
        int uid = kv.first;

        // Protože v 'disconnected_players' máme jen ID a Čas,
        // musíme se podívat do objektu 'game', abychom zjistili Jméno.
        // Hráči v tomto seznamu jsou vždy "InGame", takže musí být v nějakém lobby.
        auto lobbyOpt = game.getLobbyOf(uid);

        if (lobbyOpt.has_value()) {
            Lobby* lobby = lobbyOpt.value();
            // Prohledáme hráče v lobby
            for (auto& p : lobby->players) {
                // Pokud ID sedí a Jméno sedí, našli jsme ho!
                if (p.userId == uid && p.username == name) {
                    return uid;
                }
            }
        }
    }

    // Žádný odpojený hráč s tímto jménem nebyl nalezen
    return -1;
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

            // --- 1. KONTROLA DUPLICITNÍHO JMÉNA (NOVÉ) ---
            bool nameTaken = false;
            for (const auto& kv : g_online_users) {
                if (kv.second == username) {
                    nameTaken = true;
                    break;
                }
            }

            if (nameTaken) {
                // Jméno už je online na jiném socketu
                send_line(fd, Responses::error("Name already in use"));
                // Můžeme poslat i LOGIN_FAIL, pokud to klient podporuje lépe
                // send_line(fd, "MRLLN|RES_LOGIN_FAIL|");
                break;
            }
            // ----------------------------------------------

            // --- 2. POKUS O RECONNECT ---
            int oldUserId = find_disconnected_player_by_name(username);

            if (oldUserId != -1) {
                std::cerr << "[SYS] User " << username << " reconnected (ID: " << oldUserId << ")\n";

                fd_to_player[fd] = oldUserId;
                disconnected_players.erase(oldUserId);

                // ZAPAMATOVAT SI, ŽE JE ZASE ONLINE
                g_online_users[oldUserId] = username;

                send_line(fd, Responses::login_ok(oldUserId));
                send_line(fd, Responses::game_started());

                auto lobbyOpt = game.getLobbyOf(oldUserId);
                if (lobbyOpt.has_value()) {
                    Lobby* lobby = lobbyOpt.value();
                    // Reset tahů (jak jsme dělali minule)
                    lobby->p1Move = MoveType::NONE;
                    lobby->p2Move = MoveType::NONE;

                    for (auto& p : lobby->players) {
                        if (p.userId == oldUserId) continue;
                        for (auto& kv : fd_to_player) {
                            if (kv.second == p.userId) {
                                send_line(kv.first, Responses::game_resumed());
                            }
                        }
                    }
                }
                break;
            }

            // --- 3. NOVÝ HRÁČ ---
            if (fd_to_player.find(fd) != fd_to_player.end()) {
                send_line(fd, Responses::error_unexpected_state());
                break;
            }

            int userId = game.addPlayer(username);
            fd_to_player[fd] = userId;

            // ZAPAMATOVAT SI ONLINE JMÉNO
            g_online_users[userId] = username;

            send_line(fd, Responses::login_ok(userId));
            break;
        }

        case RequestType::LOGOUT: {
            auto it = fd_to_player.find(fd);
            if (it != fd_to_player.end()) {
                int userId = it->second;

                // 1. Zkusíme uvolnit název lobby (pokud tímto odchodem zaniká)
                try_free_lobby_name(game, userId);

                // 2. Vyhodíme hráče ze hry explicitně
                // (Aby disconnect_fd níže vyhodnotil situaci jako "Hard Disconnect"
                // a nesnažil se ho udržet ve hře pro reconnect)
                game.leaveLobby(userId);
                game.removePlayer(userId);

                // !!! DŮLEŽITÉ !!!
                // Zde NESMÍME volat 'fd_to_player.erase(it)'.
                // Musíme to mapování nechat existovat ještě chvilku,
                // aby si ho mohla přečíst funkce 'disconnect_fd' a podle ID
                // smazat jméno z 'g_online_users'.
            }

            // Potvrdíme klientovi
            send_line(fd, Responses::logout_ok());

            // Odpojíme socket -> TATO funkce se postará o vymazání g_online_users i fd_to_player
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

            // --- 1. KONTROLA DUPLICITNÍHO NÁZVU LOBBY ---
            if (g_active_lobbies.find(lobbyName) != g_active_lobbies.end()) {
                send_line(fd, Responses::error("Lobby name already taken"));
                break;
            }
            // ---------------------------------------------

            auto lobbyIdOpt = game.createLobby(userId, lobbyName);
            if (!lobbyIdOpt.has_value()) {
                send_line(fd, Responses::error("Cannot create lobby"));
                break;
            }

            // --- 2. ZAREGISTROVAT JMÉNO LOBBY ---
            g_active_lobbies.insert(lobbyName);
            // ------------------------------------

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

            // 1. Informovat soupeře (pokud tam je)
            notify_lobby_peers_player_left(userId, "Opponent left the lobby");

            // --- 2. POKUSIT SE UVOLNIT JMÉNO LOBBY (pokud zaniká) ---
            try_free_lobby_name(game, userId);
            // --------------------------------------------------------

            // 3. Opustit lobby
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
        // 1. Heartbeat tick (odesílání pingů)
        if (heartbeat_enabled) {
            heartbeat_tick();
        }

        // 2. KONTROLA ODPOJENÝCH HRÁČŮ - TOTO JE KLÍČOVÉ PRO UKONČENÍ HRY PO 15s
        // Bez tohoto volání server nikdy neví, že čas vypršel.
        check_disconnection_timeouts();

        fd_set read_fds = master_set;
        timeval tv{};
        // Důležité: Timeout selectu musí být krátký (např. 500ms),
        // aby se smyčka točila a check_disconnection_timeouts se volalo často.
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
